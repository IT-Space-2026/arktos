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
#include <qboxlayout.h>
#include <qcheckbox.h>
#include <qlabel.h>
#include <qpushbutton.h>
#include <qtoolbutton.h>
#include <qmenu.h>
#include <qmimedata.h>
#include <qstyleoption.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtooltip.h>

#include <memory>

#include "locationbar.h"
#include "locationbarsiteicon.h"
#include "privacyindicator.h"
#include "readerbutton.h"
#include "adblockdialog.h"
#include "adblockmanager.h"
#include "clearbutton.h"
#include "sitepadlock.h"
#include "sitepanel.h"
#include "siteshield.h"
#include "webview.h"
#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "browserapplication.h"
#include "historycompleter.h"
#include "historymanager.h"
#include "networkaccessmanager.h"
#include "omniboxsuggestions.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "toolbarsearch.h"
#include "webpermissionmanager.h"
#include "profilesettings.h"
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

    void sendToolTip()
    {
        QHelpEvent event(QEvent::ToolTip, QPoint(3, 3), QPoint(3, 3));
        this->event(&event);
    }

    // The text area LocationBar::paintEvent computes for the SAFE03
    // emphasis repaint.
    QRect textContentsRect()
    {
        QStyleOptionFrame panel;
        initStyleOption(&panel);
        QRect rect =
            style()->subElementRect(QStyle::SE_LineEditContents, &panel,
                                    this);
        rect.adjust(2, 0, -2, 0);
        rect.adjust(textMargin(LineEdit::LeftSide), 0,
                    -textMargin(LineEdit::RightSide), 0);
        return rect;
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
    void backspaceClearsField();
    void focusOutRestoresUrl();
    void doubleClickSelects();
    void dropUrl();
    void siteIcon();
    void privacyIndicator();
    void contentBlockingShield();
    void shieldPopupFirstShow();
    void omniboxSuggestions();
    void omniboxScopedCompletions();
    void domainEmphasis_data();
    void domainEmphasis();
    void punycodeDisplay();
    void domainEmphasisPaint();
};

void tst_LocationBar::initTestCase()
{
    QCoreApplication::setApplicationName("tst_locationbar");
    ProfileSettings settings;
    settings.clear();
    // Dead local list keeps AdBlockManager away from the live remote
    // defaults — adBlockButton() exercises the enable toggle, which
    // would otherwise grant TELEM01 consent and kick real fetches.
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-adb05.txt"
            "&title=DeadList"));
}

