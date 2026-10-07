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

// COV03: the location bar trio — LocationBar key handling (modifier
// suffix expansion, Escape restore), url tracking, drag & drop, plus
// LocationBarSiteIcon and PrivacyIndicator widget paths.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qmimedata.h>

#include <memory>

#include "locationbar.h"
#include "locationbarsiteicon.h"
#include "privacyindicator.h"
#include "clearbutton.h"
#include "webview.h"
#include "browserapplication.h"
#include "historycompleter.h"
#include "historymanager.h"
#include "networkaccessmanager.h"
#include "omniboxsuggestions.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "toolbarsearch.h"
#include "qtest_arora.h"
#include "qtry.h"

// Exposes the protected event handlers for direct dispatch.
class TestLocationBar : public LocationBar
{
public:
    TestLocationBar(QWidget *parent = nullptr)
        : LocationBar(parent)
    {
    }

    void sendFocusOut()
    {
        QFocusEvent event(QEvent::FocusOut);
        focusOutEvent(&event);
    }

    void sendDrop(QMimeData *mimeData)
    {
        QDropEvent event(QPointF(1, 1), Qt::CopyAction, mimeData,
                         Qt::LeftButton, Qt::NoModifier);
        dropEvent(&event);
    }

    void sendDragEnter(QMimeData *mimeData, bool *accepted)
    {
        QDragEnterEvent event(QPointF(1, 1), Qt::CopyAction, mimeData,
                              Qt::LeftButton, Qt::NoModifier);
        dragEnterEvent(&event);
        *accepted = event.isAccepted();
    }

    void sendDoubleClick(Qt::MouseButton button)
    {
        QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(1, 1),
                          QPointF(1, 1), button, button,
                          Qt::NoModifier);
        mouseDoubleClickEvent(&event);
    }
};

class tst_LocationBar : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void widgets();
    void urlTracking();
    void shortcuts_data();
    void shortcuts();
    void escapeRestoresUrl();
    void focusOutRestoresUrl();
    void doubleClickSelects();
    void dropUrl();
    void siteIcon();
    void privacyIndicator();
    void omniboxSuggestions();
};

void tst_LocationBar::initTestCase()
{
    QCoreApplication::setApplicationName("tst_locationbar");
    QSettings settings;
    settings.clear();
}

// The constructor wires up the side widgets.
void tst_LocationBar::widgets()
{
    TestLocationBar bar;
    QVERIFY(bar.findChild<LocationBarSiteIcon*>());
    QVERIFY(bar.findChild<PrivacyIndicator*>());
    QVERIFY(bar.findChild<ClearButton*>());
    QVERIFY(!bar.webView());
}

// urlChanged on the view updates the bar while it is not focused.
void tst_LocationBar::urlTracking()
{
    TestLocationBar bar;
    WebView view(BrowserApplication::webEngineProfile());
    bar.setWebView(&view);
    QCOMPARE(bar.webView(), &view);

    const QUrl url(QStringLiteral("data:text/html,<p>tracked</p>"));
    view.loadUrl(url);
    QTRY_VERIFY_WITH_TIMEOUT(
        bar.text().startsWith(QLatin1String("data:text/html")), 15000);
}

// Enter with modifiers appends the classic TLD suffixes.
void tst_LocationBar::shortcuts_data()
{
    QTest::addColumn<QString>("typed");
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
    QTest::addColumn<QString>("expected");

    QTest::newRow("ctrl") << QString("example") << Qt::KeyboardModifiers(Qt::ControlModifier)
                          << QString("http://example.com");
    QTest::newRow("shift") << QString("example") << Qt::KeyboardModifiers(Qt::ShiftModifier)
                           << QString("http://example.net");
    QTest::newRow("ctrl+shift") << QString("example")
                                << Qt::KeyboardModifiers(Qt::ControlModifier | Qt::ShiftModifier)
                                << QString("http://example.org");
    // Already-qualified urls and existing suffixes are left alone.
    QTest::newRow("no-modifier") << QString("example") << Qt::KeyboardModifiers(Qt::NoModifier)
                                 << QString("example");
    QTest::newRow("full-url") << QString("http://already.net")
                              << Qt::KeyboardModifiers(Qt::ControlModifier)
                              << QString("http://already.net");
    // Host already ends with the modifier's suffix: keyPressEvent
    // leaves the bar text alone (no rewrite at all).
    QTest::newRow("has-suffix") << QString("example.com")
                                << Qt::KeyboardModifiers(Qt::ControlModifier)
                                << QString("example.com");
}

void tst_LocationBar::shortcuts()
{
    QFETCH(QString, typed);
    QFETCH(Qt::KeyboardModifiers, modifiers);
    QFETCH(QString, expected);

    TestLocationBar bar;
    bar.setText(typed);
    QTest::keyClick(&bar, Qt::Key_Enter, modifiers);
    QCOMPARE(bar.text(), expected);
}

