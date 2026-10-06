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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// SEC05: the permission broker is exercised end-to-end — a real
// WebPage served by an in-process HTTP server calls the Web platform
// permission APIs, the manager resolves the QWebEnginePermission, and
// the decision is read back through JS promises (Notification.
// requestPermission resolves 'granted'/'denied', permissions.query
// reflects the outcome, clipboard read rejects with NotAllowedError).

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qcheckbox.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebenginepermission.h>
#include <qwebengineprofile.h>

#include "browserprofile.h"
#include "webpermissionmanager.h"
#include "webpage.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

// Minimal HTTP/1.0 responder serving one html page (same pattern as
// tst_adblockrequestinterceptor).  The permission origin is
// http://127.0.0.1:<port> — every server instance is a fresh origin.
class LocalHttpServer : public QObject
{
    Q_OBJECT

public:
    LocalHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this,
                [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() {
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                socket->readAll();
                const QByteArray response =
                    QByteArrayLiteral("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                                      "Content-Length: ")
                    + QByteArray::number(indexHtml.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                    + indexHtml;
                socket->write(response);
                socket->disconnectFromHost();
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1/")
                    .arg(m_server.serverPort()));
    }

    QUrl origin() const
    {
        QUrl o = url();
        o.setPath(QString());
        return o;
    }

    QByteArray indexHtml;

private:
    QTcpServer m_server;
};

// The consent prompt is a modal exec() opened inside whatever event
// loop is running — including evalSync()'s own qWait loop — so the
// answer cannot be scheduled AFTER the request is triggered (that call
// never returns until the dialog closes).  This RAII helper arms a
// repeating timer that keeps firing inside the nested modal loop and
// clicks the requested button (or rejects the dialog) the moment it
// appears.
class ModalClicker
{
public:
    ModalClicker(QMessageBox::StandardButton button,
                 bool uncheckRemember = false)
        : m_clicked(false)
    {
        m_timer.setInterval(50);
        QObject::connect(&m_timer, &QTimer::timeout, qApp,
                         [this, button, uncheckRemember]() {
            QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!box)
                return;
            if (uncheckRemember && box->checkBox())
                box->checkBox()->setChecked(false);
            QAbstractButton *b = box->button(button);
            if (b)
                b->click();
            else
                box->reject();
            m_clicked = true;
        });
        m_timer.start();
    }

    ~ModalClicker() { m_timer.stop(); }

    bool clicked() const { return m_clicked; }

private:
    QTimer m_timer;
    bool m_clicked;
};

class tst_WebPermissions : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void policyClassification_data();
    void policyClassification();
    void engineNeverRemembers();
    void typeNamesAndDescriptions();
    void storeRoundTrip();
    void sessionOnlyDecisions();

    void deniedByDefault();
    void grantPersists();
    void revokeReinstatesPrompt();
    void deniedTypesNeverPrompt();
    void otrNeverPersists();

private:
    // Runs js on the page and returns the result.
    QVariant evalSync(QWebEnginePage *page, const QString &js);
    // Waits until window.results contains a "tag"-prefixed entry.
    QStringList pollResults(QWebEnginePage *page, const QString &tag);
};

static const char *kHelpers =
    "<html><body><script>"
    "window.results = [];"
    "window.askNotify = function() {"
    "  Notification.requestPermission().then(function(p){"
    "    window.results.push('perm:' + p);});};"
    "window.queryNotify = function() {"
    "  navigator.permissions.query({name:'notifications'}).then(function(r){"
    "    window.results.push('query:' + r.state);});};"
    "window.askGeo = function() {"
    "  navigator.geolocation.getCurrentPosition("
    "    function(){window.results.push('geo:ok')},"
    "    function(e){window.results.push('geo:' + e.code)});};"
    "window.askClip = function() {"
    "  navigator.clipboard.readText().then("
    "    function(){window.results.push('clip:ok')},"
    "    function(e){window.results.push('clip:' + e.name)});};"
    "</script></html>";

