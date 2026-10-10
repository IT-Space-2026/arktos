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

// COV03: WebView itself — zoom ladder, javascript: loadUrl, Ctrl+wheel
// zoom, navigation mouse buttons, drag & drop url loading, status bar
// text, and in-page find through WebViewSearch / findText.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <QtNetwork/QtNetwork>
#include <qwebenginecontextmenurequest.h>
#include <qwebenginefindtextresult.h>
#include <qwebenginedownloadrequest.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineview.h>
#include <qmimedata.h>
#include <qfile.h>
#include <qdir.h>
#include <qlineedit.h>
#include <qlabel.h>
#include <qtoolbutton.h>
#include <qmenu.h>
#include <qclipboard.h>
#include <qpointer.h>
#include <qdialog.h>
#include <qset.h>

#include <functional>
#include <memory>

#include "webview.h"
#include "webpage.h"
#include "webviewsearch.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserprofile.h"
#include "historymanager.h"
#include "tabwidget.h"
#include "tormanager.h"
#include "qtest_arora.h"
#include "qtry.h"

// Exposes the protected event handlers for direct dispatch.
class TestWebView : public WebView
{
public:
    TestWebView(QWidget *parent = nullptr)
        : WebView(parent)
    {
    }

    TestWebView(QWebEngineProfile *profile, QWidget *parent = nullptr)
        : WebView(profile, parent)
    {
    }

    void sendWheel(int degrees, Qt::KeyboardModifiers modifiers)
    {
        QWheelEvent event(QPointF(1, 1), QPointF(1, 1),
                          QPoint(0, 0), QPoint(0, degrees * 8),
                          Qt::NoButton, modifiers, Qt::NoScrollPhase,
                          false);
        wheelEvent(&event);
    }

    void sendMousePress(Qt::MouseButton button)
    {
        QMouseEvent event(QEvent::MouseButtonPress, QPointF(1, 1),
                          QPointF(1, 1), button, button,
                          Qt::NoModifier);
        mousePressEvent(&event);
    }

    void sendMouseRelease(Qt::MouseButton button)
    {
        QMouseEvent event(QEvent::MouseButtonRelease, QPointF(1, 1),
                          QPointF(1, 1), button, Qt::NoButton,
                          Qt::NoModifier);
        mouseReleaseEvent(&event);
    }

    bool sendDragEnter(QMimeData *mimeData)
    {
        QDragEnterEvent event(QPointF(1, 1), Qt::CopyAction, mimeData,
                              Qt::LeftButton, Qt::NoModifier);
        dragEnterEvent(&event);
        return event.isAccepted();
    }

    void sendDragMove(QMimeData *mimeData, bool sourceIsSelf)
    {
        QDragMoveEvent event(QPointF(1, 1), Qt::CopyAction, mimeData,
                             Qt::LeftButton, Qt::NoModifier);
        Q_UNUSED(sourceIsSelf);
        dragMoveEvent(&event);
    }

    void sendDrop(QMimeData *mimeData)
    {
        QDropEvent event(QPointF(1, 1), Qt::CopyAction, mimeData,
                         Qt::LeftButton, Qt::NoModifier);
        dropEvent(&event);
    }
};

// CONT08: minimal loopback HTTP responder that records every request
// target AND its Referer header — a cross-profile open that leaked
// the source page's url would show up on the wire here, not just in
// the request object.
class RecordedHttpServer : public QObject
{
    Q_OBJECT

public:
    struct Request {
        QString target;
        QString referer;
    };

    QList<Request> requests;
    QByteArray indexHtml;

    explicit RecordedHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() {
                if (!socket->peek(16384).contains("\r\n\r\n"))
                    return;
                respond(socket, socket->readAll());
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url(const QString &path) const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1%2")
                    .arg(m_server.serverPort()).arg(path));
    }

    QList<Request> requestsFor(const QString &target) const
    {
        QList<Request> out;
        for (const Request &request : requests)
            if (request.target == target)
                out.append(request);
        return out;
    }

private:
    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QList<QByteArray> lines = request.split('\n');
        Request record;
        record.target = QString::fromUtf8(
            lines.value(0).split(' ').value(1));
        for (const QByteArray &line : lines) {
            if (line.startsWith("Referer:"))
                record.referer = QString::fromUtf8(
                    line.mid(8).trimmed());
        }
        requests.append(record);

        const QByteArray body = indexHtml.isEmpty()
            ? QByteArray("<html><body>ok</body></html>") : indexHtml;
        socket->write("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                      "Content-Length: "
                          + QByteArray::number(body.size())
                          + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
};

// Resets the process-global privacy flags no matter which assertion
// exits the test early — a leaked setPrivate()/setTorMode() would
// poison every later case.
struct PrivacyFlagGuard {
    ~PrivacyFlagGuard()
    {
        BrowserApplication::setPrivate(false);
        BrowserApplication::setTorMode(false);
    }
};

// Deletes every window the test spawned, including ones still open
// when an assertion ends the function early.
struct WindowListGuard {
    QList<QPointer<BrowserMainWindow>> windows;
    ~WindowListGuard()
    {
        for (const QPointer<BrowserMainWindow> &window : windows)
            if (window)
                delete window;
    }
};

class tst_WebView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void zoom();
    void wheelZoom();
    void loadUrl();
    void statusBarText();
    void mouseButtons();
    void dropUrl();
    void dropJavascriptUrl();
    void middleClickPaste();
    void middleClickAutoscroll();
    void findText();
    void forceDarkMode();
    void webViewWithSearch();
    void contextMenuLinkActions();
    void contextMenuLinkPrivateTorActions();
    void contextMenuLinkCrossProfileMatrix();
    void linkOpenRefererDiscipline();
    void contextMenuPageActions();
    void contextMenuImageActions();
    void contextMenuImageLinkActions();
    void contextMenuCanvasActions();
    void contextMenuVideoPosterActions();
    void contextMenuBlobImage();
};

// POL01 helpers — the context menu is exec()d inside the right-button
// press delivery, so a repeating timer has to find the popup inside
// the nested loop and drive it there.
static QAction *findMenuAction(QMenu *menu, const QString &text)
{
    const QList<QAction *> actions = menu->actions();
    for (QAction *action : actions) {
        if (action->text().remove(QLatin1Char('&')) == text)
            return action;
    }
    return nullptr;
}

// Sends a real right-click at pos (view coordinates) through the
// render delegate so Chromium issues a genuine context-menu request,
// then calls inspect() on the popped QMenu.  Returns whether a popup
// appeared.  inspect() may trigger() an action — it runs inside the
// menu's nested exec loop, so record verdicts, don't QVERIFY there.
static bool driveRightClick(WebView *view, const QPoint &pos,
                            const std::function<void(QMenu *)> &inspect)
{
    QWidget *proxy = view->focusProxy() ? view->focusProxy() : view;
    bool seen = false;
    QTimer timer;
    timer.setInterval(30);
    QObject::connect(&timer, &QTimer::timeout, qApp, [&]() {
        if (seen)
            return;
        QMenu *menu = qobject_cast<QMenu *>(
            QApplication::activePopupWidget());
        if (!menu)
            return;
        seen = true;
        inspect(menu);
        // The menu may already be closed-and-deleted when inspect()
        // triggered an action — QPointer guards the check.
        if (QPointer<QMenu>(menu))
            menu->close();
    });
    timer.start();
    // Chromium raises the context menu on press (Linux) or release —
    // cover both while the timer is armed.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                      proxy->mapToGlobal(pos),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(proxy, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos),
                        proxy->mapToGlobal(pos),
                        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(proxy, &release);
    // The engine posts the context-menu request asynchronously — keep
    // pumping until the popup shows (its exec() runs the timer inside
    // a nested loop) or the cap elapses.
    for (int waited = 0; !seen && waited < 5000; waited += 50)
        QTest::qWait(50);
    timer.stop();
    return seen;
}

// All five CONT07 link-open entries start with 'Open in New' —
// records order and enabled-state of each.
static void collectOpenEntries(QMenu *menu, QStringList *order,
                               QHash<QString, bool> *enabled)
{
    const QList<QAction *> actions = menu->actions();
    for (QAction *action : actions) {
        const QString text = action->text().remove(QLatin1Char('&'));
        if (text.startsWith(QLatin1String("Open in New"))) {
            order->append(text);
            enabled->insert(text, action->isEnabled());
        }
    }
}

// CONT08 matrix helpers — plain wait loops, no QTest macros, so a
// timeout surfaces as a clean nullptr/empty return the test can
// QVERIFY on.
static QSet<WebView *> tabViews(TabWidget *tabs)
{
    QSet<WebView *> views;
    for (int i = 0; i < tabs->count(); ++i)
        if (WebView *view = tabs->webView(i))
            views.insert(view);
    return views;
}

static WebView *awaitNewTabView(TabWidget *tabs,
                                const QSet<WebView *> &before)
{
    const auto deadline =
        QDateTime::currentMSecsSinceEpoch() + 5000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        for (int i = 0; i < tabs->count(); ++i) {
            WebView *view = tabs->webView(i);
            if (view && !before.contains(view))
                return view;
        }
        QTest::qWait(50);
    }
    return nullptr;
}