// The constructor wires up the side widgets.
void tst_LocationBar::widgets()
{
    TestLocationBar bar;
    QVERIFY(bar.findChild<LocationBarSiteIcon*>());
    // TRUST14: the in-bar indicator is the trust padlock now — the
    // site shield moved onto the navigation toolbar next to the bar.
    QVERIFY(bar.findChild<SitePadlockButton*>());
    QVERIFY(bar.findChild<PrivacyIndicator*>());
    QVERIFY(bar.findChild<ClearButton*>());
    // TRUST15: the site shield moved out of the bar onto the nav row
    // (tst_browsermainwindow::siteShieldButton locks the slot).
    QVERIFY(!bar.findChild<SiteShieldButton*>());
    // SHLD02: one shield only — the separate content-blocker button
    // was merged into the site shield.
    const QList<QToolButton*> buttons = bar.findChildren<QToolButton*>();
    for (QToolButton *button : buttons)
        QVERIFY(button->accessibleName()
                != QLatin1String("Content Blocker"));
    // READ03: no reader control inside the URL field — the single
    // toggle lives on the navigation toolbar (navReaderButton).
    QVERIFY(!bar.findChild<ReaderButton*>());
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

// SRCH10: the user's report — with the url bar showing its baseline
// text (the just-navigated display, cursor homed at 0) one Backspace
// clears the whole line; once the user has edited, Backspace stays
// character-wise to empty even with a live HistoryCompleter — the
// dropdown must not re-extend the field mid-delete.
void tst_LocationBar::backspaceClearsField()
{
    HistoryManager *historyManager = BrowserApplication::historyManager();
    historyManager->clear();
    historyManager->addHistoryEntry(
        QLatin1String("http://backspace-test.example/alpha"));

    TestLocationBar bar;
    WebView view(BrowserApplication::webEngineProfile());
    bar.setWebView(&view);

    const QUrl url(QStringLiteral("http://backspace-test.example/nav"));
    QVERIFY(QMetaObject::invokeMethod(&bar, "webViewUrlChanged",
                                    Q_ARG(QUrl, url)));
    QCOMPARE(bar.text(), QLatin1String("http://backspace-test.example/nav"));

    // Baseline text: focus into the bar and one Backspace clears it.
    bar.show();
    QVERIFY(QTest::qWaitForWindowExposed(&bar));
    bar.setFocus();
    QVERIFY(bar.hasFocus());
    QTest::keyClick(&bar, Qt::Key_Backspace);
    QVERIFY(bar.text().isEmpty());

    // Live completer — repeated backspace deletes single characters
    // all the way to empty; model churn cannot pin or re-extend text.
    HistoryCompletionModel *historyModel =
        new HistoryCompletionModel(&bar);
    historyModel->setSourceModel(historyManager->historyFilterModel());
    OmniboxCompletionModel *omnibox =
        new OmniboxCompletionModel(historyModel, &bar);
    bar.setCompleter(new HistoryCompleter(omnibox, &bar));

    QTest::keyClicks(&bar, QLatin1String("back"));
    QTest::qWait(200); // past the completer's 150ms filter debounce
    QTest::keyClick(&bar, Qt::Key_Backspace);
    QCOMPARE(bar.text(), QLatin1String("bac"));
    QTest::keyClick(&bar, Qt::Key_Backspace);
    QCOMPARE(bar.text(), QLatin1String("ba"));
    QTest::keyClick(&bar, Qt::Key_Backspace);
    QTest::keyClick(&bar, Qt::Key_Backspace);
    QVERIFY(bar.text().isEmpty());

    // Mid-text deletion after a real edit is still one character.
    QTest::keyClicks(&bar, QLatin1String("mid"));
    bar.setCursorPosition(1);
    QTest::keyClick(&bar, Qt::Key_Backspace);
    QCOMPARE(bar.text(), QLatin1String("id"));

    historyManager->removeHistoryEntry(
        QUrl(QLatin1String("http://backspace-test.example/alpha")));
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

// SHLD02 + TRUST14 + TRUST15: the merged site shield — the standalone
// content-blocker button is gone from the right cluster; the shield
// itself rides the navigation bar as the window's shared button
// (tst_browsermainwindow covers the toolbar placement and active-tab
// rebinding) and is never hidden — unbound it shows disabled with the
// honest reason.  The in-bar site-info slot belongs to the trust
// padlock now; the shield is exercised standalone — the panel surface
// is identical wherever the button lives.
void tst_LocationBar::contentBlockingShield()
{
    TestLocationBar bar;

    SitePadlockButton *padlock = bar.findChild<SitePadlockButton*>();
    LocationBarSiteIcon *siteIcon = bar.findChild<LocationBarSiteIcon*>();
    QVERIFY(padlock);
    QVERIFY(siteIcon);
    QVERIFY(padlock->isHidden()); // nothing to report without a view
    // One merged indicator — the right cluster has no blocker button,
    // and the bar carries no shield child of its own anymore.
    const QList<QToolButton*> buttons = bar.findChildren<QToolButton*>();
    for (QToolButton *candidate : buttons)
        QVERIFY(candidate->accessibleName()
                != QLatin1String("Content Blocker"));
    QVERIFY(!bar.findChild<SiteShieldButton*>());

    SiteShieldButton shield;
    // Unbound: still enabled-state driven, never a hidden gap.
    QVERIFY(!shield.isEnabled());
    QVERIFY(shield.toolTip().contains(QLatin1String("not available"),
                                      Qt::CaseInsensitive));

    WebView view(BrowserApplication::webEngineProfile());
    bar.setWebView(&view);
    QVERIFY(!padlock->isHidden());

    // Vivaldi order on the left — padlock, then site icon, then the
    // url text.
    bar.show();
    QTRY_VERIFY_WITH_TIMEOUT(
        padlock->mapTo(&bar, QPoint(0, 0)).x()
            < siteIcon->mapTo(&bar, QPoint(0, 0)).x(), 3000);

    // Bound, the standalone shield re-enables and drops the reason.
    shield.setWebView(&view);
    QVERIFY(shield.isEnabled());
    QVERIFY(!shield.toolTip().contains(QLatin1String("not available"),
                                       Qt::CaseInsensitive));

    // The panel popup still anchors under the button.  showMenu()
    // runs the menu's nested exec() loop, so a timer captures the
    // geometry and dismisses it from inside.
    shield.show();
    QVERIFY(shield.menu());
    QPoint menuPos(-1, -1);
    QTimer::singleShot(200, shield.menu(), [&shield, &menuPos]() {
        menuPos = shield.menu()->pos();
        shield.menu()->hide();
    });
    shield.showMenu();
    QVERIFY(menuPos.x() >= 0);
    QVERIFY(qAbs(menuPos.x()
            - shield.mapToGlobal(QPoint(0, shield.height())).x()) < 16);

    // The merged panel exposes the global toggle, the per-site row,
    // the live count and the settings entry.
    SitePanel *panel = shield.panel();
    QVERIFY(panel);
    QCheckBox *global = panel->findChild<QCheckBox*>(
        QLatin1String("siteContentBlocking"));
    QCheckBox *perSite = panel->findChild<QCheckBox*>(
        QLatin1String("siteBlockContent"));
    QLabel *count = panel->findChild<QLabel*>(
        QLatin1String("siteBlockedCount"));
    QPushButton *configure = panel->findChild<QPushButton*>(
        QLatin1String("siteAdBlockSettings"));
    QVERIFY(global);
    QVERIFY(perSite);
    QVERIFY(count);
    QVERIFY(configure);

    AdBlockManager *manager = AdBlockManager::instance();
    const bool wasEnabled = manager->isEnabled();
    manager->setEnabled(false);
    panel->refresh();
    QVERIFY(!global->isChecked());
    QVERIFY(count->text().contains(QLatin1String("disabled"),
                                   Qt::CaseInsensitive));
    QVERIFY(shield.toolTip().contains(QLatin1String("disabled"),
                                      Qt::CaseInsensitive));

    // The global row drives AdBlockManager::setEnabled both ways.
    global->setChecked(true);
    QVERIFY(manager->isEnabled());
    QVERIFY(!shield.toolTip().contains(QLatin1String("disabled"),
                                       Qt::CaseInsensitive));
    // No page host yet — the count line degrades gracefully.
    QVERIFY(!count->text().isEmpty());

    // The settings button opens the shared non-modal AdBlockDialog.
    configure->click();
    AdBlockDialog *dialog = nullptr;
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        AdBlockDialog *candidate = qobject_cast<AdBlockDialog*>(widget);
        if (candidate && candidate->isVisible())
            dialog = candidate;
    }
    QVERIFY(dialog);
    dialog->close();

    manager->setEnabled(wasEnabled);
}

// TRUST17: the first click on the shield must open the panel at its
// real size.  QToolButton's InstantPopup path resolves the menu's
// sizeHint() BEFORE aboutToShow to position the popup — that flush
// caches the widget-action rect from the panel's previous render and
// clears the item layout's dirty flag, and QMenu only re-runs the pass
// when an item changed since.  A refresh() that resized the panel
// while the popup was closed (here: a remembered-permission row
// landing through an out-of-band write) opened inside the stale rect —
// squashed until click 2.  Replicated by calling sizeHint() before
// popup(); the shown panel must be sized to its live sizeHint.
void tst_LocationBar::shieldPopupFirstShow()
{
    // A served page gives the view a real http origin the remembered
    // permission keys on.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QObject::connect(&server, &QTcpServer::newConnection, &server,
                     [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        const QByteArray body("<html><body>shield</body></html>");
        socket->write(QByteArray("HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/html\r\n"
                                 "Connection: close\r\n"
                                 "Content-Length: ")
                      + QByteArray::number(body.size()) + "\r\n\r\n"
                      + body);
        socket->disconnectFromHost();
    });

    SiteShieldButton shield;
    WebView view(BrowserApplication::webEngineProfile());
    shield.setWebView(&view);
    view.loadUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/")
                      .arg(server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(
        view.url().host() == QLatin1String("127.0.0.1"), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!view.page()->isLoading(), 15000);

    QMenu *menu = shield.menu();
    QVERIFY(menu);
    SitePanel *panel = shield.panel();
    QVERIFY(panel);
    QWidget *permissions = panel->findChild<QWidget*>(
        QLatin1String("sitePermissions"));
    QVERIFY(permissions);
    QVERIFY(permissions->findChildren<QPushButton*>().isEmpty());
    // The baseline must be a settled render: the panel's populate
    // runs on the web-view load signals, which can land after the
    // isLoading gate above, and the view's url() can still be a
    // transient about:blank (empty host -> empty trust verdict ->
    // hidden Trust section) when it runs.  A late refresh then
    // flips the Trust section on for the now-resolved host and the
    // shrink leg diffs against a stale hint.  Refresh until the hint
    // survives an event-pump window unchanged so both legs measure
    // the same chrome.
    QSize noRowHint;
    QElapsedTimer settle;
    settle.start();
    for (;;) {
        panel->refresh();
        const QSize hint = panel->sizeHint();
        QTest::qWait(100);
        panel->refresh();
        noRowHint = panel->sizeHint();
        if (hint == noRowHint)
            break;
        QVERIFY(settle.elapsed() < 10000);
    }

    // Grow leg: a remembered permission lands while the popup is
    // closed — written through a non-singleton manager so the panel's
    // changed() sync stays silent and the flush measures the stale
    // render (any unsignalled shift reproduces this; the permission
    // row is the deterministic, size-changing one).
    const QUrl origin(QStringLiteral("http://127.0.0.1:%1")
                      .arg(server.serverPort()));
    {
        WebPermissionManager outOfBand;
        outOfBand.setEntry(origin,
            QWebEnginePermission::PermissionType::Notifications, true);
    }

    menu->sizeHint();               // the tool button's pre-popup flush
    menu->popup(QPoint(80, 80));
    QTRY_VERIFY_WITH_TIMEOUT(menu->isVisible(), 5000);
    // The panel's minimumWidth clamps the action rect — compare
    // against the effective hint.
    QCOMPARE(panel->size(),
             panel->sizeHint().expandedTo(panel->minimumSize()));
    QVERIFY(!permissions->findChildren<QPushButton*>().isEmpty());
    const QSize grownSize = menu->size();
    menu->hide();

    // Shrink leg: revoking through the singleton emits changed(),
    // which keeps the closed panel current — the next popup's flush
    // measures the shrunken panel and it opens back at the no-row
    // size.
    WebPermissionManager::instance()->removeEntry(
        origin, QWebEnginePermission::PermissionType::Notifications);
    menu->sizeHint();
    menu->popup(QPoint(80, 80));
    QTRY_VERIFY_WITH_TIMEOUT(menu->isVisible(), 5000);
    QCOMPARE(panel->size(),
             panel->sizeHint().expandedTo(panel->minimumSize()));
    QVERIFY(permissions->findChildren<QPushButton*>().isEmpty());
    QCOMPARE(panel->sizeHint(), noRowHint);
    QVERIFY(menu->size().height() < grownSize.height());
    menu->hide();
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
    // The leg asserts suggestion behavior, not a pristine history:
    // earlier legs that served a real page (shieldPopupFirstShow)
    // leave the visited URL in the shared HistoryManager, and the
    // completer stack legitimately surfaces it below the suggestion
    // block.  What must start empty is the suggestion block itself.
    QVERIFY(model.suggestions().isEmpty());

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

    // SRCH04: the address-field context switch gates on top of the
    // per-engine opt-in — with it off, an opted-in engine still sees
    // nothing.
    manager->setSuggestionsEnabledForEngine(engine->name(), true);
    manager->setSuggestionsInAddressField(false);
    QTest::keyClicks(&bar, QLatin1String("x"));
    QTest::qWait(500);
    QCOMPARE(requestUrls.count(), requestsSeen);
    manager->setSuggestionsInAddressField(true);

    // Nickname-only mode: plain text stays silent while input that
    // leads with the engine's keyword routes to that engine.  The bar
    // accumulates every keyClicks run, so it must be cleared for the
    // keyword to actually sit at the start of the input.
    manager->setSuggestionsOnlyWithKeyword(true);
    bar.clear();
    QTest::keyClicks(&bar, QLatin1String("plain"));
    QTest::qWait(500);
    QCOMPARE(requestUrls.count(), requestsSeen);
    manager->setEngineForKeyword(QLatin1String("omni"), engine);
    bar.clear();
    QTest::keyClicks(&bar, QLatin1String("omni h"));
    QTRY_VERIFY_WITH_TIMEOUT(requestUrls.count() > requestsSeen, 3000);
    QVERIFY(requestUrls.last().contains(fixturePath));
    manager->setEngineForKeyword(QLatin1String("omni"), nullptr);
    manager->setSuggestionsOnlyWithKeyword(false);
    manager->setSuggestionsEnabledForEngine(engine->name(), false);

    disconnect(requestConn);
    manager->setCurrentEngineName(previousEngine);
    manager->removeEngine(engine->name());
    QFile::remove(fixturePath);
}

// SRCH06: a leading "@bookmarks"/"@history"/"@tabs " nickname scopes
// the omnibox dropdown to that one provider — the history block is
// fed the stripped term, engine suggestion rows never leak into the
// scoped view, a disabled or edited token stays plain text and an
// engine keyword keeps first priority.
void tst_LocationBar::omniboxScopedCompletions()
{
    // Seed one history hit and one bookmark for the scoped providers,
    // plus entries the filter term must exclude.
    HistoryManager *historyManager = BrowserApplication::historyManager();
    historyManager->clear();
    historyManager->addHistoryEntry(
        QLatin1String("http://scoped-history.example/arora"));
    historyManager->updateHistoryEntry(
        QUrl(QLatin1String("http://scoped-history.example/arora")),
        QLatin1String("Scoped History Page"));
    historyManager->addHistoryEntry(
        QLatin1String("http://unrelated.example/"));
    historyManager->updateHistoryEntry(
        QUrl(QLatin1String("http://unrelated.example/")),
        QLatin1String("Unrelated"));

    BookmarksManager *bookmarks = BookmarksManager::instance();
    BookmarkNode *bookmark = new BookmarkNode(BookmarkNode::Bookmark);
    bookmark->title = QLatin1String("Scoped Bookmark");
    bookmark->url = QLatin1String("http://scoped-bookmark.example/");
    bookmarks->addBookmark(bookmarks->bookmarks(), bookmark);

    HistoryCompletionModel *historyModel =
        new HistoryCompletionModel(this);
    historyModel->setSourceModel(historyManager->historyFilterModel());
    OmniboxCompletionModel model(historyModel);
    model.setTabEntryProvider([] {
        QList<OmniboxCompletionModel::TabEntry> tabs;
        OmniboxCompletionModel::TabEntry first;
        first.index = 0;
        first.title = QLatin1String("First Tab");
        first.url = QLatin1String("http://tab-one.example/");
        tabs.append(first);
        OmniboxCompletionModel::TabEntry second;
        second.index = 1;
        second.title = QLatin1String("Second Tab");
        second.url = QLatin1String("http://tab-two.example/");
        tabs.append(second);
        return tabs;
    });

    // Plain input stays in the merged scope.
    model.setSearchText(QLatin1String("scoped"));
    QCOMPARE(model.scope(), ScopeShortcuts::NoScope);
    QVERIFY(model.historyVisible());

    // @history scopes to history only — the stripped term filters
    // the history block itself.
    model.setSearchText(QLatin1String("@history scoped-history"));
    QCOMPARE(model.scope(), ScopeShortcuts::HistoryScope);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0),
                        HistoryModel::UrlStringRole).toString(),
             QLatin1String("http://scoped-history.example/arora"));

    // Stale suggestion rows must not leak into a scoped view.
    model.setSearchText(QLatin1String("scoped"));
    model.setSuggestions(QStringList()
                         << QLatin1String("sug-one")
                         << QLatin1String("sug-two"));
    QCOMPARE(model.suggestions().count(), 2);
    model.setSearchText(QLatin1String("@history scoped-history"));
    QCOMPARE(model.suggestions(), QStringList());

    // @bookmarks lists matching bookmarks — and nothing else.
    model.setSearchText(QLatin1String("@bookmarks scoped-bookmark"));
    QCOMPARE(model.scope(), ScopeShortcuts::BookmarksScope);
    QVERIFY(!model.historyVisible());
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0),
                        HistoryModel::UrlStringRole).toString(),
             QLatin1String("http://scoped-bookmark.example/"));
    QCOMPARE(model.data(model.index(0, 1)).toString(),
             QLatin1String("Scoped Bookmark"));
    QVERIFY(!model.data(model.index(0, 0),
                        OmniboxCompletionModel::TabIndexRole).isValid());

    // @tabs lists matching open tabs carrying their tab index.
    model.setSearchText(QLatin1String("@tabs second"));
    QCOMPARE(model.scope(), ScopeShortcuts::TabsScope);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0),
                        OmniboxCompletionModel::TabIndexRole).toInt(), 1);
    QCOMPARE(model.data(model.index(0, 0),
                        HistoryModel::UrlStringRole).toString(),
             QLatin1String("http://tab-two.example/"));

    // Back to unscoped input the merged model returns.
    model.setSearchText(QLatin1String("scoped"));
    QCOMPARE(model.scope(), ScopeShortcuts::NoScope);
    QVERIFY(model.historyVisible());

    // A disabled nickname is ordinary search text — no scoping.
    ScopeShortcuts::setEnabled(ScopeShortcuts::HistoryScope, false);
    QString rest;
    QCOMPARE(ScopeShortcuts::parse(QLatin1String("@history scoped"), &rest),
             ScopeShortcuts::NoScope);
    model.setSearchText(QLatin1String("@history scoped"));
    QCOMPARE(model.scope(), ScopeShortcuts::NoScope);
    ScopeShortcuts::setEnabled(ScopeShortcuts::HistoryScope, true);

    // An edited token takes effect — the old spelling stops scoping.
    ScopeShortcuts::setToken(ScopeShortcuts::HistoryScope,
                             QLatin1String("@hist"));
    QCOMPARE(ScopeShortcuts::parse(QLatin1String("@hist scoped"), &rest),
             ScopeShortcuts::HistoryScope);
    QCOMPARE(rest, QLatin1String("scoped"));
    QCOMPARE(ScopeShortcuts::parse(QLatin1String("@history scoped")),
             ScopeShortcuts::NoScope);
    ScopeShortcuts::setToken(ScopeShortcuts::HistoryScope,
                             QLatin1String("@history"));

    // Engine keyword search keeps first priority: a token that is
    // also a keyword must not scope.
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = new OpenSearchEngine;
    engine->setName(QLatin1String("scope-shortcut-test"));
    engine->setSearchUrlTemplate(
        QLatin1String("http://scope-shortcut.invalid/q={searchTerms}"));
    if (manager->engineExists(engine->name()))
        manager->removeEngine(engine->name());
    QVERIFY(manager->addEngine(engine));
    manager->setEngineForKeyword(QLatin1String("@history"), engine);
    QCOMPARE(ScopeShortcuts::parse(QLatin1String("@history scoped")),
             ScopeShortcuts::NoScope);
    model.setSearchText(QLatin1String("@history scoped"));
    QCOMPARE(model.scope(), ScopeShortcuts::NoScope);
    manager->setEngineForKeyword(QLatin1String("@history"), nullptr);
    manager->removeEngine(engine->name());

    bookmarks->removeBookmark(bookmark);
    historyManager->removeHistoryEntry(
        QUrl(QLatin1String("http://scoped-history.example/arora")));
    historyManager->removeHistoryEntry(
        QUrl(QLatin1String("http://unrelated.example/")));
    ScopeShortcuts::reset();
}