QVariant tst_WebPermissions::evalSync(QWebEnginePage *page, const QString &js)
{
    QVariant result;
    bool done = false;
    page->runJavaScript(js, [&](const QVariant &v) { result = v; done = true; });
    // QTRY_* macros return void — unusable inside a value-returning
    // helper.
    for (int waited = 0; !done && waited < 10000; waited += 50)
        QTest::qWait(50);
    return result;
}

QStringList tst_WebPermissions::pollResults(QWebEnginePage *page,
        const QString &tag)
{
    QStringList results;
    for (int waited = 0; waited < 10000; waited += 100) {
        results = evalSync(page, QLatin1String("window.results")).toStringList();
        if (results.filter(tag).count() > 0)
            return results;
        QTest::qWait(100);
    }
    return results;
}

void tst_WebPermissions::initTestCase()
{
    QCoreApplication::setApplicationName("tst_webpermissions");
    QSettings settings;
    settings.remove(QLatin1String("webpermissions"));
}

void tst_WebPermissions::init()
{
    // Fresh store + fresh session decisions per test.
    WebPermissionManager::instance()->clearEntries();
}

void tst_WebPermissions::cleanup()
{
    WebPermissionManager::instance()->clearEntries();
}

void tst_WebPermissions::policyClassification_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<bool>("promptable");

    // The classification is total: every type is either promptable or
    // hard-denied.
    QTest::newRow("unsupported") << int(QWebEnginePermission::PermissionType::Unsupported) << false;
    QTest::newRow("audio") << int(QWebEnginePermission::PermissionType::MediaAudioCapture) << false;
    QTest::newRow("video") << int(QWebEnginePermission::PermissionType::MediaVideoCapture) << false;
    QTest::newRow("audiovideo") << int(QWebEnginePermission::PermissionType::MediaAudioVideoCapture) << false;
    QTest::newRow("desktop-video") << int(QWebEnginePermission::PermissionType::DesktopVideoCapture) << false;
    QTest::newRow("desktop-av") << int(QWebEnginePermission::PermissionType::DesktopAudioVideoCapture) << false;
    QTest::newRow("mouselock") << int(QWebEnginePermission::PermissionType::MouseLock) << true;
    QTest::newRow("notifications") << int(QWebEnginePermission::PermissionType::Notifications) << true;
    QTest::newRow("geolocation") << int(QWebEnginePermission::PermissionType::Geolocation) << true;
    QTest::newRow("clipboard") << int(QWebEnginePermission::PermissionType::ClipboardReadWrite) << false;
    QTest::newRow("localfonts") << int(QWebEnginePermission::PermissionType::LocalFontsAccess) << false;
}

void tst_WebPermissions::policyClassification()
{
    QFETCH(int, type);
    QFETCH(bool, promptable);
    const QWebEnginePermission::PermissionType t =
        static_cast<QWebEnginePermission::PermissionType>(type);
    QCOMPARE(WebPermissionManager::isPromptable(t), promptable);
    QCOMPARE(WebPermissionManager::isDenied(t), !promptable);
}