static BrowserMainWindow *awaitNewWindow(
        BrowserApplication *application,
        const QList<BrowserMainWindow *> &before)
{
    const auto deadline =
        QDateTime::currentMSecsSinceEpoch() + 5000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        const QList<BrowserMainWindow *> windows =
            application->mainWindows();
        for (BrowserMainWindow *window : windows)
            if (!before.contains(window))
                return window;
        QTest::qWait(50);
    }
    return nullptr;
}

// The 'blob' anchor's href is minted by page script — poll until it
// is a blob: url (or give up, in which case the blob menu cell is
// skipped: the slot-level probes still lock the scheme).
static bool awaitBlobHref(WebView *view)
{
    std::shared_ptr<bool> probed(new bool(false));
    std::shared_ptr<QString> href(new QString);
    const auto deadline =
        QDateTime::currentMSecsSinceEpoch() + 15000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        view->page()->runJavaScript(
            QLatin1String("document.getElementById('blob').href"),
            [probed, href](const QVariant &result) {
                *href = result.toString();
                *probed = true;
            });
        const auto innerDeadline =
            QDateTime::currentMSecsSinceEpoch() + 5000;
        while (!*probed
               && QDateTime::currentMSecsSinceEpoch() < innerDeadline)
            QTest::qWait(50);
        *probed = false;
        if (href->startsWith(QLatin1String("blob:")))
            return true;
        QTest::qWait(100);
    }
    return false;
}

// CTX01 detached-view helpers — defined below ahead of the image
// tests; the CONT07 link test uses them too.
static WebView *findDetachedView(WebView *source);
static WebView *awaitDetachedView(WebView *source, const QString &scheme);
static void closeDetached(WebView *view);

void tst_WebView::initTestCase()
{
    QCoreApplication::setApplicationName("tst_webview");
    QSettings settings;
    settings.clear();
}

// The Firefox-style zoom ladder walks fixed percentage steps.
void tst_WebView::zoom()
{
    TestWebView view;
    QCOMPARE(view.zoomFactor(), qreal(1.0));
    QCOMPARE(view.progress(), 0);

    view.zoomIn();
    QVERIFY(view.zoomFactor() > 1.0);
    view.zoomOut();
    QCOMPARE(view.zoomFactor(), qreal(1.0));
    view.zoomOut();
    QVERIFY(view.zoomFactor() < 1.0);
    view.resetZoom();
    QCOMPARE(view.zoomFactor(), qreal(1.0));

    // Clamped at both ends of the ladder.
    for (int i = 0; i < 20; ++i)
        view.zoomIn();
    const qreal top = view.zoomFactor();
    view.zoomIn();
    QCOMPARE(view.zoomFactor(), top);
    view.resetZoom();
    for (int i = 0; i < 20; ++i)
        view.zoomOut();
    const qreal bottom = view.zoomFactor();
    view.zoomOut();
    QCOMPARE(view.zoomFactor(), bottom);
}

// Ctrl+wheel detents zoom in steps of ten.
void tst_WebView::wheelZoom()
{
    TestWebView view;
    view.sendWheel(120, Qt::ControlModifier);
    QVERIFY(view.zoomFactor() > 1.0);
    view.sendWheel(-120, Qt::ControlModifier);
    QCOMPARE(view.zoomFactor(), qreal(1.0));

    // No modifier: handled by the base class, zoom untouched.
    view.sendWheel(120, Qt::NoModifier);
    QCOMPARE(view.zoomFactor(), qreal(1.0));
}

void tst_WebView::loadUrl()
{
    TestWebView view;
    QSignalSpy titleSpy(&view, SIGNAL(titleChanged(QString)));

    // An empty title announces "Loading...", a given one announces
    // itself.
    const QUrl url(QStringLiteral("data:text/html,<p>x</p>"));
    view.loadUrl(url);
    QVERIFY(titleSpy.count() >= 1);

    // url() falls back to the requested url while the load is pending.
    QTRY_VERIFY_WITH_TIMEOUT(
        view.url().toString().startsWith(QLatin1String("data:")),
        15000);

    // javascript: urls are executed in place, never navigated.
    const QUrl before = view.url();
    QTRY_VERIFY_WITH_TIMEOUT(!before.isEmpty(), 15000);
    view.loadUrl(QUrl(QLatin1String("javascript:void(0)")));
    QTest::qWait(250);
    QCOMPARE(view.url(), before);
}

void tst_WebView::statusBarText()
{
    TestWebView view;
    QSignalSpy spy(&view, SIGNAL(statusBarMessage(QString)));

    // linkHovered feeds setStatusBarText; a load clears it.
    view.loadUrl(QUrl(QStringLiteral("data:text/html,<p>x</p>")));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 15000);
}

// Back/forward mouse buttons and the modifier stash used by
// acceptNavigationRequest.
void tst_WebView::mouseButtons()
{
    TestWebView view;
    view.loadUrl(QUrl(QStringLiteral("data:text/html,<p>x</p>")));
    QTRY_VERIFY_WITH_TIMEOUT(!view.url().isEmpty(), 15000);

    BrowserApplication *application = BrowserApplication::instance();
    view.sendMousePress(Qt::LeftButton);
    QCOMPARE(application->eventMouseButtons(), Qt::LeftButton);

    // XButton1/XButton2 drive Back/Forward page actions (no-ops on a
    // fresh history).
    view.sendMousePress(Qt::XButton1);
    view.sendMousePress(Qt::XButton2);
    view.sendMouseRelease(Qt::LeftButton);
}

// Dropping a url on the view loads it.
void tst_WebView::dropUrl()
{
    TestWebView view;

    QMimeData mime;
    mime.setUrls(QList<QUrl>()
        << QUrl(QStringLiteral("data:text/html,<p>dropped</p>")));
    QVERIFY(view.sendDragEnter(&mime));
    view.sendDragMove(&mime, false);

    QSignalSpy spy(&view, SIGNAL(urlChanged(QUrl)));
    view.sendDrop(&mime);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 15000);
    QCOMPARE(view.url(),
             QUrl(QStringLiteral("data:text/html,<p>dropped</p>")));

    // A drop with neither urls nor text decodes to nothing — the
    // view leaves the current page alone.
    QMimeData opaqueMime;
    opaqueMime.setData(QLatin1String("application/x-arora-test"),
                       QByteArray("payload"));
    const QUrl before = view.url();
    view.sendDrop(&opaqueMime);
    QTest::qWait(250);
    QCOMPARE(view.url(), before);
}

// SEC02: a dropped javascript: url must not run its script in the
// page's origin — drops that are urls navigate, scripts do not run.
void tst_WebView::dropJavascriptUrl()
{
    TestWebView view;
    view.loadUrl(QUrl(QStringLiteral("data:text/html,<p>x</p>")));
    QTRY_VERIFY_WITH_TIMEOUT(!view.url().isEmpty(), 15000);
    const QUrl before = view.url();

    QMimeData mime;
    mime.setUrls(QList<QUrl>() << QUrl(
        QLatin1String("javascript:window.__dropped=42;void(0)")));
    view.sendDrop(&mime);
    QTest::qWait(300);
    QCOMPARE(view.url(), before);

    // The text fallback path is guarded too.
    QMimeData textMime;
    textMime.setText(QLatin1String("javascript:window.__dropped=43"));
    view.sendDrop(&textMime);
    QTest::qWait(300);
    QCOMPARE(view.url(), before);

    // Prove no script ran in this page.
    std::shared_ptr<bool> probed(new bool(false));
    std::shared_ptr<QVariant> marker(new QVariant);
    view.page()->runJavaScript(QLatin1String("window.__dropped"),
        [probed, marker](const QVariant &result) {
            *marker = result;
            *probed = true;
        });
    QTRY_VERIFY_WITH_TIMEOUT(*probed, 60000);
    QVERIFY(!marker->isValid());
}

// X11 middle-click paste loads the PRIMARY selection as a url —
// except javascript:, which would execute in the current page.
void tst_WebView::middleClickPaste()
{
    TestWebView view;
    view.loadUrl(QUrl(QStringLiteral("data:text/html,<p>x</p>")));
    QTRY_VERIFY_WITH_TIMEOUT(!view.url().isEmpty(), 15000);
    const QUrl before = view.url();

    QClipboard *clipboard = QApplication::clipboard();
    const QString scriptUrl = QLatin1String("javascript:void(0)");
    clipboard->setText(scriptUrl, QClipboard::Selection);
    if (clipboard->text(QClipboard::Selection) != scriptUrl)
        QSKIP("the offscreen clipboard has no Selection mode");

    view.sendMouseRelease(Qt::MiddleButton);
    QTest::qWait(300);
    QCOMPARE(view.url(), before);

    // A plain url in the selection still loads.
    const QString httpUrl = QStringLiteral("data:text/html,<p>pasted</p>");
    clipboard->setText(httpUrl, QClipboard::Selection);
    view.sendMouseRelease(Qt::MiddleButton);
    QTRY_VERIFY_WITH_TIMEOUT(view.url().toString() == httpUrl, 15000);
}