// SAFE03: the registrable-domain character range inside an encoded
// url — scheme, subdomains, userinfo, port and path are the dimmed
// parts; the eTLD+1 range is what a phishing reader must see.
void tst_LocationBar::domainEmphasis_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("domain"); // empty => no range

    QTest::newRow("phishing-subdomain")
        << QString("http://paypal.com.evil.tld/login")
        << QString("evil.tld");
    QTest::newRow("two-level-tld")
        << QString("https://www.example.co.uk/path?q=1#f")
        << QString("example.co.uk");
    QTest::newRow("plain-host")
        << QString("http://example.com") << QString("example.com");
    QTest::newRow("deep-subdomains")
        << QString("http://a.b.c.example.com/")
        << QString("example.com");
    QTest::newRow("userinfo")
        << QString("http://user:pass@www.bank.com/")
        << QString("bank.com");
    QTest::newRow("ipv4")
        << QString("http://192.168.0.1:8080/x")
        << QString("192.168.0.1");
    QTest::newRow("ipv6")
        << QString("http://[2001:db8::1]:8443/")
        << QString("[2001:db8::1]");
    QTest::newRow("localhost")
        << QString("http://localhost:9/") << QString("localhost");
    QTest::newRow("no-subdomain")
        << QString("http://example.co.uk") << QString("example.co.uk");
    QTest::newRow("bare-suffix")
        << QString("http://co.uk/") << QString("co.uk");
    QTest::newRow("punycode-host")
        << QString("https://xn--tst-qla.example.de/")
        << QString("example.de");
    QTest::newRow("fqdn-root-dot")
        << QString("http://example.com./root")
        << QString("example.com");
    QTest::newRow("uppercase")
        << QString("HTTP://WWW.EXAMPLE.CO.UK/")
        << QString("EXAMPLE.CO.UK");
    QTest::newRow("about-blank") << QString("about:blank")
                                 << QString();
    QTest::newRow("file-no-host") << QString("file:///tmp/x")
                                  << QString();
    QTest::newRow("no-authority") << QString("qrc:/startpage.html")
                                  << QString();
    QTest::newRow("internal-empty-authority")
        << QString("arora-file:///tmp/") << QString();
    QTest::newRow("plain-text") << QString("just words")
                                << QString();
}

