/*
 * Copyright (c) 2026, The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// COV03: end-to-end coverage for the custom URL scheme handlers under
// a real offscreen WebEngine — arora-file:// directory listings
// (FileAccessHandler), the file:// directory redirect in WebPage,
// arora-resource:// bundled adblock stubs (AdBlockResourceHandler) and
// abp:subscribe links (AdBlockSchemeAccessHandler).  Custom schemes
// must be registered before BrowserApplication brings up the browsing
// profile, so this test uses a custom main() instead of QTEST_MAIN.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <QtGui/QtGui>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebengineloadinginfo.h>
#include <qabstractbutton.h>
#include <qapplication.h>
#include <qmessagebox.h>
#include <qtimer.h>

#include <memory>

#include "schemeaccesshandler.h"
#include "fileaccesshandler.h"
#include "adblockschemeaccesshandler.h"
#include "adblockresourcehandler.h"
#include "adblockmanager.h"
#include "adblocksubscription.h"
#include "adblockdialog.h"
#include "webpage.h"
#include "browserapplication.h"
#include "qtry.h"

// Clicks the requested button on any modal QMessageBox that pops while
// this object is alive — the abp:subscribe flow prompts through
// QMessageBox::question, which exec()s a real modal dialog even under
// the offscreen QPA.
class ModalAnswer : public QObject
{
public:
    ModalAnswer(QMessageBox::StandardButton button, QObject *parent = 0)
        : QObject(parent)
        , m_button(button)
    {
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() {
            QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!box)
                return;
            QAbstractButton *button = box->button(m_button);
            if (button)
                button->click();
            else
                box->reject();
            ++m_answered;
        });
        timer->start(50);
    }

    int answered() const { return m_answered; }

private:
    QMessageBox::StandardButton m_button;
    int m_answered = 0;
};

// Spins the event loop until flag flips or the deadline passes —
// QTest's QTRY_* macros can't live in helpers that return a value.
static bool waitFor(const std::shared_ptr<bool> &flag, int timeout = 15000)
{
    for (int waited = 0; !*flag && waited < timeout; waited += 50)
        QTest::qWait(50);
    return *flag;
}

// Loads url on the page and waits for the next loadFinished; returns
// the success flag.  The signal state is heap-shared so a late signal
// cannot write into dead stack frames.
static bool loadSync(QWebEnginePage *page, const QUrl &url)
{
    std::shared_ptr<bool> done(new bool(false));
    std::shared_ptr<bool> ok(new bool(false));
    QMetaObject::Connection connection = QObject::connect(
        page, &QWebEnginePage::loadFinished, page,
        [done, ok](bool result) { *done = true; *ok = result; });
    page->load(url);
    waitFor(done);
    QObject::disconnect(connection);
    return *ok;
}

static QString pageHtml(QWebEnginePage *page)
{
    std::shared_ptr<bool> done(new bool(false));
    std::shared_ptr<QString> html(new QString);
    page->toHtml([done, html](const QString &result) {
        *html = result;
        *done = true;
    });
    waitFor(done);
    return *html;
}

// Waits until the load reports LoadFailedStatus on loadingChanged.
// loadFinished is unreliable here: WebPage replaces a failed
// navigation with its own not-found page, whose loadFinished(true)
// can be the only result the test sees.
static bool loadAndFail(QWebEnginePage *page, const QUrl &url)
{
    std::shared_ptr<bool> failed(new bool(false));
    QMetaObject::Connection connection = QObject::connect(
        page, &QWebEnginePage::loadingChanged, page,
        [failed](const QWebEngineLoadingInfo &info) {
            if (info.status() == QWebEngineLoadingInfo::LoadFailedStatus)
                *failed = true;
        });
    page->load(url);
    const bool seen = waitFor(failed);
    QObject::disconnect(connection);
    return seen;
}

class tst_SchemeHandlers : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void resourceNames_data();
    void resourceNames();
    void stubResources();
    void fileSchemeListing();
    void fileSchemeRedirect();
    void fileSchemeErrors();
    void fileSchemeRemoteInitiator();
    void resourceScheme();
    void abpSchemeInvalid();
    void abpSchemeSubscribeDeclined();
    void abpSchemeSubscribeAccepted();

private:
    QWebEngineProfile *m_profile;
};

void tst_SchemeHandlers::initTestCase()
{
    QCoreApplication::setApplicationName("tst_schemehandlers");

    QSettings settings;
    settings.clear();
    // Point the stored subscription list at a dead local file so the
    // manager never reaches for live lists (an empty QStringList can
    // round-trip through QSettings as invalid and fall back to the
    // defaults).
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-cov03.txt"
            "&title=DeadList"));

    // A throwaway off-the-record profile keeps the handlers away from
    // the real browsing profile.
    m_profile = new QWebEngineProfile(this);
    SchemeAccessHandler::installAll(m_profile, this);
    AdBlockManager::instance()->installOnProfile(m_profile);
}

void tst_SchemeHandlers::cleanup()
{
    // Drop whatever subscribe-accept tests added.
    AdBlockManager *manager = AdBlockManager::instance();
    const QList<AdBlockSubscription *> subscriptions = manager->subscriptions();
    for (AdBlockSubscription *subscription : subscriptions)
        manager->removeSubscription(subscription);
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (AdBlockDialog *dialog = qobject_cast<AdBlockDialog*>(widget))
            dialog->close();
    }
}

void tst_SchemeHandlers::resourceNames_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("canonical");

    QTest::newRow("empty") << QString() << QByteArray();
    QTest::newRow("blank") << QString("   ") << QByteArray();
    QTest::newRow("self") << QString("noop.js") << QByteArray("noop.js");
    QTest::newRow("alias") << QString("1x1-transparent-gif") << QByteArray("1x1.gif");
    QTest::newRow("alias2") << QString("32x32-transparent-png") << QByteArray("32x32.png");
    QTest::newRow("priority") << QString("noop.js:47") << QByteArray("noop.js");
    QTest::newRow("priority-nonnum") << QString("noop.js:xx") << QByteArray();
    QTest::newRow("ext-js") << QString("some-tracker.js") << QByteArray("noop.js");
    QTest::newRow("ext-gif") << QString("pixel.gif") << QByteArray("1x1.gif");
    QTest::newRow("ext-png") << QString("pixel.png") << QByteArray("2x2.png");
    QTest::newRow("ext-html") << QString("frame.html") << QByteArray("noop.html");
    QTest::newRow("ext-css") << QString("sheet.css") << QByteArray("noop.css");
    QTest::newRow("ext-txt") << QString("note.txt") << QByteArray("noop.txt");
    QTest::newRow("unknown") << QString("binary.bin") << QByteArray();
}

void tst_SchemeHandlers::resourceNames()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, canonical);
    QCOMPARE(AdBlockResourceHandler::canonicalResourceName(name), canonical);

    QCOMPARE(AdBlockResourceHandler::schemeName(),
             QByteArrayLiteral("arora-resource"));

    FileAccessHandler fileHandler;
    QCOMPARE(fileHandler.scheme(), QByteArrayLiteral("arora-file"));
    AdBlockSchemeAccessHandler abpHandler;
    QCOMPARE(abpHandler.scheme(), QByteArrayLiteral("abp"));
}

// The bundled stub table: every registered name resolves to a real
// resource, aliases canonicalize first, unknown names do not.
void tst_SchemeHandlers::stubResources()
{
    const QList<QByteArray> names = AdBlockResourceHandler::registrationNames();
    QVERIFY(names.count() > 10);
    for (const QByteArray &name : names) {
        const QByteArray canonical =
            AdBlockResourceHandler::canonicalResourceName(
                QString::fromUtf8(name));
        QCOMPARE(canonical.isEmpty(), false);
        QByteArray mimeType, body;
        QVERIFY(AdBlockResourceHandler::resourceFor(canonical, &mimeType, &body));
        QVERIFY2(!mimeType.isEmpty(), name.constData());
        QVERIFY2(!body.isEmpty(), name.constData());
    }

    QByteArray mimeType, body;
    QVERIFY(!AdBlockResourceHandler::resourceFor("no-such-stub", &mimeType, &body));

    QCOMPARE(AdBlockResourceHandler::urlForResource("noop.js"),
             QUrl(QLatin1String("arora-resource:/noop.js")));
}

// arora-file:// renders the app's own directory listing.
void tst_SchemeHandlers::fileSchemeListing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.filePath(QLatin1String("cov03file.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    {
        QFile file(dir.filePath(QLatin1String(".cov03hidden")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QVERIFY(QDir(dir.path()).mkdir(QLatin1String("cov03dir")));

    WebPage page(m_profile);
    const QUrl url = FileAccessHandler::urlForLocalPath(dir.path());
    QCOMPARE(url.scheme(), QLatin1String("arora-file"));
    QVERIFY(loadSync(&page, url));

    const QString html = pageHtml(&page);
    QVERIFY(html.contains(QLatin1String("cov03file.txt")));
    QVERIFY(html.contains(QLatin1String("cov03dir")));
    QVERIFY(html.contains(QLatin1String(".."))); // parent directory row
    QVERIFY(html.contains(QLatin1String("Show Hidden Files")));

    // SEC02: a listing page may still navigate into a child directory —
    // an arora-file initiator is local and allowed.
    std::shared_ptr<bool> navDone(new bool(false));
    QObject::connect(&page, &QWebEnginePage::loadFinished, &page,
                     [navDone](bool) { *navDone = true; });
    page.runJavaScript(QStringLiteral(
        "location.href='arora-file:%1/cov03dir'").arg(dir.path()));
    QVERIFY(waitFor(navDone));
    QVERIFY(pageHtml(&page).contains(QLatin1String("Contents of")));
}

// Navigating to a file:// directory bounces to arora-file:// inside
// WebPage::acceptNavigationRequest so the handler can list it.
void tst_SchemeHandlers::fileSchemeRedirect()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.filePath(QLatin1String("redirected.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    WebPage page(m_profile);
    std::shared_ptr<bool> done(new bool(false));
    std::shared_ptr<bool> ok(new bool(false));
    // The rejected file:// navigation may emit loadFinished(false)
    // before the redirected arora-file load; only the arora-file
    // result counts.
    QObject::connect(&page, &QWebEnginePage::loadFinished, &page,
                     [&page, done, ok](bool result) {
        if (page.url().scheme() == QLatin1String("arora-file")) {
            *done = true;
            *ok = result;
        }
    });
    page.load(QUrl::fromLocalFile(dir.path()));
    QTRY_VERIFY_WITH_TIMEOUT(*done, 15000);
    QVERIFY(*ok);
    QVERIFY(pageHtml(&page).contains(QLatin1String("redirected.txt")));
}

void tst_SchemeHandlers::fileSchemeErrors()
{
    WebPage page(m_profile);

    // Directory that does not exist.
    QVERIFY(loadAndFail(&page, QUrl(QLatin1String(
        "arora-file:/nonexistent-cov03-dir-xyz"))));

    // A regular file is not this handler's job.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.filePath(QLatin1String("plain.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QVERIFY(loadAndFail(&page, FileAccessHandler::urlForLocalPath(
        dir.filePath(QLatin1String("plain.txt")))));

    // An unreadable directory is denied rather than listed.
    QVERIFY(QDir(dir.path()).mkdir(QLatin1String("locked")));
    const QString locked = dir.filePath(QLatin1String("locked"));
    QVERIFY(QFile::setPermissions(locked, QFile::Permissions()));
    QVERIFY(loadAndFail(&page, FileAccessHandler::urlForLocalPath(locked)));
    QVERIFY(QFile::setPermissions(locked, QFile::ReadOwner
                                | QFile::WriteOwner | QFile::ExeOwner));
}

// SEC02: a remote page must never reach the local directory listing —
// requests whose initiator is a remote origin are denied in the
// handler, whether they arrive as an iframe, a subresource, or a
// script-poked top-level navigation like the one exercised here.
void tst_SchemeHandlers::fileSchemeRemoteInitiator()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.filePath(QLatin1String("secret.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    WebPage page(m_profile);
    const QString target =
        FileAccessHandler::urlForLocalPath(dir.path()).toString();

    // An http-origin page poking location= at the internal scheme must
    // be refused — the listing content may never reach it.
    std::shared_ptr<bool> settled(new bool(false));
    QObject::connect(&page, &QWebEnginePage::loadFinished, &page,
                     [settled](bool) { *settled = true; });
    page.setHtml(QStringLiteral(
        "<html><body><script>location='%1';</script></body></html>")
            .arg(target),
        QUrl(QLatin1String("http://remote-sec02.example/")));
    waitFor(settled);
    // Give the denied navigation (or a Chromium-level refusal) time to
    // settle into whatever page survives.
    QTest::qWait(1000);
    const QString html = pageHtml(&page);
    QVERIFY(!html.contains(QLatin1String("secret.txt")));
    QVERIFY(!html.contains(QLatin1String("Contents of")));
}

void tst_SchemeHandlers::resourceScheme()
{
    WebPage page(m_profile);
    QVERIFY(loadSync(&page,
                     AdBlockResourceHandler::urlForResource("noop.html")));
    QVERIFY(loadAndFail(&page,
                        QUrl(QLatin1String("arora-resource:/bogus-stub"))));
}

void tst_SchemeHandlers::abpSchemeInvalid()
{
    WebPage page(m_profile);
    QVERIFY(loadAndFail(&page, QUrl(QLatin1String("abp:not-a-subscribe"))));
}

void tst_SchemeHandlers::abpSchemeSubscribeDeclined()
{
    AdBlockManager *manager = AdBlockManager::instance();
    const int before = manager->subscriptions().count();

    WebPage page(m_profile);
    ModalAnswer answer(QMessageBox::No, this);
    const QUrl url(QLatin1String("abp:subscribe?location=")
        + QString::fromUtf8(QUrl::fromLocalFile(
            QLatin1String("/nonexistent-cov03-list.txt")).toEncoded())
        + QLatin1String("&title=Cov03Test"));
    // An aborted navigation may not produce loadFinished at all — the
    // subscribe prompt being answered is the observable side effect.
    page.load(url);
    QTRY_VERIFY_WITH_TIMEOUT(answer.answered() >= 1, 15000);
    QTest::qWait(500); // let handleSubscribe finish + the job abort
    QCOMPARE(manager->subscriptions().count(), before);
}

void tst_SchemeHandlers::abpSchemeSubscribeAccepted()
{
    AdBlockManager *manager = AdBlockManager::instance();
    const int before = manager->subscriptions().count();

    WebPage page(m_profile);
    ModalAnswer answer(QMessageBox::Yes, this);
    const QUrl url(QLatin1String("abp:subscribe?location=")
        + QString::fromUtf8(QUrl::fromLocalFile(
            QLatin1String("/nonexistent-cov03-list.txt")).toEncoded())
        + QLatin1String("&title=Cov03Test"));
    page.load(url);
    QTRY_VERIFY_WITH_TIMEOUT(answer.answered() >= 1, 15000);
    // Accepting opens the AdBlock dialog, which itself materializes the
    // "Custom Rules" subscription — assert membership, not a delta.
    QTRY_VERIFY_WITH_TIMEOUT(
        manager->subscriptions().count() > before, 15000);
    bool found = false;
    const QList<AdBlockSubscription *> subscriptions =
        manager->subscriptions();
    for (AdBlockSubscription *subscription : subscriptions) {
        if (subscription->title() == QLatin1String("Cov03Test"))
            found = true;
    }
    QVERIFY(found);
    // cleanup() closes the dialog and drops the subscription.
}

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    // Custom URL schemes must be registered before BrowserApplication's
    // constructor brings up the browsing profile (same order as main.cpp).
    SchemeAccessHandler::registerUrlSchemes();
    AdBlockSchemeAccessHandler::registerUrlScheme();
    AdBlockResourceHandler::registerUrlScheme();
    BrowserApplication app(argc, argv);
    tst_SchemeHandlers tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_schemehandlers.moc"