// POL03: middle-click autoscroll — pressing the middle button on
// empty page content and moving the pointer scrolls the document;
// releasing ends the session.  The Blink feature flag is armed in
// initTestCase (engine-latched), mirroring the shipping default.
void tst_WebView::middleClickAutoscroll()
{
    TestWebView view;
    view.resize(800, 600);
    view.show();
    std::shared_ptr<bool> loaded(new bool(false));
    QObject::connect(&view, &QWebEngineView::loadFinished, &view,
                     [loaded](bool) { *loaded = true; });
    view.setHtml(QStringLiteral(
        "<html><body style='margin:0;height:4000px'>"
        "<div style='height:200px'>top</div></body></html>"));
    QTRY_VERIFY_WITH_TIMEOUT(*loaded, 15000);

    QWidget *proxy = view.focusProxy();
    QVERIFY(proxy);
    const QPoint pos(400, 300);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos),
                      proxy->mapToGlobal(pos), Qt::MiddleButton,
                      Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(proxy, &press);
    // Autoscroll engages once the pointer leaves the dead zone around
    // the press point; each step below the origin scrolls further.
    for (int i = 0; i < 10; ++i) {
        const QPoint moved = pos + QPoint(0, 15 + i * 15);
        QMouseEvent move(QEvent::MouseMove, QPointF(moved),
                         proxy->mapToGlobal(moved), Qt::NoButton,
                         Qt::MiddleButton, Qt::NoModifier);
        QCoreApplication::sendEvent(proxy, &move);
        QTest::qWait(80);
    }
    QTest::qWait(800);

    std::shared_ptr<bool> probed(new bool(false));
    std::shared_ptr<int> scrollY(new int(-1));
    view.page()->runJavaScript(QLatin1String("window.scrollY"),
        [probed, scrollY](const QVariant &value) {
        *scrollY = value.toInt();
        *probed = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(*probed, 15000);
    QVERIFY2(*scrollY > 0,
             qPrintable(QStringLiteral("middle-click autoscroll did not"
                                       " move the page (scrollY=%1)")
                        .arg(*scrollY)));

    // A click ends any sticky autoscroll session for later tests.
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos),
                        proxy->mapToGlobal(pos), Qt::MiddleButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(proxy, &release);
    QMouseEvent click(QEvent::MouseButtonPress, QPointF(pos),
                      proxy->mapToGlobal(pos), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(proxy, &click);
}

// In-page find through WebViewSearch — QWebEnginePage::findText is
// asynchronous; the n/m indicator reports active match and total,
// "Not Found" stands in for zero.
void tst_WebView::findText()
{
    TestWebView view;
    // Chromium skips text finding on a hidden WebContents — the view
    // must be visible (still offscreen under QT_QPA_PLATFORM).
    view.resize(800, 600);
    view.show();
    std::shared_ptr<bool> loaded(new bool(false));
    QObject::connect(&view, &QWebEngineView::loadFinished, &view,
                     [loaded](bool) { *loaded = true; });
    view.setHtml(QStringLiteral(
        "<html><body>hello arora hello</body></html>"));
    QTRY_VERIFY_WITH_TIMEOUT(*loaded, 15000);

    WebViewSearch search(&view);
    QLineEdit *edit = search.findChild<QLineEdit*>(
        QLatin1String("searchLineEdit"));
    QVERIFY(edit);
    QLabel *info = search.findChild<QLabel*>(QLatin1String("searchInfo"));
    QVERIFY(info);

    // Two "hello" occurrences — the counter walks n/m as the active
    // match moves, wrapping past the last one.  This runs before the
    // raw probe below because every findText() moves the renderer's
    // active match, which seeds where the next find lands.
    edit->setText(QLatin1String("hello"));
    search.findNext();
    QTRY_VERIFY_WITH_TIMEOUT(info->text() == QLatin1String("1/2"), 15000);
    search.findNext();
    QTRY_VERIFY_WITH_TIMEOUT(info->text() == QLatin1String("2/2"), 15000);
    search.findPrevious();
    QTRY_VERIFY_WITH_TIMEOUT(info->text() == QLatin1String("1/2"), 15000);

    edit->setText(QLatin1String("no-such-text-xyz"));
    search.findNext();
    QTRY_VERIFY_WITH_TIMEOUT(
        info->text() == WebViewSearch::tr("Not Found"), 15000);

    // Clearing the field clears the counter along with the highlights.
    edit->setText(QString());
    search.findNext();
    QVERIFY(info->text().isEmpty());

    // highlightAll toggles between re-running the find and clearing
    // highlights; switching it off clears the label too.
    QToolButton *highlight = search.findChild<QToolButton*>(
        QLatin1String("highlightAllButton"));
    QVERIFY(highlight);
    edit->setText(QLatin1String("arora"));
    highlight->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(info->text() == QLatin1String("1/1"), 15000);
    highlight->setChecked(false);
    QVERIFY(info->text().isEmpty());
}

// POL03: ForceDarkMode — Chromium's auto-dark inverts light pages.
// (It does not flip prefers-color-scheme — the media query still
// follows the OS theme — so the visible check is the composited
// pixel.)  A scratch off-the-record profile keeps the shared one
// untouched.
void tst_WebView::forceDarkMode()
{
    const QString whitePage = QStringLiteral(
        "data:text/html,<html><body style='background:#fff'>x</body></html>");

    // Control: an untouched profile renders the page light.
    {
        QWebEngineProfile profile;
        TestWebView view(&profile);
        view.resize(800, 600);
        view.show();
        std::shared_ptr<bool> loaded(new bool(false));
        QObject::connect(&view, &QWebEngineView::loadFinished, &view,
                         [loaded](bool) { *loaded = true; });
        view.loadUrl(QUrl(whitePage));
        QTRY_VERIFY_WITH_TIMEOUT(*loaded, 15000);
        QTest::qWait(500);
        const QColor pixel = view.grab().toImage().pixelColor(400, 300);
        QVERIFY2(pixel.lightnessF() > 0.5,
                 qPrintable(QStringLiteral("control page composited"
                                           " dark (%1)")
                            .arg(pixel.name())));
    }

    QWebEngineProfile profile;
    QVERIFY(!profile.settings()->testAttribute(
        QWebEngineSettings::ForceDarkMode));
    profile.settings()->setAttribute(QWebEngineSettings::ForceDarkMode,
                                     true);
    QVERIFY(profile.settings()->testAttribute(
        QWebEngineSettings::ForceDarkMode));

    TestWebView view(&profile);
    view.resize(800, 600);
    view.show();
    std::shared_ptr<bool> loaded(new bool(false));
    QObject::connect(&view, &QWebEngineView::loadFinished, &view,
                     [loaded](bool) { *loaded = true; });
    view.loadUrl(QUrl(whitePage));
    QTRY_VERIFY_WITH_TIMEOUT(*loaded, 15000);

    // The inversion is visible output: the white body must composite
    // as a dark pixel (POL02's software rasterizer keeps offscreen
    // grabs real).
    QTest::qWait(500);
    const QColor pixel = view.grab().toImage().pixelColor(400, 300);
    QVERIFY2(pixel.lightnessF() < 0.5,
             qPrintable(QStringLiteral("forced dark mode left the page"
                                       " light (%1)")
                        .arg(pixel.name())));
}

void tst_WebView::webViewWithSearch()
{
    // WebViewWithSearch reparents the view into itself, so the view
    // must outlive stack scope — heap-allocated, owned by the wrapper.
    TestWebView *view = new TestWebView;
    WebViewWithSearch withSearch(view);
    QCOMPARE(withSearch.m_webView, static_cast<WebView*>(view));
    QVERIFY(withSearch.m_webViewSearch);
    QVERIFY(withSearch.layout());
}

// POL01: a real right-click on a link must offer 'Copy Clean Link'
// (which strips tracking parameters) and the page QR share action.
void tst_WebView::contextMenuLinkActions()
{
    TestWebView view;
    view.resize(800, 600);
    view.show();

    const QString html = QStringLiteral(
        "<html><body><a href='https://example.com/path"
        "?utm_source=news&amp;fbclid=zz&amp;id=42' "
        "style='position:fixed;left:0;top:0;display:block;"
        "width:300px;height:60px'>link</a></body></html>");
    const QUrl url(QStringLiteral("data:text/html,")
        + QString::fromUtf8(QUrl::toPercentEncoding(html)));
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(url);
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    bool foundClean = false;
    bool foundQr = false;
    const bool popped = driveRightClick(&view, QPoint(30, 30),
                                        [&](QMenu *menu) {
        if (QAction *clean = findMenuAction(
                menu, QStringLiteral("Copy Clean Link"))) {
            foundClean = true;
            clean->trigger();
        }
        foundQr = findMenuAction(
            menu, QStringLiteral("Show QR Code for This Page"))
            != nullptr;
    });
    QVERIFY2(popped, "no context menu on link right-click");
    QVERIFY(foundClean);
    QVERIFY(foundQr);
    QCOMPARE(QApplication::clipboard()->text(),
             QStringLiteral("https://example.com/path?id=42"));
}

// CONT07: a link right-click offers the full cross-context open set —
// New Tab, New Window, New Private Tab, New Private Window, New Tor
// Window — in that order.  The private entries disappear wherever
// they could not honestly deliver a fresh isolated context (global
// private mode and tor mode), and a dangerous-scheme link disables
// every open entry.  Opens land where they claim: the private tab is
// a real off-the-record view, the private window is a real
// BrowserMainWindow whose first tab is OTR.
void tst_WebView::contextMenuLinkPrivateTorActions()
{
    // The strict gate all five targets funnel through.
    QVERIFY(WebView::isUrlAllowedFromPageLink(
        QUrl(QLatin1String("https://example.com/"))));
    QVERIFY(WebView::isUrlAllowedFromPageLink(
        QUrl(QLatin1String("file:///tmp/x"))));
    QVERIFY(!WebView::isUrlAllowedFromPageLink(
        QUrl(QLatin1String("javascript:alert(1)"))));
    QVERIFY(!WebView::isUrlAllowedFromPageLink(
        QUrl(QLatin1String("JAVASCRIPT:alert(1)"))));
    QVERIFY(!WebView::isUrlAllowedFromPageLink(
        QUrl(QLatin1String("data:text/html,<h1>x</h1>"))));
    QVERIFY(!WebView::isUrlAllowedFromPageLink(
        QUrl(QLatin1String("blob:https://example.com/uuid"))));
    // The base gate stays permissive — internally generated data:
    // urls (the canvas serializer) still ride it.
    QVERIFY(WebView::isUrlAllowedOnUntrustedInput(
        QUrl(QLatin1String("data:text/plain,x"))));

    // A file: link target loads without a network, so the opened
    // view's url settles deterministically.
    const QString fixturePath = QDir::temp().filePath(
        QLatin1String("arora-cont07-target.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>cont07-target</title>"
                      "</head><body>t</body></html>");
    }
    const QUrl linkTarget = QUrl::fromLocalFile(fixturePath);

    // On the app's named browsing profile — the default-constructed
    // WebPage binds QWebEngineProfile::defaultProfile(), which is
    // itself off-the-record in Qt6 and would make the same-context
    // opens look private.
    TestWebView view(BrowserApplication::webEngineProfile());
    view.resize(800, 600);
    view.show();
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    const QString html = QStringLiteral(
        "<html><body>"
        "<a href='%1' style='position:fixed;left:0;top:0;display:block;"
        "width:300px;height:60px'>link</a>"
        "<a href='javascript:alert(1)' style='position:fixed;left:0;"
        "top:80px;display:block;width:300px;height:60px'>bad</a>"
        "</body></html>").arg(linkTarget.toString());
    view.loadUrl(QUrl(QStringLiteral("data:text/html,")
        + QString::fromUtf8(QUrl::toPercentEncoding(html))));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    // Menu shape and order on an ordinary link.
    QStringList order;
    QHash<QString, bool> enabled;
    const bool popped = driveRightClick(&view, QPoint(30, 30),
                                        [&](QMenu *menu) {
        collectOpenEntries(menu, &order, &enabled);
    });
    QVERIFY2(popped, "no context menu on link right-click");
    QCOMPARE(order, (QStringList{
        QStringLiteral("Open in New Tab"),
        QStringLiteral("Open in New Window"),
        QStringLiteral("Open in New Private Tab"),
        QStringLiteral("Open in New Private Window"),
        QStringLiteral("Open in New Tor Window")}));
    const bool torAvailable = !TorManager::resolveBinary().isEmpty();
    for (const QString &text : order) {
        const bool expected = text == QLatin1String("Open in New Tor Window")
            ? torAvailable : true;
        QVERIFY2(enabled.value(text) == expected,
                 qPrintable(QStringLiteral("%1 enabled=%2 expected=%3")
                     .arg(text).arg(enabled.value(text)).arg(expected)));
    }

    // 'Open in New Private Tab' produces a detached OTR view (there is
    // no TabWidget above the detached source) and loads the link.
    const bool poppedPrivateTab = driveRightClick(&view, QPoint(30, 30),
                                                  [&](QMenu *menu) {
        if (QAction *action = findMenuAction(
                menu, QStringLiteral("Open in New Private Tab")))
            action->trigger();
    });
    QVERIFY(poppedPrivateTab);
    WebView *privateTab = awaitDetachedView(&view, QLatin1String("file"));
    QVERIFY2(privateTab, "private-tab open produced no view");
    QCOMPARE(privateTab->webPage()->profile(),
             BrowserApplication::privateWebEngineProfile());
    QVERIFY(privateTab->webPage()->profile()->isOffTheRecord());
    QCOMPARE(privateTab->url(), linkTarget);
    closeDetached(privateTab);

    // 'Open in New Private Window' spawns a real BrowserMainWindow
    // whose first tab is off-the-record.
    BrowserApplication *application = BrowserApplication::instance();
    const QList<BrowserMainWindow *> beforeWindows =
        application->mainWindows();
    const bool poppedPrivateWindow = driveRightClick(
        &view, QPoint(30, 30), [&](QMenu *menu) {
        if (QAction *action = findMenuAction(
                menu, QStringLiteral("Open in New Private Window")))
            action->trigger();
    });
    QVERIFY(poppedPrivateWindow);
    QPointer<BrowserMainWindow> privateWindow;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QList<BrowserMainWindow *> windows =
            application->mainWindows();
        for (BrowserMainWindow *window : windows) {
            if (!beforeWindows.contains(window)) {
                privateWindow = window;
                return true;
            }
        }
        return false;
    }(), 5000);
    QVERIFY(privateWindow);
    WebView *windowTab = privateWindow->tabWidget()->currentWebView();
    QVERIFY(windowTab);
    QCOMPARE(windowTab->webPage()->profile(),
             BrowserApplication::privateWebEngineProfile());
    QVERIFY(windowTab->webPage()->profile()->isOffTheRecord());
    QTRY_VERIFY_WITH_TIMEOUT(windowTab->url() == linkTarget, 15000);
    privateWindow->close();
    QTRY_VERIFY_WITH_TIMEOUT(privateWindow.isNull(), 5000);

    // The plain 'Open in New Tab' still lands on this page's profile.
    const bool poppedTab = driveRightClick(&view, QPoint(30, 30),
                                           [&](QMenu *menu) {
        if (QAction *action = findMenuAction(
                menu, QStringLiteral("Open in New Tab")))
            action->trigger();
    });
    QVERIFY(poppedTab);
    WebView *normalTab = awaitDetachedView(&view, QLatin1String("file"));
    QVERIFY2(normalTab, "new-tab open produced no view");
    // Same context as the source page — that is also what keeps the
    // Referer header attached (cross-profile opens drop it).
    QCOMPARE(normalTab->webPage()->profile(), view.webPage()->profile());
    QVERIFY(!normalTab->webPage()->profile()->isOffTheRecord());
    QCOMPARE(normalTab->url(), linkTarget);
    closeDetached(normalTab);

    // A javascript: link disables every open entry — the menu is
    // honest instead of offering a click that opens nothing.
    QStringList badOrder;
    QHash<QString, bool> badEnabled;
    const bool poppedBad = driveRightClick(&view, QPoint(30, 100),
                                           [&](QMenu *menu) {
        collectOpenEntries(menu, &badOrder, &badEnabled);
    });
    QVERIFY2(poppedBad, "no context menu on javascript: link");
    QVERIFY2(!badOrder.isEmpty(),
             "javascript: link reported no link menu entries");
    for (const QString &text : badOrder)
        QVERIFY2(!badEnabled.value(text),
                 qPrintable(text + QLatin1String(" stayed enabled")));

    // Global private mode (a private-window process) collapses the
    // private entries — New Tab/Window are already private there.
    BrowserApplication::setPrivate(true);
    QStringList privateModeOrder;
    QHash<QString, bool> privateModeEnabled;
    driveRightClick(&view, QPoint(30, 30), [&](QMenu *menu) {
        collectOpenEntries(menu, &privateModeOrder, &privateModeEnabled);
    });
    BrowserApplication::setPrivate(false);
    QVERIFY(!privateModeOrder.contains(
        QLatin1String("Open in New Private Tab")));
    QVERIFY(!privateModeOrder.contains(
        QLatin1String("Open in New Private Window")));
    QVERIFY(privateModeOrder.contains(
        QLatin1String("Open in New Tor Window")));

    // Tor mode does the same — a private entry could only ever land
    // on the clearnet OTR profile from a tor window.
    BrowserApplication::setTorMode(true);
    QStringList torModeOrder;
    QHash<QString, bool> torModeEnabled;
    driveRightClick(&view, QPoint(30, 30), [&](QMenu *menu) {
        collectOpenEntries(menu, &torModeOrder, &torModeEnabled);
    });
    BrowserApplication::setTorMode(false);
    QVERIFY(!torModeOrder.contains(
        QLatin1String("Open in New Private Tab")));
    QVERIFY(!torModeOrder.contains(
        QLatin1String("Open in New Private Window")));
    QVERIFY(torModeOrder.contains(
        QLatin1String("Open in New Tor Window")));

    QFile::remove(fixturePath);
}