void tst_LocationBar::domainEmphasis()
{
    QFETCH(QString, text);
    QFETCH(QString, domain);

    int start = -1;
    int length = 0;
    const bool ok =
        LocationBar::registrableDomainRange(text, start, length);
    if (domain.isEmpty()) {
        QVERIFY(!ok);
        return;
    }
    QVERIFY(ok);
    QCOMPARE(text.mid(start, length), domain);
}

// SAFE03(b): an internationalized host is stored/displayed as
// punycode; the tooltip offers the Unicode form.
void tst_LocationBar::punycodeDisplay()
{
    QCOMPARE(LocationBar::unicodeUrlHint(
                 QLatin1String("http://xn--mnchen-3ya.de/")),
             QString::fromUtf8("http://münchen.de/"));
    QVERIFY(LocationBar::unicodeUrlHint(
                QLatin1String("http://example.com/")).isEmpty());
    QVERIFY(LocationBar::unicodeUrlHint(
                QLatin1String("not a url")).isEmpty());
    QVERIFY(LocationBar::unicodeUrlHint(
                QLatin1String("http://192.168.0.1/")).isEmpty());

    // A real url delivered to the bar renders ACE in the field.
    TestLocationBar bar;
    WebView view(BrowserApplication::webEngineProfile());
    bar.setWebView(&view);
    const QUrl idn(QString::fromUtf8("http://münchen.de/"));
    QVERIFY(QMetaObject::invokeMethod(&bar, "webViewUrlChanged",
                                    Q_ARG(QUrl, idn)));
    QCOMPARE(bar.text(), QLatin1String("http://xn--mnchen-3ya.de/"));
    QCOMPARE(bar.cursorPosition(), 0);

    // The ToolTip event surfaces the Unicode form through QToolTip.
    const auto findTip = []() -> QLabel * {
        for (QWidget *widget : qApp->topLevelWidgets()) {
            if (widget->inherits("QTipLabel") && widget->isVisible())
                return qobject_cast<QLabel*>(widget);
        }
        return nullptr;
    };
    bar.sendToolTip();
    QLabel *tip = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((tip = findTip()) != nullptr, 3000);
    QVERIFY(tip->text().contains(QString::fromUtf8("münchen")));
    QToolTip::hideText();
}