void tst_WebPermissions::engineNeverRemembers()
{
    // The named browsing profile must stay AskEveryTime: with the
    // default StoreOnDisk, Chromium persists every grant()/deny() to
    // permissions.json and silently applies it on later visits —
    // bypassing the broker's prompt, its "remember" checkbox and the
    // auditable store (a stale on-disk grant once resolved
    // 'granted' without permissionRequested ever firing).
    QCOMPARE(BrowserProfile::normalProfile()->persistentPermissionsPolicy(),
             QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
}

void tst_WebPermissions::typeNamesAndDescriptions()
{
    // Every type must render into the prompt/audit UI without falling
    // back to the generic label (except Unsupported, which IS the
    // generic label).
    for (int i = 0; i <= int(QWebEnginePermission::PermissionType::LocalFontsAccess); ++i) {
        const QWebEnginePermission::PermissionType t =
            static_cast<QWebEnginePermission::PermissionType>(i);
        QVERIFY(!WebPermissionManager::typeName(t).isEmpty());
        QVERIFY(!WebPermissionManager::typeDescription(t).isEmpty());
    }
}

void tst_WebPermissions::storeRoundTrip()
{
    WebPermissionManager manager;
    WebPermissionManager second;

    const QUrl origin(QStringLiteral("https://example.com:8443"));
    const QUrl other(QStringLiteral("http://127.0.0.1:9"));

    QVERIFY(manager.entries().isEmpty());

    manager.setEntry(origin, QWebEnginePermission::PermissionType::Notifications, true);
    manager.setEntry(origin, QWebEnginePermission::PermissionType::Geolocation, false);
    manager.setEntry(other, QWebEnginePermission::PermissionType::Notifications, true);

    // The second instance reads the same QSettings store.
    const QList<WebPermissionManager::Entry> entries = second.entries();
    QCOMPARE(entries.count(), 3);

    int granted = 0, denied = 0;
    for (const WebPermissionManager::Entry &e : entries) {
        if (e.origin == origin
            && e.type == QWebEnginePermission::PermissionType::Notifications && e.granted)
            ++granted;
        if (e.origin == origin
            && e.type == QWebEnginePermission::PermissionType::Geolocation && !e.granted)
            ++denied;
        if (e.origin == other
            && e.type == QWebEnginePermission::PermissionType::Notifications && e.granted)
            ++granted;
    }
    QCOMPARE(granted, 2);
    QCOMPARE(denied, 1);

    second.removeEntry(origin, QWebEnginePermission::PermissionType::Geolocation);
    QCOMPARE(manager.entries().count(), 2);

    second.clearEntries();
    QVERIFY(manager.entries().isEmpty());
}

void tst_WebPermissions::sessionOnlyDecisions()
{
    // A prompt answered with "Remember this decision" unchecked still
    // applies for the session (anti spam-loop) but writes nothing to
    // the persistent store.
    LocalHttpServer server;
    server.indexHtml = kHelpers;
    QVERIFY(server.start());

    WebPermissionManager *manager = WebPermissionManager::instance();
    QSignalSpy prompts(manager, SIGNAL(promptRequested(QUrl,QWebEnginePermission::PermissionType)));

    WebView view(BrowserProfile::normalProfile());
    view.show();
    QSignalSpy loaded(view.webPage(), SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    QCOMPARE(loaded.at(0).at(0).toBool(), true);

    ModalClicker allowNoRemember(QMessageBox::Yes, true);
    evalSync(view.webPage(), QLatin1String("window.askNotify()"));
    QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                 .filter(QLatin1String("perm:granted")).count(), 1);
    QVERIFY(allowNoRemember.clicked());
    // Not persisted — entries() is the auditable store.
    QVERIFY(manager->entries().isEmpty());

    // But the answer IS remembered for the session: no second prompt.
    evalSync(view.webPage(), QLatin1String("window.askNotify()"));
    QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                 .filter(QLatin1String("perm:granted")).count(), 2);
    QCOMPARE(prompts.count(), 1);
}

void tst_WebPermissions::deniedByDefault()
{
    // Notifications are the promptable path; the safe answer is Deny
    // and — with "remember" checked by default — it is persisted.
    LocalHttpServer server;
    server.indexHtml = kHelpers;
    QVERIFY(server.start());

    WebPermissionManager *manager = WebPermissionManager::instance();
    QSignalSpy prompts(manager, SIGNAL(promptRequested(QUrl,QWebEnginePermission::PermissionType)));

    WebView view(BrowserProfile::normalProfile());
    view.show();
    QSignalSpy loaded(view.webPage(), SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    {
        ModalClicker deny(QMessageBox::No);
        evalSync(view.webPage(), QLatin1String("window.askNotify()"));
        QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                     .filter(QLatin1String("perm:denied")).count(), 1);
        QVERIFY(deny.clicked());
    }
    QCOMPARE(prompts.count(), 1);

    const QList<WebPermissionManager::Entry> entries = manager->entries();
    QCOMPARE(entries.count(), 1);
    QCOMPARE(entries.at(0).origin, server.origin());
    QCOMPARE(int(entries.at(0).type),
             int(QWebEnginePermission::PermissionType::Notifications));
    QCOMPARE(entries.at(0).granted, false);

    // The remembered deny is honored without prompting again.
    evalSync(view.webPage(), QLatin1String("window.askNotify()"));
    QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                 .filter(QLatin1String("perm:denied")).count(), 2);
    QCOMPARE(prompts.count(), 1);
}