// CONT08: the adversarial source-context x open-target matrix —
// every combination must land the created page on EXACTLY the
// expected profile (named profile / shared clearnet OTR / tor), and
// the tor/OTR isolations must be structural (entries absent or
// remapped), never just a refused load.  'Open in New Tor Window' is
// asserted present but never triggered here — it would spawn a real
// `arora --tor` process; its argv shape and receiving-side re-gate
// are locked by tst_browserapp::torWindowHandoffGate.
void tst_WebView::contextMenuLinkCrossProfileMatrix()
{
    PrivacyFlagGuard flagGuard;
    WindowListGuard windowGuard;
    BrowserApplication *application = BrowserApplication::instance();
    QVERIFY(application);

    // A file: target loads with no network so opened views settle
    // deterministically.
    const QString fixturePath = QDir::temp().filePath(
        QLatin1String("arora-cont08-target.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>cont08-target</title>"
                      "</head><body>t</body></html>");
    }
    const QUrl linkTarget = QUrl::fromLocalFile(fixturePath);

    // The source page: a good link plus one of each refused scheme.
    const QString sourceHtml = QStringLiteral(
        "<html><body style='margin:0'>"
        "<a href='%1' style='position:fixed;left:0;top:0;display:block;"
        "width:300px;height:60px'>ok</a>"
        "<a href='data:text/html,refused' "
        "style='position:fixed;left:0;top:80px;display:block;"
        "width:300px;height:60px'>d</a>"
        "<a href='javascript:alert(1)' style='position:fixed;left:0;"
        "top:160px;display:block;width:300px;height:60px'>j</a>"
        "<a id='blob' style='position:fixed;left:0;top:240px;"
        "display:block;width:300px;height:60px'>b</a>"
        "<script>document.getElementById('blob').href="
        "URL.createObjectURL(new Blob(['<h1>x</h1>'],"
        "{type:'text/html'}));</script>"
        "</body></html>").arg(linkTarget.toString());
    const QUrl sourceUrl(QStringLiteral("data:text/html,")
        + QString::fromUtf8(QUrl::toPercentEncoding(sourceHtml)));

    const auto loadSource = [&sourceUrl](WebView *view) {
        QSignalSpy loaded(view, SIGNAL(loadFinished(bool)));
        view->loadUrl(sourceUrl);
        const auto deadline =
            QDateTime::currentMSecsSinceEpoch() + 15000;
        while (loaded.count() < 1
               && QDateTime::currentMSecsSinceEpoch() < deadline)
            QTest::qWait(50);
        return loaded.count() >= 1;
    };
    const auto triggerOpen = [](WebView *view, const QString &text) {
        bool found = false;
        const bool popped = driveRightClick(view, QPoint(30, 30),
                                            [&](QMenu *menu) {
            if (QAction *action = findMenuAction(menu, text)) {
                found = true;
                action->trigger();
            }
        });
        return popped && found;
    };
    const auto awaitUrl = [&linkTarget](WebView *view) {
        const auto deadline =
            QDateTime::currentMSecsSinceEpoch() + 15000;
        while (view->url() != linkTarget
               && QDateTime::currentMSecsSinceEpoch() < deadline)
            QTest::qWait(50);
        return view->url() == linkTarget;
    };

    // ==== source context 1: a normal window ====
    BrowserMainWindow *window = application->newMainWindow();
    windowGuard.windows << window;
    window->resize(900, 700);
    TabWidget *tabs = window->tabWidget();
    WebView *source = tabs->currentWebView();
    QVERIFY(source);
    QVERIFY2(loadSource(source), "source fixture never loaded");
    QVERIFY(!source->webPage()->profile()->isOffTheRecord());
    QCOMPARE(source->webPage()->profile(),
             BrowserApplication::webEngineProfile());

    // A refused-scheme link must disable every open entry — the
    // menu is honest instead of offering a click that goes nowhere.
    // The blob: link's entries may not surface at all when Chromium
    // declines to report the url — either way nothing is offered.
    const bool blobMinted = awaitBlobHref(source);
    const struct { QPoint pos; bool expectEntries; } refusedCells[] = {
        { QPoint(30, 110), true },   // data: link
        { QPoint(30, 190), true },   // javascript: link
        { QPoint(30, 270), false },  // blob: link
    };
    for (int i = 0; i < 3; ++i) {
        if (i == 2 && !blobMinted)
            break;
        QStringList order;
        QHash<QString, bool> enabled;
        const bool popped = driveRightClick(source, refusedCells[i].pos,
                                            [&](QMenu *menu) {
            collectOpenEntries(menu, &order, &enabled);
        });
        QVERIFY2(popped, "no context menu on refused-scheme link");
        if (refusedCells[i].expectEntries)
            QVERIFY(!order.isEmpty());
        for (const QString &text : order) {
            QVERIFY2(!enabled.value(text),
                     qPrintable(text + QLatin1String(
                                    " stayed enabled on refused link")));
        }
    }

    // New Tab -> a background tab on THIS page's named profile.
    {
        const QSet<WebView *> before = tabViews(tabs);
        QVERIFY2(triggerOpen(source, QStringLiteral("Open in New Tab")),
                 "New Tab entry missing or menu never popped");
        WebView *created = awaitNewTabView(tabs, before);
        QVERIFY2(created, "New Tab produced no tab");
        QCOMPARE(created->webPage()->profile(),
                 source->webPage()->profile());
        QVERIFY(!created->webPage()->profile()->isOffTheRecord());
        QVERIFY2(awaitUrl(created), "New Tab never loaded the link");
    }
    // New Window -> a window on the named profile.
    {
        const QList<BrowserMainWindow *> before =
            application->mainWindows();
        QVERIFY(triggerOpen(source,
                            QStringLiteral("Open in New Window")));
        BrowserMainWindow *created = awaitNewWindow(application, before);
        QVERIFY2(created, "New Window produced no window");
        windowGuard.windows << created;
        WebView *view = created->tabWidget()->currentWebView();
        QVERIFY(view);
        QCOMPARE(view->webPage()->profile(),
                 source->webPage()->profile());
        QVERIFY(!view->webPage()->profile()->isOffTheRecord());
        QVERIFY2(awaitUrl(view), "New Window never loaded the link");
    }
    // New Private Tab -> the shared clearnet OTR profile.
    WebView *openedPrivate = nullptr;
    {
        const QSet<WebView *> before = tabViews(tabs);
        QVERIFY(triggerOpen(source,
                    QStringLiteral("Open in New Private Tab")));
        openedPrivate = awaitNewTabView(tabs, before);
        QVERIFY2(openedPrivate, "New Private Tab produced no tab");
        QCOMPARE(openedPrivate->webPage()->profile(),
                 BrowserApplication::privateWebEngineProfile());
        QVERIFY(openedPrivate->webPage()->profile()->isOffTheRecord());
        QVERIFY2(awaitUrl(openedPrivate),
                 "New Private Tab never loaded the link");
    }
    // New Private Window -> an OTR first tab in a new window.
    {
        const QList<BrowserMainWindow *> before =
            application->mainWindows();
        QVERIFY(triggerOpen(source,
                    QStringLiteral("Open in New Private Window")));
        BrowserMainWindow *created = awaitNewWindow(application, before);
        QVERIFY2(created, "New Private Window produced no window");
        windowGuard.windows << created;
        WebView *view = created->tabWidget()->currentWebView();
        QVERIFY(view);
        QCOMPARE(view->webPage()->profile(),
                 BrowserApplication::privateWebEngineProfile());
        QVERIFY2(awaitUrl(view),
                 "New Private Window never loaded the link");
    }

    // The OTR tabs the menu created are invisible to session state —
    // only the named-profile tabs survive a save/restore round trip.
    {
        int namedCount = 0;
        for (int i = 0; i < tabs->count(); ++i)
            if (!tabs->isTabPrivate(i))
                ++namedCount;
        QVERIFY(namedCount >= 2); // source + the New Tab above
        TabWidget restored;
        QVERIFY(restored.restoreState(tabs->saveState()));
        QCOMPARE(restored.count(), namedCount);
        for (int i = 0; i < restored.count(); ++i)
            QVERIFY(!restored.isTabPrivate(i));
    }
    // Closing a menu-opened private tab must not queue it for
    // reopen — the undo stack is part of the residue.
    {
        const bool wasEnabled =
            tabs->recentlyClosedTabsAction()->isEnabled();
        const int index = [&]() {
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->webView(i) == openedPrivate)
                    return i;
            return -1;
        }();
        QVERIFY(index != -1);
        tabs->closeTab(index);
        QCOMPARE(tabs->recentlyClosedTabsAction()->isEnabled(),
                 wasEnabled);
    }

    // ==== source context 2: a private tab inside the normal window ====
    WebView *privateSource = tabs->makeNewPrivateTab(true);
    QVERIFY(privateSource);
    QVERIFY(privateSource->webPage()->profile()->isOffTheRecord());
    QVERIFY2(loadSource(privateSource), "private source never loaded");

    // Plain 'New Tab' from an OTR page can only ever be another OTR
    // page — a named-profile child would record the visit.
    {
        const QSet<WebView *> before = tabViews(tabs);
        QVERIFY(triggerOpen(privateSource,
                            QStringLiteral("Open in New Tab")));
        WebView *created = awaitNewTabView(tabs, before);
        QVERIFY2(created, "private-tab New Tab produced no tab");
        QVERIFY(created->webPage()->profile()->isOffTheRecord());
        QCOMPARE(created->webPage()->profile(),
                 BrowserApplication::privateWebEngineProfile());
    }
    // 'New Window' from an OTR page gets the OTR first-tab swap.
    {
        const QList<BrowserMainWindow *> before =
            application->mainWindows();
        QVERIFY(triggerOpen(privateSource,
                            QStringLiteral("Open in New Window")));
        BrowserMainWindow *created = awaitNewWindow(application, before);
        QVERIFY2(created, "private-tab New Window produced no window");
        windowGuard.windows << created;
        WebView *view = created->tabWidget()->currentWebView();
        QVERIFY(view);
        QVERIFY(view->webPage()->profile()->isOffTheRecord());
        QCOMPARE(view->webPage()->profile(),
                 BrowserApplication::privateWebEngineProfile());
    }
    // 'New Private Tab' from an OTR page is a real second context —
    // not a no-op — and lands OTR like any other private open.
    {
        const QSet<WebView *> before = tabViews(tabs);
        QVERIFY(triggerOpen(privateSource,
                    QStringLiteral("Open in New Private Tab")));
        WebView *created = awaitNewTabView(tabs, before);
        QVERIFY2(created, "private-tab Private Tab produced no tab");
        QVERIFY(created->webPage()->profile()->isOffTheRecord());
    }
    {
        const QList<BrowserMainWindow *> before =
            application->mainWindows();
        QVERIFY(triggerOpen(privateSource,
                    QStringLiteral("Open in New Private Window")));
        BrowserMainWindow *created = awaitNewWindow(application, before);
        QVERIFY2(created,
                 "private-tab Private Window produced no window");
        windowGuard.windows << created;
        QVERIFY(created->tabWidget()->currentWebView()
                    ->webPage()->profile()->isOffTheRecord());
    }

    // ==== slot-level defense in depth ====
    // A refused url pushed through the real slot (a QAction sender is
    // all the slot asks for) must still open nothing — the gate below
    // the menu is the second line of defense.  The tor slot is
    // omitted: an allowed url would spawn a real process, and its
    // refusal order is identical by inspection.
    {
        TestWebView probeSource(BrowserApplication::webEngineProfile());
        QAction goodPrivateTab;
        goodPrivateTab.setData(linkTarget);
        QVERIFY(QObject::connect(&goodPrivateTab, SIGNAL(triggered()),
                                 &probeSource,
                                 SLOT(openUrlInNewPrivateTab())));
        // Positive control first: the probe genuinely reaches the
        // slot's open path.
        goodPrivateTab.trigger();
        WebView *control =
            awaitDetachedView(&probeSource, QLatin1String("file"));
        QVERIFY2(control, "slot probe never reached the open path");
        QVERIFY(control->webPage()->profile()->isOffTheRecord());
        closeDetached(control);

        QAction badNewTab, badNewWindow, badPrivateTab, badPrivateWindow;
        QVERIFY(QObject::connect(&badNewTab, SIGNAL(triggered()),
                                 &probeSource,
                                 SLOT(openLinkInNewTab())));
        QVERIFY(QObject::connect(&badNewWindow, SIGNAL(triggered()),
                                 &probeSource,
                                 SLOT(openLinkInNewWindow())));
        QVERIFY(QObject::connect(&badPrivateTab, SIGNAL(triggered()),
                                 &probeSource,
                                 SLOT(openUrlInNewPrivateTab())));
        QVERIFY(QObject::connect(&badPrivateWindow, SIGNAL(triggered()),
                                 &probeSource,
                                 SLOT(openUrlInNewPrivateWindow())));
        const int windowsBefore = application->mainWindows().count();
        const char *badUrls[] = {
            "javascript:alert(1)",
            "data:text/html,<h1>x</h1>",
            "blob:https://example.com/uuid",
        };
        for (const char *bad : badUrls) {
            const QUrl url = QUrl(QLatin1String(bad));
            badNewTab.setData(url);
            badNewWindow.setData(url);
            badPrivateTab.setData(url);
            badPrivateWindow.setData(url);
            badNewTab.trigger();
            badNewWindow.trigger();
            badPrivateTab.trigger();
            badPrivateWindow.trigger();
        }
        QTest::qWait(100);
        QCOMPARE(application->mainWindows().count(), windowsBefore);
        QVERIFY2(!findDetachedView(&probeSource),
                 "a refused url still spawned a view");
    }

    // ==== source context 3: a fully private window ====
    BrowserApplication::setPrivate(true);
    BrowserMainWindow *privateWindow = application->newMainWindow();
    windowGuard.windows << privateWindow;
    privateWindow->resize(900, 700);
    WebView *pwSource = privateWindow->tabWidget()->currentWebView();
    QVERIFY(pwSource);
    QVERIFY(pwSource->webPage()->profile()->isOffTheRecord());
    QVERIFY2(loadSource(pwSource), "private-window source never loaded");
    {
        // The private entries do not exist here — that is the
        // structural guarantee, not a refused load.
        QStringList order;
        QHash<QString, bool> enabled;
        QVERIFY(driveRightClick(pwSource, QPoint(30, 30),
                                [&](QMenu *menu) {
            collectOpenEntries(menu, &order, &enabled);
        }));
        QVERIFY(!order.contains(
            QLatin1String("Open in New Private Tab")));
        QVERIFY(!order.contains(
            QLatin1String("Open in New Private Window")));
        QVERIFY(order.contains(QLatin1String("Open in New Tab")));
        QVERIFY(order.contains(
            QLatin1String("Open in New Tor Window")));
    }
    {
        TabWidget *pwTabs = privateWindow->tabWidget();
        const QSet<WebView *> before = tabViews(pwTabs);
        QVERIFY(triggerOpen(pwSource,
                            QStringLiteral("Open in New Tab")));
        WebView *created = awaitNewTabView(pwTabs, before);
        QVERIFY2(created, "private-window New Tab produced no tab");
        QVERIFY(created->webPage()->profile()->isOffTheRecord());
    }
    {
        const QList<BrowserMainWindow *> before =
            application->mainWindows();
        QVERIFY(triggerOpen(pwSource,
                            QStringLiteral("Open in New Window")));
        BrowserMainWindow *created = awaitNewWindow(application, before);
        QVERIFY2(created, "private-window New Window produced none");
        windowGuard.windows << created;
        QVERIFY(created->tabWidget()->currentWebView()
                    ->webPage()->profile()->isOffTheRecord());
    }
    BrowserApplication::setPrivate(false);

    // ==== source context 4: tor mode ====
    // No real tor daemon exists in the test — the profile identity
    // assertions are the boundary that matters: a tor tab's private
    // semantics resolve to the tor profile and can never reach the
    // clearnet OTR profile.
    BrowserApplication::setTorMode(true);
    {
        TabWidget torTabs;
        torTabs.newTab();
        WebView *torTab = torTabs.webView(0);
        QVERIFY(torTab);
        QCOMPARE(torTab->webPage()->profile(),
                 BrowserProfile::torProfile());
        // 'Private tab' under tor resolves to a tor-profile tab —
        // never the shared clearnet OTR profile.
        WebView *torPrivate = torTabs.makeNewPrivateTab(true);
        QVERIFY(torPrivate);
        QCOMPARE(torPrivate->webPage()->profile(),
                 BrowserProfile::torProfile());
        QVERIFY(torPrivate->webPage()->profile()
                != BrowserApplication::privateWebEngineProfile());
        // A child of a tor tab stays tor-bound.
        torTabs.setCurrentIndex(0);
        const int countBefore = torTabs.count();
        torTabs.newTab();
        QCOMPARE(torTabs.count(), countBefore + 1);
        QCOMPARE(torTabs.webView(torTabs.count() - 1)
                     ->webPage()->profile(),
                 BrowserProfile::torProfile());
    }
    {
        // The detached-view fallback of the private-tab open picks the
        // tor profile under tor mode, not the clearnet OTR one.
        TestWebView torSource(BrowserProfile::torProfile());
        QAction probe;
        probe.setData(linkTarget);
        QVERIFY(QObject::connect(&probe, SIGNAL(triggered()),
                                 &torSource,
                                 SLOT(openUrlInNewPrivateTab())));
        probe.trigger();
        WebView *detached =
            awaitDetachedView(&torSource, QLatin1String("file"));
        QVERIFY2(detached, "tor private-tab open produced no view");
        QCOMPARE(detached->webPage()->profile(),
                 BrowserProfile::torProfile());
        QVERIFY(detached->webPage()->profile()
                != BrowserApplication::privateWebEngineProfile());
        closeDetached(detached);
    }
    BrowserApplication::setTorMode(false);

    QFile::remove(fixturePath);
}