// Escape restores the current page url into the bar.
void tst_LocationBar::escapeRestoresUrl()
{
    TestLocationBar bar;
    WebView view(BrowserApplication::webEngineProfile());
    bar.setWebView(&view);

    const QUrl url(QStringLiteral("data:text/html,<p>esc</p>"));
    view.loadUrl(url);
    QTRY_VERIFY_WITH_TIMEOUT(
        bar.text().startsWith(QLatin1String("data:text/html")), 15000);

    bar.setText(QLatin1String("junk"));
    QTest::keyClick(&bar, Qt::Key_Escape);
    QCOMPARE(bar.text(), QString::fromUtf8(view.url().toEncoded()));
    QCOMPARE(bar.selectedText(), bar.text());
}

// Clearing the bar and defocusing it restores the page url.
void tst_LocationBar::focusOutRestoresUrl()
{
    TestLocationBar bar;
    WebView view(BrowserApplication::webEngineProfile());
    bar.setWebView(&view);

    const QUrl url(QStringLiteral("data:text/html,<p>focus</p>"));
    view.loadUrl(url);
    QTRY_VERIFY_WITH_TIMEOUT(
        bar.text().startsWith(QLatin1String("data:text/html")), 15000);

    bar.clear();
    bar.sendFocusOut();
    QCOMPARE(bar.text(), QString::fromUtf8(view.url().toEncoded()));
}

void tst_LocationBar::doubleClickSelects()
{
    TestLocationBar bar;
    bar.setText(QLatin1String("select-me"));

    bar.sendDoubleClick(Qt::LeftButton);
    QCOMPARE(bar.selectedText(), bar.text());

    // Other buttons pass through unhandled.
    bar.deselect();
    bar.setCursorPosition(0);
    bar.sendDoubleClick(Qt::RightButton);
    QVERIFY(bar.selectedText().isEmpty());
}

// A url dragged onto the bar fills it; undecodable drops are ignored.
void tst_LocationBar::dropUrl()
{
    TestLocationBar bar;

    QMimeData urlMime;
    urlMime.setUrls(QList<QUrl>() << QUrl(QLatin1String("http://dropped.example/")));
    bool accepted = false;
    bar.sendDragEnter(&urlMime, &accepted);
    QVERIFY(accepted);
    bar.sendDrop(&urlMime);
    QCOMPARE(bar.text(), QLatin1String("http://dropped.example/"));
    QCOMPARE(bar.selectedText(), bar.text());

    QMimeData textMime;
    textMime.setText(QLatin1String("http://text.example/"));
    bar.sendDragEnter(&textMime, &accepted);
    QVERIFY(accepted);
    bar.sendDrop(&textMime);
    QCOMPARE(bar.text(), QLatin1String("http://text.example/"));

    // Empty payload: nothing to turn into a url, text is untouched.
    QMimeData emptyMime;
    const QString before = bar.text();
    bar.sendDragEnter(&emptyMime, &accepted);
    bar.sendDrop(&emptyMime);
    QCOMPARE(bar.text(), before);
}

void tst_LocationBar::siteIcon()
{
    LocationBarSiteIcon icon;
    WebView view(BrowserApplication::webEngineProfile());
    icon.setWebView(&view);

    // click arms the drag start position
    QTest::mouseClick(&icon, Qt::LeftButton);

    // The default icon is installed immediately and refreshed on load.
    QVERIFY(!icon.pixmap().isNull());
    view.loadUrl(QUrl(QStringLiteral("data:text/html,<p>icon</p>")));
    QTRY_VERIFY_WITH_TIMEOUT(!icon.pixmap().isNull(), 15000);
}

void tst_LocationBar::privacyIndicator()
{
    // The shared persistent profile — the bare WebView() ctor would
    // land on QWebEngineProfile::defaultProfile(), which is
    // off-the-record in Qt6.
    WebView normalView(BrowserApplication::webEngineProfile());
    PrivacyIndicator indicator;
    indicator.setWebView(&normalView);
    QVERIFY(!indicator.isVisible()); // normal profile: nothing to show

    // A page on an off-the-record profile reveals the indicator.
    QWebEngineProfile privateProfile;
    WebView privateView(&privateProfile);
    indicator.setWebView(&privateView);
    QVERIFY(indicator.isVisible());

    // Clicking it asks the application to leave private mode.
    QTest::mouseClick(&indicator, Qt::LeftButton);
    QVERIFY(!BrowserApplication::isPrivate());
}

