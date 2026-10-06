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
#include <qwebenginefindtextresult.h>
#include <qwebenginepage.h>
#include <qwebengineview.h>
#include <qmimedata.h>
#include <qlineedit.h>
#include <qlabel.h>
#include <qtoolbutton.h>

#include <memory>

#include "webview.h"
#include "webpage.h"
#include "webviewsearch.h"
#include "browserapplication.h"
#include "qtest_arora.h"
#include "qtry.h"

// Exposes the protected event handlers for direct dispatch.
class TestWebView : public WebView
{
public:
    TestWebView(QWidget *parent = 0)
        : WebView(parent)
    {
    }

    TestWebView(QWebEngineProfile *profile, QWidget *parent = 0)
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
        QDragEnterEvent event(QPoint(1, 1), Qt::CopyAction, mimeData,
                              Qt::LeftButton, Qt::NoModifier);
        dragEnterEvent(&event);
        return event.isAccepted();
    }

    void sendDragMove(QMimeData *mimeData, bool sourceIsSelf)
    {
        QDragMoveEvent event(QPoint(1, 1), Qt::CopyAction, mimeData,
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
    void findText();
    void webViewWithSearch();
};

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
    for (int waited = 0; !*probed && waited < 15000; waited += 50)
        QTest::qWait(50);
    QVERIFY(*probed);
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

// In-page find through WebViewSearch — QWebEnginePage::findText is
// asynchronous; the "Not Found" label reflects the match count.
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

    // Sanity probe straight through QWebEngineView: the async result
    // reports the document match count.
    std::shared_ptr<bool> probed(new bool(false));
    std::shared_ptr<int> matches(new int(-1));
    view.findText(QLatin1String("arora"), QWebEnginePage::FindFlags(),
                  [probed, matches](const QWebEngineFindTextResult &r) {
        *matches = r.numberOfMatches();
        *probed = true;
    });
    for (int waited = 0; !*probed && waited < 15000; waited += 50)
        QTest::qWait(50);
    QVERIFY(*probed);
    QVERIFY(*matches > 0);

    edit->setText(QLatin1String("arora"));
    search.findNext();
    QTRY_VERIFY_WITH_TIMEOUT(info->text().isEmpty(), 15000);

    search.findPrevious();
    QTest::qWait(250);
    QVERIFY(info->text().isEmpty());

    edit->setText(QLatin1String("no-such-text-xyz"));
    search.findNext();
    QTRY_VERIFY_WITH_TIMEOUT(
        info->text() == WebViewSearch::tr("Not Found"), 15000);

    // highlightAll toggles between re-running the find and clearing
    // highlights.
    QToolButton *highlight = search.findChild<QToolButton*>(
        QLatin1String("highlightAllButton"));
    QVERIFY(highlight);
    edit->setText(QLatin1String("arora"));
    highlight->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(info->text().isEmpty(), 15000);
    highlight->setChecked(false);
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

QTEST_MAIN(tst_WebView)
#include "tst_webview.moc"