// CONT08: the Referer discipline — a link opened into the SAME
// profile keeps the hotlink-compat Referer header; a private or tor
// hand-off must send none, because the header would carry the source
// page's url across a privacy boundary.  Asserted on the wire, not on
// the request object: the loopback server records exactly what
// arrived.  Each open gets its own target path so a request maps
// back to exactly one menu entry — which also makes the history
// residue checks unambiguous.
void tst_WebView::linkOpenRefererDiscipline()
{
    PrivacyFlagGuard flagGuard;
    WindowListGuard windowGuard;
    RecordedHttpServer server;
    QVERIFY(server.start());
    server.indexHtml = QByteArray(
        "<html><body style='margin:0'>"
        "<a href='/t-tab.html' style='position:fixed;left:0;top:0;"
        "display:block;width:300px;height:60px'>t</a>"
        "<a href='/t-win.html' style='position:fixed;left:0;top:80px;"
        "display:block;width:300px;height:60px'>w</a>"
        "<a href='/t-ptab.html' style='position:fixed;left:0;top:160px;"
        "display:block;width:300px;height:60px'>p</a>"
        "<a href='/t-pwin.html' style='position:fixed;left:0;top:240px;"
        "display:block;width:300px;height:60px'>q</a>"
        "</body></html>");

    const QUrl sourceUrl = server.url(QLatin1String("/source.html"));
    TestWebView view(BrowserApplication::webEngineProfile());
    view.resize(900, 700);
    view.show();
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(sourceUrl);
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    QTRY_VERIFY(server.requestsFor(QLatin1String("/source.html"))
                    .count() >= 1);

    const auto triggerEntry = [&view](const QPoint &pos,
                                      const QString &text) {
        bool found = false;
        const bool popped = driveRightClick(&view, pos,
                                            [&](QMenu *menu) {
            if (QAction *action = findMenuAction(menu, text)) {
                found = true;
                action->trigger();
            }
        });
        return popped && found;
    };
    const auto awaitRequest = [&server](const QString &target,
                                        int expected) {
        const auto deadline =
            QDateTime::currentMSecsSinceEpoch() + 15000;
        while (server.requestsFor(target).count() < expected
               && QDateTime::currentMSecsSinceEpoch() < deadline)
            QTest::qWait(50);
        return server.requestsFor(target).count() == expected;
    };

    // AUDIT FINDING (CONT08): Chromium clamps our custom 'Referer'
    // request header through its referrer policy — the wire value is
    // the source page's ORIGIN, not the full url loadUrlInView sets.
    // That is still hotlink-adequate and strictly more private; the
    // lock here is presence-vs-absence plus the clamped shape.
    const QString sourceOrigin = server.url(QLatin1String("/"))
                                     .toString();

    // Same-profile opens keep a Referer — both the new-tab and the
    // new-window slot take loadUrlInView's same-profile branch.
    // Results are captured before cleanup so an assertion can't leak
    // the spawned view.
    for (int i = 0; i < 2; ++i) {
        const QString entry = i == 0
            ? QStringLiteral("Open in New Tab")
            : QStringLiteral("Open in New Window");
        const QString target = i == 0
            ? QStringLiteral("/t-tab.html")
            : QStringLiteral("/t-win.html");
        const QPoint pos = i == 0 ? QPoint(30, 30) : QPoint(30, 110);
        QVERIFY2(triggerEntry(pos, entry),
                 qPrintable(entry + QLatin1String(" missing")));
        WebView *detached =
            awaitDetachedView(&view, QLatin1String("http"));
        QVERIFY2(detached,
                 qPrintable(entry + QLatin1String(" produced no view")));
        const bool sameProfile =
            detached->webPage()->profile() == view.webPage()->profile();
        const bool got = awaitRequest(target, 1);
        const QString seenReferer = got
            ? server.requestsFor(target).first().referer
            : QStringLiteral("<none>");
        closeDetached(detached);
        QVERIFY(sameProfile);
        QVERIFY2(got,
                 qPrintable(target + QLatin1String(" never requested")));
        QCOMPARE(seenReferer, sourceOrigin);
        QTRY_VERIFY(HistoryManager::instance()->historyContains(
            server.url(target).toString()));
    }

    // The private hand-offs send NO Referer — a leak here would carry
    // the source page's url into the OTR context.
    {
        QVERIFY(triggerEntry(QPoint(30, 190),
                    QStringLiteral("Open in New Private Tab")));
        WebView *detached =
            awaitDetachedView(&view, QLatin1String("http"));
        QVERIFY2(detached, "private-tab open produced no view");
        const bool otr =
            detached->webPage()->profile()->isOffTheRecord()
            && detached->webPage()->profile()
                == BrowserApplication::privateWebEngineProfile();
        const bool got = awaitRequest(QLatin1String("/t-ptab.html"), 1);
        const QString seenReferer = got
            ? server.requestsFor(QLatin1String("/t-ptab.html"))
                  .first().referer
            : QStringLiteral("<none>");
        closeDetached(detached);
        QVERIFY(otr);
        QVERIFY2(got, "private target never requested");
        QCOMPARE(seenReferer, QString());
        QTest::qWait(200);
        QVERIFY(!HistoryManager::instance()->historyContains(
            server.url(QLatin1String("/t-ptab.html")).toString()));
    }
    {
        BrowserApplication *application =
            BrowserApplication::instance();
        const QList<BrowserMainWindow *> before =
            application->mainWindows();
        QVERIFY(triggerEntry(QPoint(30, 270),
                    QStringLiteral("Open in New Private Window")));
        QPointer<BrowserMainWindow> created =
            awaitNewWindow(application, before);
        QVERIFY2(created, "private-window open produced no window");
        windowGuard.windows << created;
        WebView *tab = created->tabWidget()->currentWebView();
        QVERIFY(tab);
        QVERIFY(tab->webPage()->profile()->isOffTheRecord());
        QVERIFY2(awaitRequest(QLatin1String("/t-pwin.html"), 1),
                 "private-window target never requested");
        QVERIFY(server.requestsFor(QLatin1String("/t-pwin.html"))
                    .first().referer.isEmpty());
        QTest::qWait(200);
        QVERIFY(!HistoryManager::instance()->historyContains(
            server.url(QLatin1String("/t-pwin.html")).toString()));
    }
}