void tst_WebPermissions::grantPersists()
{
    LocalHttpServer server;
    server.indexHtml = kHelpers;
    QVERIFY(server.start());

    WebPermissionManager *manager = WebPermissionManager::instance();
    QSignalSpy prompts(manager, SIGNAL(promptRequested(QUrl,QWebEnginePermission::PermissionType)));

    WebView view(BrowserProfile::normalProfile());
    view.show();
    QSignalSpy loaded(view.webPage(), SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    {
        ModalClicker allow(QMessageBox::Yes);
        evalSync(view.webPage(), QLatin1String("window.askNotify()"));
        QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                     .filter(QLatin1String("perm:granted")).count(), 1);
        QVERIFY(allow.clicked());
    }

    evalSync(view.webPage(), QLatin1String("window.queryNotify()"));
    QCOMPARE(pollResults(view.webPage(), QLatin1String("query:"))
                 .filter(QLatin1String("query:granted")).count(), 1);

    QCOMPARE(manager->entries().count(), 1);
    QCOMPARE(manager->entries().at(0).granted, true);

    // A brand new page on the same origin grants without a prompt —
    // the decision survived in the store.
    WebView view2(BrowserProfile::normalProfile());
    view2.show();
    QSignalSpy loaded2(view2.webPage(), SIGNAL(loadFinished(bool)));
    view2.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded2.count() >= 1, 15000);
    QCOMPARE(loaded2.at(0).at(0).toBool(), true);

    ModalClicker allow(QMessageBox::Yes);
    evalSync(view2.webPage(), QLatin1String("window.askNotify()"));
    QCOMPARE(pollResults(view2.webPage(), QLatin1String("perm:"))
                 .filter(QLatin1String("perm:granted")).count(), 1);
    QTest::qWait(300);
    QCOMPARE(prompts.count(), 1);
    QVERIFY(!allow.clicked());
}

void tst_WebPermissions::revokeReinstatesPrompt()
{
    LocalHttpServer server;
    server.indexHtml = kHelpers;
    QVERIFY(server.start());

    WebPermissionManager *manager = WebPermissionManager::instance();
    QSignalSpy prompts(manager, SIGNAL(promptRequested(QUrl,QWebEnginePermission::PermissionType)));

    WebView view(BrowserProfile::normalProfile());
    view.show();
    QSignalSpy loaded(view.webPage(), SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    {
        ModalClicker allow(QMessageBox::Yes);
        evalSync(view.webPage(), QLatin1String("window.askNotify()"));
        QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                     .filter(QLatin1String("perm:granted")).count(), 1);
        QVERIFY(allow.clicked());
    }
    QCOMPARE(manager->entries().count(), 1);

    // Revoke through the manager (what the Settings Remove button
    // does).  The stored grant is gone — a request that would have
    // silently re-applied it must now reach the consent prompt.
    //
    // Arora pins every profile to AskEveryTime, so the engine-side
    // cache cannot resurrect the removed grant — but the check still
    // uses a fresh off-the-record profile: a different permission
    // context, which must consult the shared persistent store and find
    // nothing.
    manager->removeEntry(server.origin(),
        QWebEnginePermission::PermissionType::Notifications);
    QVERIFY(manager->entries().isEmpty());

    QWebEngineProfile freshProfile;
    WebView view2(&freshProfile);
    view2.show();
    QSignalSpy loaded2(view2.webPage(), SIGNAL(loadFinished(bool)));
    view2.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded2.count() >= 1, 15000);
    QCOMPARE(loaded2.at(0).at(0).toBool(), true);

    ModalClicker deny(QMessageBox::No);
    evalSync(view2.webPage(), QLatin1String("window.askNotify()"));
    pollResults(view2.webPage(), QLatin1String("perm:"));
    QVERIFY(deny.clicked());
    QCOMPARE(prompts.count(), 2);
    // The OTR page never writes to the store — entries stay empty.
    QVERIFY(manager->entries().isEmpty());
}