// SRCH01: the location-bar dropdown merges engine suggestions above
// the history completion — but keystrokes may only reach the suggest
// endpoint while the engine is opted in (SEC11).  Off must mean zero
// app-side requests and no rows; on must fetch, populate the merged
// model and surface the suggestion text through the url role so
// activation routes back through guessUrlFromString.
void tst_LocationBar::omniboxSuggestions()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();

    // A hermetic engine: its suggest endpoint is a local file so the
    // enabled-path check never leaves the box.
    const QString fixturePath = QDir::temp().filePath(
        QLatin1String("arora-omnibox-suggest.json"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("[\"hi\",[\"hi there\",\"hi all\"]]");
    }
    OpenSearchEngine *engine = new OpenSearchEngine;
    engine->setName(QLatin1String("omnibox-suggest-test"));
    engine->setSearchUrlTemplate(
        QLatin1String("http://omnibox-suggest.invalid/q={searchTerms}"));
    engine->setSuggestionsUrlTemplate(
        QLatin1String("file://") + fixturePath
        + QLatin1String("?q={searchTerms}"));
    QVERIFY(engine->providesSuggestions());
    // A previous interrupted run may have persisted this engine.
    if (manager->engineExists(engine->name()))
        manager->removeEngine(engine->name());
    QVERIFY(manager->addEngine(engine));

    const QString previousEngine = manager->currentEngineName();
    manager->setCurrentEngineName(engine->name());
    QVERIFY(!manager->suggestionsEnabledForEngine(engine->name()));

    // The same completion stack makeNewTab() builds.
    HistoryCompletionModel *history =
        new HistoryCompletionModel(this);
    history->setSourceModel(
        BrowserApplication::historyManager()->historyFilterModel());
    OmniboxCompletionModel model(history);
    HistoryCompleter completer(&model);
    TestLocationBar bar;
    bar.setCompleter(&completer);
    OmniboxSuggestions provider(&model, &completer, this);
    connect(&bar, &QLineEdit::textEdited,
            &provider, &OmniboxSuggestions::scheduleSuggestions);
    QCOMPARE(model.data(model.index(0, 1)).toString(), QString());

    // Every app-side request the NAM creates reports through this
    // signal — a suggest fetch cannot hide from it.
    NetworkAccessManager *nam = NetworkAccessManager::instance();
    QStringList requestUrls;
    const QMetaObject::Connection requestConn =
        connect(nam, &NetworkAccessManager::requestCreated, nam,
            [&requestUrls](QNetworkAccessManager::Operation,
                           const QNetworkRequest &request, QNetworkReply *) {
                requestUrls << request.url().toString();
            });

    // Opt-out (the default): typing must produce ZERO requests.
    QTest::keyClicks(&bar, QLatin1String("hi"));
    QTest::qWait(500); // past the 200ms debounce timer
    QCOMPARE(requestUrls.count(), 0);
    QCOMPARE(model.suggestions(), QStringList());

    // Opt in: the debounced fetch hits the suggest endpoint and the
    // rows land above the history block.
    manager->setSuggestionsEnabledForEngine(engine->name(), true);
    QTRY_VERIFY_WITH_TIMEOUT(!requestUrls.isEmpty(), 3000);
    QVERIFY(requestUrls.last().contains(fixturePath));
    QTRY_VERIFY_WITH_TIMEOUT(model.suggestions().count() == 2, 3000);

    QCOMPARE(model.data(model.index(0, 0)).toString(),
             QLatin1String("hi there"));
    QCOMPARE(model.data(model.index(0, 0), HistoryModel::UrlStringRole)
                 .toString(),
             QLatin1String("hi there"));
    QCOMPARE(model.data(model.index(1, 0)).toString(),
             QLatin1String("hi all"));
    QCOMPARE(model.data(model.index(0, 1)).toString(),
             QLatin1String("Search omnibox-suggest-test"));
    QVERIFY(model.flags(model.index(0, 0)) & Qt::ItemIsSelectable);
    QVERIFY(model.flags(model.index(0, 0)) & Qt::ItemIsEnabled);

    // History rows are still reachable below the suggestion block and
    // keep their real roles.
    const int historyRows = history->rowCount();
    QCOMPARE(model.rowCount(), 2 + historyRows);

    // Disabling drops the rows and silences the data path again.
    manager->setSuggestionsEnabledForEngine(engine->name(), false);
    QCOMPARE(model.suggestions(), QStringList());
    QCOMPARE(model.rowCount(), historyRows);
    const int requestsSeen = requestUrls.count();
    QTest::keyClicks(&bar, QLatin1String(" again"));
    QTest::qWait(500);
    QCOMPARE(requestUrls.count(), requestsSeen);

    disconnect(requestConn);
    manager->setCurrentEngineName(previousEngine);
    manager->removeEngine(engine->name());
    QFile::remove(fixturePath);
}

QTEST_MAIN(tst_LocationBar)
#include "tst_locationbar.moc"