// POL01: right-clicking plain page content gets the stock menu plus
// the page-level 'Copy Clean Link' (cleans the page address) and
// 'Show QR Code for This Page' — the latter pops the QR dialog.
void tst_WebView::contextMenuPageActions()
{
    TestWebView view;
    view.resize(800, 600);
    view.show();

    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(QUrl(QStringLiteral(
        "data:text/html,<html><body><p>plain page</p></body></html>")));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    bool foundClean = false;
    bool foundQr = false;
    const bool popped = driveRightClick(&view, QPoint(400, 300),
                                        [&](QMenu *menu) {
        foundClean = findMenuAction(
            menu, QStringLiteral("Copy Clean Link")) != nullptr;
        if (QAction *qr = findMenuAction(
                menu, QStringLiteral("Show QR Code for This Page"))) {
            foundQr = true;
            qr->trigger();
        }
    });
    QVERIFY2(popped, "no context menu on page right-click");
    QVERIFY(foundClean);
    QVERIFY(foundQr);

    // The QR action opens a non-modal QrCodeDialog for the page URL.
    QPointer<QDialog> dialog;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget *top : tops) {
            if (top->inherits("QrCodeDialog")) {
                dialog = qobject_cast<QDialog *>(top);
                return true;
            }
        }
        return false;
    }(), 5000);
    QVERIFY(dialog);
    dialog->close();
    QTRY_VERIFY_WITH_TIMEOUT(dialog.isNull(), 5000);
}