// SAFE03 render check: the unfocused bar must really repaint the url
// with the domain at full text strength and the rest faded — assert
// on the painted pixels, not just the range maths.
void tst_LocationBar::domainEmphasisPaint()
{
    QWidget window;
    QVBoxLayout layout(&window);
    TestLocationBar *bar = new TestLocationBar(&window);
    layout.addWidget(bar);
    QLineEdit *focusSponge = new QLineEdit(&window);
    layout.addWidget(focusSponge);
    window.resize(520, window.sizeHint().height());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    focusSponge->setFocus();
    qApp->processEvents();
    QVERIFY(!bar->hasFocus());

    const QString url = QLatin1String("http://paypal.com.evil.tld/login");
    bar->setText(url);
    bar->setCursorPosition(0);

    int start = -1;
    int length = 0;
    QVERIFY(LocationBar::registrableDomainRange(url, start, length));
    QCOMPARE(url.mid(start, length), QLatin1String("evil.tld"));

    const QImage image = bar->grab().toImage();
    QVERIFY(!image.isNull());
    const QRect textRect = bar->textContentsRect();
    const QFontMetrics fm = bar->fontMetrics();
    const int domainX =
        textRect.x() + fm.horizontalAdvance(url.left(start));
    const int domainEnd =
        domainX + fm.horizontalAdvance(url.mid(start, length));

    // Closest any pixel in the range gets to the full text color.
    const QColor strong = bar->palette().color(QPalette::Text);
    const auto closestToStrong = [&](int fromX, int toX) {
        int best = 255 * 3;
        for (int x = fromX; x < qMin(toX, image.width()); ++x) {
            for (int y = textRect.y();
                 y <= qMin(textRect.bottom(), image.height() - 1); ++y) {
                const QColor pixel = image.pixelColor(x, y);
                const int diff = qAbs(pixel.red() - strong.red())
                    + qAbs(pixel.green() - strong.green())
                    + qAbs(pixel.blue() - strong.blue());
                best = qMin(best, diff);
            }
        }
        return best;
    };

    const int prefixDiff = closestToStrong(textRect.x(), domainX);
    const int domainDiff = closestToStrong(domainX, domainEnd);
    const int suffixDiff =
        closestToStrong(domainEnd, textRect.right() + 1);

    // Domain glyphs keep (near) full-strength text color; the
    // surrounding scheme/subdomain/path runs are visibly dimmed.
    QVERIFY(domainDiff <= 60);
    QVERIFY(prefixDiff > domainDiff + 20);
    QVERIFY(suffixDiff > domainDiff + 20);
}

QTEST_MAIN(tst_LocationBar)
#include "tst_locationbar.moc"