void tst_WebPermissions::deniedTypesNeverPrompt()
{
    // Clipboard reads are policy-denied: no dialog, no store entry,
    // just a rejected promise.
    LocalHttpServer server;
    server.indexHtml = kHelpers;
    QVERIFY(server.start());

    WebPermissionManager *manager = WebPermissionManager::instance();
    QSignalSpy prompts(manager, SIGNAL(promptRequested(QUrl,QWebEnginePermission::PermissionType)));

    WebView view(BrowserProfile::normalProfile());
    view.show();
    QSignalSpy loaded(view.webPage(), SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    ModalClicker trap(QMessageBox::Yes);
    evalSync(view.webPage(), QLatin1String("window.askClip()"));
    QCOMPARE(pollResults(view.webPage(), QLatin1String("clip:"))
                 .filter(QLatin1String("clip:NotAllowedError")).count(), 1);
    QTest::qWait(300);
    QCOMPARE(prompts.count(), 0);
    QVERIFY(!trap.clicked());
    QVERIFY(manager->entries().isEmpty());
}

void tst_WebPermissions::otrNeverPersists()
{
    // A private (off-the-record) page gets the same prompt and grant
    // flow, but "remember" must never touch the persistent store — and
    // the session answer stays in its own namespace so it cannot bleed
    // into normal browsing.
    LocalHttpServer server;
    server.indexHtml = kHelpers;
    QVERIFY(server.start());

    WebPermissionManager *manager = WebPermissionManager::instance();
    QSignalSpy prompts(manager, SIGNAL(promptRequested(QUrl,QWebEnginePermission::PermissionType)));

    WebView view(BrowserProfile::privateProfile());
    view.show();
    QSignalSpy loaded(view.webPage(), SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    QCOMPARE(loaded.at(0).at(0).toBool(), true);

    {
        ModalClicker allow(QMessageBox::Yes);
        evalSync(view.webPage(), QLatin1String("window.askNotify()"));
        QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                     .filter(QLatin1String("perm:granted")).count(), 1);
        QVERIFY(allow.clicked());
    }
    // Granted but NOT persisted: private browsing must not leak.
    QVERIFY(manager->entries().isEmpty());

    // Session-remembered within the OTR namespace: no second prompt.
    evalSync(view.webPage(), QLatin1String("window.askNotify()"));
    QCOMPARE(pollResults(view.webPage(), QLatin1String("perm:"))
                 .filter(QLatin1String("perm:granted")).count(), 2);
    QCOMPARE(prompts.count(), 1);

    // The OTR answer does not leak into normal browsing — the same
    // origin on the normal profile prompts again.
    WebView normalView(BrowserProfile::normalProfile());
    normalView.show();
    QSignalSpy loadedN(normalView.webPage(), SIGNAL(loadFinished(bool)));
    normalView.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loadedN.count() >= 1, 15000);
    QCOMPARE(loadedN.at(0).at(0).toBool(), true);

    ModalClicker allow(QMessageBox::Yes);
    evalSync(normalView.webPage(), QLatin1String("window.askNotify()"));
    QCOMPARE(pollResults(normalView.webPage(), QLatin1String("perm:"))
                 .filter(QLatin1String("perm:granted")).count(), 1);
    QVERIFY(allow.clicked());
    QCOMPARE(prompts.count(), 2);
    // And that normal answer persisted normally.
    QCOMPARE(manager->entries().count(), 1);
}

QTEST_MAIN(tst_WebPermissions)
#include "tst_webpermissions.moc"