// CTX01 helpers — 'Open Image in New Tab/Window' lands in a detached
// top-level WebView when the source has no TabWidget (openUrlInTarget's
// fallback), so the tests locate it among the application's top-levels.
static WebView *findDetachedView(WebView *source)
{
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget *top : tops) {
        WebView *candidate = qobject_cast<WebView *>(top);
        if (candidate && candidate != source)
            return candidate;
    }
    return nullptr;
}

// Waits for a detached view whose committed url has the given scheme,
// then returns it (still alive) or nullptr on timeout.
static WebView *awaitDetachedView(WebView *source,
                                  const QString &scheme)
{
    WebView *detached = nullptr;
    const auto deadline =
        QDateTime::currentMSecsSinceEpoch() + 15000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        detached = findDetachedView(source);
        if (detached
            && detached->url().scheme() == scheme)
            return detached;
        QTest::qWait(50);
    }
    return nullptr;
}

static void closeDetached(WebView *view)
{
    QPointer<WebView> guard(view);
    if (view)
        view->close();
    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 5000);
}

// CTX01: a plain <img> offers the full image block; 'Open Image in
// New Tab' loads the mediaUrl in a detached view.  The img source is
// itself a data: url — covering the data: open path at the same time.
void tst_WebView::contextMenuImageActions()
{
    const QString imageSrc = QStringLiteral(
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAA"
        "AfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");

    TestWebView view;
    view.resize(800, 600);
    view.show();
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(QUrl(QStringLiteral(
        "data:text/html,<html><body><img src='%1' "
        "style='position:fixed;left:0;top:0;width:200px;height:200px'>"
        "</body></html>").arg(imageSrc)));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    bool opened = false;
    bool sawSave = false;
    bool sawCopy = false;
    bool sawCopyLocation = false;
    bool sawBlock = false;
    const bool popped = driveRightClick(&view, QPoint(40, 40),
                                        [&](QMenu *menu) {
        sawSave = findMenuAction(
            menu, QStringLiteral("Save Image")) != nullptr;
        sawCopy = findMenuAction(
            menu, QStringLiteral("Copy Image")) != nullptr;
        sawCopyLocation = findMenuAction(
            menu, QStringLiteral("Copy Image Location")) != nullptr;
        sawBlock = findMenuAction(
            menu, QStringLiteral("Block Image")) != nullptr;
        if (QAction *open = findMenuAction(
                menu, QStringLiteral("Open Image in New Tab"))) {
            opened = true;
            open->trigger();
        }
    });
    QVERIFY2(popped, "no context menu on image right-click");
    QVERIFY(opened);
    QVERIFY(sawSave);
    QVERIFY(sawCopy);
    QVERIFY(sawCopyLocation);
    QVERIFY(sawBlock);

    WebView *detached = awaitDetachedView(&view, QLatin1String("data"));
    QVERIFY2(detached, "open-in-new-tab produced no data: view");
    QCOMPARE(detached->url(), QUrl(imageSrc));
    closeDetached(detached);
}

// CTX01: an image that is also a link keeps BOTH menu blocks — the
// link actions and the image actions.
void tst_WebView::contextMenuImageLinkActions()
{
    const QString imageSrc = QStringLiteral(
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAA"
        "AfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");

    TestWebView view;
    view.resize(800, 600);
    view.show();
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(QUrl(QStringLiteral(
        "data:text/html,<html><body><a href='https://example.com/x'>"
        "<img src='%1' style='position:fixed;left:0;top:0;width:200px;"
        "height:200px'></a></body></html>").arg(imageSrc)));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    bool sawLinkOpen = false;
    bool sawCopyLink = false;
    bool sawImageOpen = false;
    bool sawCopyImageLocation = false;
    const bool popped = driveRightClick(&view, QPoint(40, 40),
                                        [&](QMenu *menu) {
        sawLinkOpen = findMenuAction(
            menu, QStringLiteral("Open in New Tab")) != nullptr;
        sawCopyLink = findMenuAction(
            menu, QStringLiteral("Copy Link Location")) != nullptr;
        sawImageOpen = findMenuAction(
            menu, QStringLiteral("Open Image in New Tab")) != nullptr;
        sawCopyImageLocation = findMenuAction(
            menu, QStringLiteral("Copy Image Location")) != nullptr;
    });
    QVERIFY2(popped, "no context menu on linked image right-click");
    QVERIFY(sawLinkOpen);
    QVERIFY(sawCopyLink);
    QVERIFY(sawImageOpen);
    QVERIFY(sawCopyImageLocation);
}

// CTX01: <canvas> has no mediaUrl — the menu must still offer image
// actions resolved through the in-page serializer (contextimage.js).
// Open lands a data: png in a detached view, Copy puts the pixels on
// the clipboard, Save issues a data: download on the profile.
void tst_WebView::contextMenuCanvasActions()
{
    TestWebView view;
    view.resize(800, 600);
    view.show();
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(QUrl(QStringLiteral(
        "data:text/html,<html><body style='margin:0'>"
        "<canvas id='c' width='160' height='120' "
        "style='position:fixed;left:0;top:0'></canvas><script>"
        "var x=document.getElementById('c').getContext('2d');"
        "x.fillStyle='#e02040';x.fillRect(0,0,160,120);"
        "</script></body></html>")));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    // Let the paint script run before the menu is built.
    QTest::qWait(300);

    bool sawOpenTab = false;
    bool sawOpenWindow = false;
    bool sawSave = false;
    bool sawCopy = false;
    const bool popped = driveRightClick(&view, QPoint(40, 40),
                                        [&](QMenu *menu) {
        sawOpenTab = findMenuAction(
            menu, QStringLiteral("Open Image in New Tab")) != nullptr;
        sawOpenWindow = findMenuAction(
            menu, QStringLiteral("Open Image in New Window"))
            != nullptr;
        sawSave = findMenuAction(
            menu, QStringLiteral("Save Image")) != nullptr;
        if (QAction *copy = findMenuAction(
                menu, QStringLiteral("Copy Image"))) {
            sawCopy = true;
            copy->trigger();
        }
    });
    QVERIFY2(popped, "no context menu on canvas right-click");
    QVERIFY(sawOpenTab);
    QVERIFY(sawOpenWindow);
    QVERIFY(sawSave);
    QVERIFY(sawCopy);

    // Copy decodes the serialized png onto the clipboard.
    QTRY_VERIFY_WITH_TIMEOUT(
        !QApplication::clipboard()->image().isNull(), 10000);
    const QImage copied = QApplication::clipboard()->image();
    QCOMPARE(copied.width(), 160);
    QCOMPARE(copied.height(), 120);

    // 'Open Image in New Tab' resolves the canvas to a png data url.
    const bool poppedOpen = driveRightClick(&view, QPoint(40, 40),
                                            [&](QMenu *menu) {
        if (QAction *open = findMenuAction(
                menu, QStringLiteral("Open Image in New Tab")))
            open->trigger();
    });
    QVERIFY(poppedOpen);
    WebView *detached = awaitDetachedView(&view, QLatin1String("data"));
    QVERIFY2(detached, "canvas open produced no data: view");
    QVERIFY(detached->url().toString().startsWith(
        QLatin1String("data:image/png")));
    closeDetached(detached);

    // 'Save Image' issues a real download request for the data url on
    // the page's profile.
    QList<QUrl> requested;
    QObject::connect(view.page()->profile(),
            &QWebEngineProfile::downloadRequested, &view,
            [&requested](QWebEngineDownloadRequest *download) {
        requested.append(download->url());
        download->cancel();
    });
    const bool poppedSave = driveRightClick(&view, QPoint(40, 40),
                                            [&](QMenu *menu) {
        if (QAction *save = findMenuAction(
                menu, QStringLiteral("Save Image")))
            save->trigger();
    });
    QVERIFY(poppedSave);
    QTRY_VERIFY_WITH_TIMEOUT(!requested.isEmpty(), 15000);
    QVERIFY(requested.first().toString().startsWith(
        QLatin1String("data:image/png")));
}

// CTX01: a <video>'s poster frame is offered for opening — the
// request only carries the stream url, so the poster attribute is
// resolved in-page.
void tst_WebView::contextMenuVideoPosterActions()
{
    const QString posterSrc = QStringLiteral(
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAA"
        "AfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==");

    TestWebView view;
    view.resize(800, 600);
    view.show();
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(QUrl(QStringLiteral(
        "data:text/html,<html><body style='margin:0'>"
        "<video poster='%1' src='data:video/mp4;base64,AAAA' "
        "style='position:fixed;left:0;top:0;width:240px;height:160px'>"
        "</video></body></html>").arg(posterSrc)));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    QTest::qWait(300);

    // NOTE: Chromium only reports MediaTypeVideo when the element has
    // a loadable absolute src — a src-less <video> is MediaTypeNone.
    bool sawPoster = false;
    const bool popped = driveRightClick(&view, QPoint(40, 40),
                                        [&](QMenu *menu) {
        if (QAction *poster = findMenuAction(
                menu, QStringLiteral("Open Poster in New Tab"))) {
            sawPoster = true;
            poster->trigger();
        }
    });
    QVERIFY2(popped, "no context menu on video right-click");
    QVERIFY(sawPoster);

    WebView *detached = awaitDetachedView(&view, QLatin1String("data"));
    QVERIFY2(detached, "open-poster produced no data: view");
    QCOMPARE(detached->url(), QUrl(posterSrc));
    closeDetached(detached);
}

// CTX01: blob: media urls resolve inside the creating profile — a
// page-generated blob image must still open in a new tab on the same
// profile while the source document lives.
void tst_WebView::contextMenuBlobImage()
{
    TestWebView view;
    view.resize(800, 600);
    view.show();
    QSignalSpy loaded(&view, SIGNAL(loadFinished(bool)));
    view.loadUrl(QUrl(QStringLiteral(
        "data:text/html,<html><body style='margin:0'>"
        "<img id='i' style='position:fixed;left:0;top:0;width:120px;"
        "height:120px'><script>"
        "var c=document.createElement('canvas');c.width=c.height=8;"
        "c.getContext('2d').fillRect(0,0,8,8);"
        "c.toBlob(function(b){"
        "document.getElementById('i').src=URL.createObjectURL(b);});"
        "</script></body></html>")));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    // Wait until the blob url is actually assigned to the img.
    std::shared_ptr<bool> probed(new bool(false));
    std::shared_ptr<QString> src(new QString);
    const auto deadline =
        QDateTime::currentMSecsSinceEpoch() + 15000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline
           && !src->startsWith(QLatin1String("blob:"))) {
        view.page()->runJavaScript(
            QLatin1String("document.getElementById('i').src"),
            [probed, src](const QVariant &result) {
                *src = result.toString();
                *probed = true;
            });
        QTRY_VERIFY_WITH_TIMEOUT(*probed, 5000);
        *probed = false;
        if (!src->startsWith(QLatin1String("blob:")))
            QTest::qWait(100);
    }
    QVERIFY2(src->startsWith(QLatin1String("blob:")),
             qPrintable(QStringLiteral("img src never became blob: %1")
                        .arg(*src)));

    bool opened = false;
    const bool popped = driveRightClick(&view, QPoint(40, 40),
                                        [&](QMenu *menu) {
        if (QAction *open = findMenuAction(
                menu, QStringLiteral("Open Image in New Tab"))) {
            opened = true;
            open->trigger();
        }
    });
    QVERIFY2(popped, "no context menu on blob image right-click");
    QVERIFY(opened);

    WebView *detached = awaitDetachedView(&view, QLatin1String("blob"));
    QVERIFY2(detached, "blob: image did not open in a detached view");
    QVERIFY(detached->url().toString().startsWith(
        QLatin1String("blob:")));
    closeDetached(detached);
}

int main(int argc, char *argv[])
{
    // POL03: Chromium latches QTWEBENGINE_CHROMIUM_FLAGS as the engine
    // context spins up — that happens during application construction,
    // so the Blink MiddleClickAutoscroll switch (what
    // BrowserProfile::applyChromiumFlags() emits for the shipping
    // default) must be in the environment before the app exists.
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!flags.contains("MiddleClickAutoscroll")) {
        if (!flags.isEmpty())
            flags += ' ';
        flags += "--enable-blink-features=MiddleClickAutoscroll";
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
    }
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    BrowserApplication app(argc, argv);
    tst_WebView tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_webview.moc"
