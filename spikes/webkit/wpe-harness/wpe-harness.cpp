/* WK02 spike: WPE WebKit harness skeleton + event-loop verdict.
 *
 * Standalone qmake project — NOT in the top-level SUBDIRS build.
 *   source .devin/webkit-env.sh && source .devin/qt-env.sh
 *   ./build-harness.sh && ./run-harness.sh
 *
 * What this proves (WK02):
 *   (a) WPEDisplayHeadless + WPEToplevel + WPEView + WebKitWebView at
 *       800x600 can be created inside a Qt Widgets process linked
 *       against the WK00 ~/wpe prefix (sid glibc recipe — see the .pro).
 *   (b) THE EVENT-LOOP QUESTION, first-class. WPE WebKit's UI process
 *       does all of its work — WPEWebProcess/WPENetworkProcess spawn,
 *       IPC replies, load-changed delivery — as GSources on a
 *       GMainContext. Somebody must pump it. Three candidate models:
 *
 *       --loop=glib    attempt 1: view objects on the Qt GUI thread;
 *                      rely on Qt's glib event dispatcher
 *                      (QEventDispatcherGlib pumps g_main_context_default()
 *                      whenever glib support is compiled in — Qt6 default
 *                      unless QT_NO_GLIB is set).
 *       --loop=timer   attempt 2: QT_NO_GLIB forces Qt onto
 *                      QEventDispatcherUNIX (select(); never touches
 *                      glib); a QTimer at ~60 Hz drains
 *                      g_main_context_iteration(ctx, false) on the GUI
 *                      thread. Proves manual pumping is viable.
 *       --loop=thread  attempt 3: a dedicated std::thread pushes a
 *                      PRIVATE GMainContext as its thread-default, owns
 *                      every WPE/WebKit object, runs g_main_loop_run,
 *                      and marshals results into Qt via
 *                      QMetaObject::invokeMethod + QueuedConnection.
 *                      Proves the objects need not live on the GUI
 *                      thread at all — only their marshaling must.
 *
 * EVENT-LOOP VERDICT (measured 2026-10-19, Qt 6.12.0 + WPE WebKit 2.54.1,
 * glib 2.80 — this is the threading contract the WebkitBackend/
 * WebkitPage work in WK07+ must follow):
 *
 *   ALL THREE MODELS WORK — load uri -> STARTED/COMMITTED/FINISHED
 *   delivered in every model; ./run-harness.sh prints PASS x3.
 *
 *   1. glib (PREFERRED): Qt6's default dispatcher here is
 *      QPAEventDispatcherGlib — it pumps the global-default GMainContext
 *      on the Qt GUI thread, which is exactly where WPE WebKit attaches
 *      its sources (WebKit binds g_main_context_get_thread_default(),
 *      which is the global default on the GUI thread). Zero extra code:
 *      WPE/WebKit objects can live on the GUI thread and the Qt event
 *      loop services them natively. THIS IS THE BACKEND MODEL.
 *   2. timer (fallback): with QT_NO_GLIB Qt drops to
 *      QUnixEventDispatcherQPA (select(); never touches glib). A 16 ms
 *      QTimer draining g_main_context_iteration(ctx, false) delivers
 *      load-finished fine. Use only if glib dispatch is unavailable.
 *   3. thread (works, fragile): private GMainContext pushed as
 *      thread-default on a std::thread + g_main_loop_run + queued
 *      QMetaObject::invokeMethod marshaling works — BUT two measured
 *      traps: (a) g_timeout_add/g_idle_add/etc. attach to the
 *      GLOBAL-default ctx (g_source_attach(NULL)), NOT thread-default —
 *      embedder sources must be g_source_attach()'d to the private ctx
 *      explicitly or they silently never fire; (b) the worker
 *      permanently becomes WebKit's "main thread" — process exit via
 *      normal teardown runs WebKit singleton destructors on the real
 *      main thread and self-SIGABRTs; exit must go through _exit or a
 *      coordinated on-worker shutdown (WebKit thread is process-
 *      lifetime). These traps make model 3 strictly worse than 1 here.
 *
 *   Ancillary WK02 findings:
 *   - WebKitWebView owns its WPEView in 2.54 — construct with
 *     "display" property (WPEDisplay), retrieve via
 *     webkit_web_view_get_wpe_view(), then wpe_view_set_toplevel +
 *     wpe_toplevel_resize + wpe_view_set_visible/map.
 *   - The Debian build bakes PKGLIBEXECDIR=/usr/lib/... for its
 *     subprocesses (WEBKIT_EXEC_PATH is DEVELOPER_MODE-only); the
 *     WK00 prefix was binary-patched to /home/szefi/wpe/wpe-webkit-2.0
 *     (symlink) — see ~/wpe/ACQUIRED.md.
 *   - The bwrap sandbox runs, but the subprocess can't see the sid
 *     glibc/prefix — whitelist it via
 *     webkit_web_context_add_path_to_sandbox (done in createViewStack)
 *     or set WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1.
 *
 * RENDER-PATH VERDICT (WK03, measured 2026-10-19 — see run-render.sh):
 *   - Frame delivery: the WebKitWebView's internal WPEView emits the
 *     GObject signal "buffer-rendered" (WPEView*, WPEBuffer*, ud) each
 *     time the web process presents a buffer. On the glib loop model it
 *     fires on the Qt GUI thread. The embedder only OBSERVES — the
 *     rendered/released bookkeeping stays WebKit-side (it is the
 *     wpe_view_render_buffer caller); wpe_buffer_import_to_pixels +
 *     deep copy inside the handler is the safe consume.
 *   - wpe_buffer_import_to_pixels returns a BORROWED GBytes owned by
 *     the buffer (never unref it — copy if keeping), ARGB8888
 *     premultiplied (BGRA bytes little-endian) — QImage::
 *     Format_ARGB32_Premultiplied matches. Stride:
 *     wpe_buffer_shm_get_stride for WPEBufferSHM, else tight w*4,
 *     always size-checked against the GBytes length.
 *   - Buffer type observed headless with CPU raster
 *     (WEBKIT_SKIA_ENABLE_CPU_RENDERING=1, exported by run-render.sh):
 *     WPEBufferDMABuf — import_to_pixels still maps it fine.
 *   - Resize: QWidget::resizeEvent -> wpe_toplevel_resize propagates to
 *     the view and the NEXT produced buffer carries the new dims.
 */

#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>
#include <QDebug>
#include <QWidget>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QHash>
#include <QList>

#include <atomic>
#include <cstring>
#include <functional>
#include <future>
#include <thread>
#include <unistd.h>

#include <glib.h>
#include <glib-object.h>
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/wpe-platform.h>

namespace {

const char *loadEventName(WebKitLoadEvent ev)
{
    switch (ev) {
    case WEBKIT_LOAD_STARTED:    return "STARTED";
    case WEBKIT_LOAD_REDIRECTED: return "REDIRECTED";
    case WEBKIT_LOAD_COMMITTED:  return "COMMITTED";
    case WEBKIT_LOAD_FINISHED:   return "FINISHED";
    }
    return "?";
}

// Everything WebKit-side for one view. In models 1+2 this lives on the
// Qt GUI thread; in model 3 it lives entirely on the worker thread.
struct ViewStack {
    WPEDisplay     *display  = nullptr;
    WPEToplevel    *toplevel = nullptr;
    WPEView        *view     = nullptr;
    WebKitWebView  *webView  = nullptr;
    GMainContext   *context  = nullptr; // thread-default ctx at create time
    GMainLoop      *loop     = nullptr; // model 3 only
    std::atomic<bool> loadFinished{false};
    std::atomic<bool> loadFailed{false};
    std::atomic<bool> processDead{false};
    std::atomic<bool> timedOut{false};
};

void onLoadChanged(WebKitWebView *, WebKitLoadEvent ev, ViewStack *s)
{
    qInfo("[wpe] load-changed %s", loadEventName(ev));
    if (ev == WEBKIT_LOAD_FINISHED) {
        s->loadFinished = true;
        if (s->loop) // model 3: quit the worker's loop from inside dispatch
            g_main_loop_quit(s->loop);
    }
}

gboolean onLoadFailed(WebKitWebView *, WebKitLoadEvent ev,
                      const gchar *uri, GError *err, ViewStack *s)
{
    qInfo("[wpe] load-failed event=%s uri=%s err=%s",
          loadEventName(ev), uri, err ? err->message : "?");
    s->loadFailed = true;
    return FALSE; // not handled — let WebKit's default run too
}

void onWebProcessTerminated(WebKitWebView *,
                            WebKitWebProcessTerminationReason reason,
                            ViewStack *s)
{
    qInfo("[wpe] web-process-terminated reason=%d", int(reason));
    s->processDead = true;
}

void onNotifyTitle(GObject *, GParamSpec *, ViewStack *s)
{
    gchar *title = nullptr;
    g_object_get(s->webView, "title", &title, nullptr);
    qInfo("[wpe] title=\"%s\"", title ? title : "");
    g_free(title);
}

bool createViewStack(ViewStack *s, QString *why)
{
    // Capture the thread-default context BEFORE any WebKit object —
    // that is the context all of WebKit's GSources will attach to.
    // (NULL means "global default" — g_main_context_iteration(NULL)
    // resolves it, but we keep the real pointer for reporting.)
    s->context = g_main_context_get_thread_default();
    if (!s->context)
        s->context = g_main_context_default();

    // The bubblewrap sandbox only bind-mounts what it knows about —
    // our sid-glibc tree under ~/wpe is not one of the standard binds,
    // so the patchelf'd WPEWebProcess (interp + libs there) must be
    // whitelisted explicitly. Without this the inner execvp fails with
    // ENOENT on the binary/interp. WEBKIT_DISABLE_SANDBOX_THIS_IS_
    // DANGEROUS=1 bypasses all of it (run-harness.sh --no-sandbox).
    const gchar *wpePrefix = g_getenv("ARKTOS_WPE_PREFIX");
    if (wpePrefix && *wpePrefix) {
        WebKitWebContext *webCtx = webkit_web_context_get_default();
        const char *dirs[] = { "/wpe-webkit-2.0",
                               "/usr/lib/x86_64-linux-gnu",
                               "/glibc/usr/lib/x86_64-linux-gnu",
                               "/compat" };
        for (const char *d : dirs) {
            gchar *p = g_build_filename(wpePrefix, d, nullptr);
            webkit_web_context_add_path_to_sandbox(webCtx, p, TRUE);
            g_free(p);
        }
    }

    s->display = wpe_display_headless_new();
    if (!s->display) { *why = QStringLiteral("wpe_display_headless_new returned null"); return false; }

    GError *err = nullptr;
    if (!wpe_display_connect(s->display, &err)) {
        *why = QStringLiteral("wpe_display_connect failed: %1").arg(err ? err->message : "?");
        g_clear_error(&err);
        return false;
    }
    // Anything that consults wpe_display_get_primary() internally lands here.
    wpe_display_set_primary(s->display);

    s->toplevel = wpe_display_create_toplevel(s->display, 1);
    if (!s->toplevel) { *why = QStringLiteral("wpe_display_create_toplevel returned null"); return false; }

    // WPE 2.54: WebKitWebView owns its WPEView internally; the display is
    // a construct-time property. (The legacy webkit_web_view_new(backend)
    // wpe_view_backend path still exists but is not what WK00 acquired.)
    GObjectClass *klass = G_OBJECT_CLASS(g_type_class_ref(WEBKIT_TYPE_WEB_VIEW));
    const bool hasDisplayProp =
        g_object_class_find_property(klass, "display") != nullptr;
    g_type_class_unref(klass);
    qInfo("[wpe] WebKitWebView construct props: display=%s",
          hasDisplayProp ? "yes" : "no");

    if (hasDisplayProp)
        s->webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                                  "display", s->display,
                                                  nullptr));
    else
        s->webView = webkit_web_view_new(nullptr); // binds the primary display
    if (!s->webView) { *why = QStringLiteral("WebKitWebView creation failed"); return false; }

    s->view = webkit_web_view_get_wpe_view(s->webView); // owned by webView
    if (!s->view) { *why = QStringLiteral("webkit_web_view_get_wpe_view returned null"); return false; }

    wpe_view_set_toplevel(s->view, s->toplevel);
    wpe_toplevel_resize(s->toplevel, 800, 600);
    wpe_view_set_visible(s->view, TRUE);
    wpe_view_map(s->view);

    g_signal_connect(s->webView, "load-changed",
                     G_CALLBACK(onLoadChanged), s);
    g_signal_connect(s->webView, "load-failed",
                     G_CALLBACK(onLoadFailed), s);
    g_signal_connect(s->webView, "web-process-terminated",
                     G_CALLBACK(onWebProcessTerminated), s);
    g_signal_connect(s->webView, "notify::title",
                     G_CALLBACK(onNotifyTitle), s);

    qInfo("[wpe] view stack created: display=%s toplevel=%dx%d "
          "context=%p (default=%p)",
          G_OBJECT_TYPE_NAME(s->display), 800, 600,
          (void *)s->context, (void *)g_main_context_default());
    return true;
}

void destroyViewStack(ViewStack *s)
{
    g_clear_object(&s->webView);
    g_clear_object(&s->toplevel);
    g_clear_object(&s->display);
}

bool finished(const ViewStack &s)
{
    return s.loadFinished || s.loadFailed || s.processDead;
}

// ------------------------- WK03: render path -------------------------

/* A QWidget whose paintEvent blits the newest imported WPEBuffer frame.
 * All WPE calls here run on the Qt GUI thread — the "buffer-rendered"
 * signal is emitted while pumping the global-default GMainContext,
 * which on the glib loop model (WK02 verdict, backend contract) IS the
 * GUI thread. */
class RasterWidget : public QWidget
{
public:
    ViewStack *stack = nullptr;
    QImage     frame;
    QSize      frameSize;
    int        frameCount  = 0;
    int        importFails = 0;

    void attach(ViewStack *s) { stack = s; }

    void adoptFrame(WPEBuffer *buffer, const void *data, int stride)
    {
        const int w = wpe_buffer_get_width(buffer);
        const int h = wpe_buffer_get_height(buffer);
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        const quint8 *src = static_cast<const quint8 *>(data);
        for (int y = 0; y < h; ++y)
            memcpy(img.scanLine(y), src + size_t(stride) * y, size_t(w) * 4);
        frame = img;
        frameSize = QSize(w, h);
        ++frameCount;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // Sentinel background: pixels still magenta in a saved shot are
        // places the imported frame never covered (hole / no frame).
        QPainter p(this);
        p.fillRect(rect(), QColor(0xff, 0x00, 0xff));
        if (!frame.isNull())
            p.drawImage(0, 0, frame);
    }
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        // WK03 resize contract: re-size the toplevel; the headless
        // toplevel propagates to its views and the next produced buffer
        // carries the new dims.
        if (stack && stack->toplevel)
            wpe_toplevel_resize(stack->toplevel,
                                e->size().width(), e->size().height());
    }
};

/* WPEView::buffer-rendered handler. WebKit (the wpe_view_render_buffer
 * caller) owns the rendered/released bookkeeping; the embedder only
 * imports + copies inside the handler. */
void onBufferRendered(WPEView *, WPEBuffer *buffer, RasterWidget *w)
{
    if (!buffer)
        return;
    GError *err = nullptr;
    // The returned GBytes is OWNED BY THE BUFFER (borrowed, not a copy,
    // never unref it) — valid while the buffer lives; we deep-copy into
    // a QImage inside the handler and keep nothing.
    GBytes *pixels = wpe_buffer_import_to_pixels(buffer, &err);
    if (!pixels) {
        ++w->importFails;
        qWarning("[wpe] wpe_buffer_import_to_pixels failed (%s): %s",
                 G_OBJECT_TYPE_NAME(buffer), err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    const int bw = wpe_buffer_get_width(buffer);
    const int bh = wpe_buffer_get_height(buffer);
    int stride = bw * 4;
    if (WPE_IS_BUFFER_SHM(buffer))
        stride = int(wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer)));
    gsize len = 0;
    const void *data = g_bytes_get_data(pixels, &len);
    if (stride <= 0 || bw <= 0 || bh <= 0
        || qint64(len) < qint64(stride) * (bh - 1) + bw * 4) {
        ++w->importFails;
        qWarning("[wpe] imported pixels undersized: %zu for %dx%d stride=%d",
                 size_t(len), bw, bh, stride);
    } else {
        if (w->frameCount == 0)
            qInfo("[wpe] first buffer: %s %dx%d stride=%d bytes=%zu",
                  G_OBJECT_TYPE_NAME(buffer), bw, bh, stride, size_t(len));
        w->adoptFrame(buffer, data, stride);
    }
}

void onBufferReleased(WPEView *, WPEBuffer *buffer, RasterWidget *)
{
    qInfo("[wpe] buffer-released %p", (void *)buffer);
}

/* A saved shot "counts" only if it looks like a real rendered page:
 * enough pixels that are NOT the sentinel background, several distinct
 * colors, no >98% dominant fill (rejects a solid painted blank page),
 * and every witness color present in quantity. */
bool analyzeShot(const QImage &img, const QVector<QRgb> &witness,
                 QString *report)
{
    QHash<QRgb, qint64> hist;
    hist.reserve(4096);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row =
            reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            ++hist[row[x] | 0xff000000u];
    }
    const qint64 total = qint64(img.width()) * img.height();
    qint64 dominant = 0;
    QRgb dom = 0;
    for (auto it = hist.cbegin(); it != hist.cend(); ++it) {
        if (it.value() > dominant) {
            dominant = it.value();
            dom = it.key();
        }
    }
    const qint64 sentinel = hist.value(0xffff00ffu);
    const qint64 nonBg = total - sentinel;
    QStringList miss;
    for (QRgb wc : witness) {
        if (wc > 0x00ffffffu)
            continue; // "--witness=none" sentinel — no color check
        const qint64 n = hist.value(0xff000000u | (wc & 0x00ffffffu));
        if (n < 64)
            miss << QStringLiteral("#%1:%2")
                        .arg(wc & 0xffffff, 6, 16, QChar('0')).arg(n);
    }
    const double share = total ? double(dominant) / double(total) : 1.0;
    *report = QStringLiteral("%1x%2 non-bg=%3/%4 distinct=%5 "
                             "dominant=#%6(%7%) witness-miss=%8")
                  .arg(img.width()).arg(img.height())
                  .arg(nonBg).arg(total).arg(hist.size())
                  .arg(dom & 0xffffff, 6, 16, QChar('0'))
                  .arg(share * 100.0, 0, 'f', 1)
                  .arg(miss.isEmpty() ? QStringLiteral("none")
                                      : miss.join(QStringLiteral(",")));
    return nonBg > 2000 && hist.size() >= 6 && share < 0.98
           && miss.isEmpty();
}

bool pumpUntil(QApplication &app, const ViewStack &s, int timeoutMs,
               const std::function<bool()> &pred)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && !s.processDead && t.elapsed() < timeoutMs)
        app.processEvents(QEventLoop::WaitForMoreEvents, 40);
    return pred();
}

// WK03 entry: real QWidget consumer + screenshot proof + resize leg.
int runRender(QApplication &app, const QString &uri,
              const QString &shotPath, const QString &resizeShotPath,
              const QSize &resizeTo, const QVector<QRgb> &witness,
              int timeoutMs)
{
    ViewStack s;
    QString why;
    if (!createViewStack(&s, &why)) {
        qWarning("FAIL view stack: %s", qPrintable(why));
        return 2;
    }

    RasterWidget w;
    w.resize(800, 600);
    w.attach(&s);
    w.show();

    g_signal_connect(s.view, "buffer-rendered",
                     G_CALLBACK(onBufferRendered), &w);
    g_signal_connect(s.view, "buffer-released",
                     G_CALLBACK(onBufferReleased), &w);

    qInfo("[wpe] issuing load: %s", qPrintable(uri));
    webkit_web_view_load_uri(s.webView, uri.toUtf8().constData());

    bool ok = pumpUntil(app, s, timeoutMs, [&] {
        return (s.loadFinished || s.loadFailed) && w.frameCount >= 1;
    }) && s.loadFinished && w.frameCount >= 1;
    // Let a follow-up frame land so the shot isn't a partial paint.
    pumpUntil(app, s, 1500, [&] { return w.frameCount >= 2; });
    app.processEvents(); // flush pending paints

    if (!ok) {
        qWarning("[wpe] no rendered frame (finished=%d failed=%d dead=%d "
                 "frames=%d import-fails=%d)",
                 int(s.loadFinished), int(s.loadFailed), int(s.processDead),
                 w.frameCount, w.importFails);
        destroyViewStack(&s);
        return 1;
    }
    qInfo("[wpe] frame #%d %dx%d imported (import-fails=%d)",
          w.frameCount, w.frameSize.width(), w.frameSize.height(),
          w.importFails);

    int rc = 0;
    const QImage shot = w.grab().toImage();
    if (!shot.save(shotPath)) {
        qWarning("FAIL shot save: %s", qPrintable(shotPath));
        rc = 1;
    } else {
        QString rep;
        const bool pass = analyzeShot(shot, witness, &rep);
        qInfo("[shot] %s -> %s", qPrintable(shotPath), qPrintable(rep));
        if (!pass) {
            qWarning("FAIL shot analysis: %s", qPrintable(rep));
            rc = 1;
        }
    }

    if (!resizeShotPath.isEmpty() && resizeTo.isValid()) {
        const int before = w.frameCount;
        w.resize(resizeTo);
        const bool resized = pumpUntil(app, s, 20000, [&] {
            return w.frameCount > before && w.frameSize == resizeTo;
        });
        if (!resized) {
            qWarning("FAIL resize: no %dx%d buffer within deadline "
                     "(frames=%d last=%dx%d)", resizeTo.width(),
                     resizeTo.height(), w.frameCount,
                     w.frameSize.width(), w.frameSize.height());
            rc = 1;
        } else {
            qInfo("[wpe] resize delivered %dx%d buffer (frame #%d)",
                  w.frameSize.width(), w.frameSize.height(), w.frameCount);
            app.processEvents();
            const QImage rshot = w.grab().toImage();
            if (!rshot.save(resizeShotPath)) {
                qWarning("FAIL resize shot save: %s",
                         qPrintable(resizeShotPath));
                rc = 1;
            } else {
                QString rep;
                const bool pass = analyzeShot(rshot, witness, &rep);
                qInfo("[shot] %s -> %s",
                      qPrintable(resizeShotPath), qPrintable(rep));
                if (!pass) {
                    qWarning("FAIL resize shot analysis: %s",
                             qPrintable(rep));
                    rc = 1;
                }
            }
        }
    }

    destroyViewStack(&s);
    return rc;
}

// Models 1+2: view on the Qt GUI thread. |pumpTimer| is null for the
// glib model (Qt's dispatcher does the work) and a 60 Hz drainer for
// the timer model.
int runOnGuiThread(QApplication &app, const QString &uri,
                   int timeoutMs, bool manualPump)
{
    ViewStack s;
    QString why;
    if (!createViewStack(&s, &why)) {
        qWarning("FAIL view stack: %s", qPrintable(why));
        return 2;
    }

    QTimer pump;
    if (manualPump) {
        pump.setInterval(16);
        QObject::connect(&pump, &QTimer::timeout, [&] {
            // ~60 Hz: dispatch every ready GSource on the thread-default
            // context. Bound the drain so an always-ready source (idle
            // funcs that re-arm) cannot starve Qt's own loop.
            for (int i = 0; i < 64; ++i) {
                if (!g_main_context_iteration(s.context, FALSE))
                    break;
            }
        });
        pump.start();
    }

    qInfo("[wpe] issuing load: %s", qPrintable(uri));
    webkit_web_view_load_uri(s.webView, uri.toUtf8().constData());

    QElapsedTimer t;
    t.start();
    while (!finished(s) && t.elapsed() < timeoutMs)
        app.processEvents(QEventLoop::WaitForMoreEvents, 40);
    const bool ok = s.loadFinished && !s.processDead;
    const qint64 ms = t.elapsed();

    if (ok) {
        gchar *title = nullptr;
        g_object_get(s.webView, "title", &title, nullptr);
        qInfo("[wpe] load FINISHED in %lld ms — uri=%s title=\"%s\"",
              (long long)ms,
              webkit_web_view_get_uri(s.webView), title ? title : "");
        g_free(title);
    } else {
        qWarning("[wpe] no load-finished within %d ms (failed=%d dead=%d)",
                 timeoutMs, int(s.loadFailed), int(s.processDead));
    }
    pump.stop();
    destroyViewStack(&s);
    return ok ? 0 : 1;
}

gboolean quitDeadline(gpointer data)
{
    ViewStack *s = static_cast<ViewStack *>(data);
    s->timedOut = true;
    if (s->loop)
        g_main_loop_quit(s->loop);
    return G_SOURCE_REMOVE;
}

/* Attach a timeout to ctx EXPLICITLY. glib's convenience helpers
 * (g_timeout_add, g_idle_add, g_child_watch_add, ...) attach to the
 * GLOBAL-default context — g_source_attach(source, NULL) does NOT mean
 * thread-default. On the dedicated-thread model that silently lands the
 * source on a context nobody pumps; the source must go to ctx directly.
 * (WebKit itself uses g_main_context_get_thread_default() internally,
 * which is why its sources were fine while ours never fired.) */
guint addTimeout(GMainContext *ctx, guint ms, GSourceFunc fn, gpointer data)
{
    GSource *src = g_timeout_source_new(ms);
    g_source_set_callback(src, fn, data, nullptr);
    guint id = g_source_attach(src, ctx);
    g_source_unref(src);
    return id;
}

// Model 3: dedicated std::thread owns ALL WPE/WebKit objects behind a
// private thread-default GMainContext; results marshal into Qt via
// QMetaObject::invokeMethod(QueuedConnection).
int runOnWorkerThread(QApplication &app, const QString &uri, int timeoutMs)
{
    std::promise<int> result;
    std::future<int> fut = result.get_future();

    std::thread worker([&app, uri, timeoutMs](std::promise<int> result) {
        GMainContext *ctx = g_main_context_new();
        g_main_context_push_thread_default(ctx);

        ViewStack s;
        s.loop = g_main_loop_new(ctx, FALSE);
        QString why;
        int rc = 2;
        if (createViewStack(&s, &why)) {
            // Deadline source attached EXPLICITLY to the private ctx —
            // g_timeout_add would land on the unpumped global default.
            addTimeout(ctx, timeoutMs, quitDeadline, &s);
            qInfo("[wpe] issuing load (worker): %s", qPrintable(uri));
            webkit_web_view_load_uri(s.webView, uri.toUtf8().constData());
            g_main_loop_run(s.loop);
            rc = (s.loadFinished && !s.processDead) ? 0 : 1;

            // The marshaling contract, demonstrated: a queued call into
            // the Qt GUI thread's event queue — the ONLY thing that may
            // cross the thread boundary.
            const bool ok = (rc == 0);
            QMetaObject::invokeMethod(&app, [ok] {
                qInfo("[qt ] worker marshaled verdict: %s",
                      ok ? "loadFinished" : "FAILED");
            }, Qt::QueuedConnection);
        } else {
            qWarning("FAIL view stack (worker): %s", qPrintable(why));
        }

        qInfo("[wpe] worker: loop exited (rc=%d), tearing down", rc);
        destroyViewStack(&s);
        qInfo("[wpe] worker: stack destroyed");
        g_main_loop_unref(s.loop);
        g_main_context_pop_thread_default(ctx);
        g_main_context_unref(ctx);
        qInfo("[wpe] worker: context released, setting result");
        result.set_value(rc);
    }, std::move(result));

    // Keep Qt's own loop alive while the worker runs — the point is
    // coexistence, not blocking.
    while (fut.wait_for(std::chrono::milliseconds(40))
               != std::future_status::ready)
        app.processEvents(QEventLoop::WaitForMoreEvents, 40);
    const int rc = fut.get();
    worker.join();
    app.processEvents(); // flush the queued verdict print

    // MODEL-3 TEARDOWN CONTRACT (measured, not theoretical): the worker
    // thread permanently becomes WebKit's "main thread" — its RunLoop::
    // main() binds to our pushed ctx and process-wide singletons
    // (default WebKitWebContext/WebProcessPool) affinitize to it.
    // Ordinary return → exit_group → static dtors/atexit on the REAL
    // main thread → WebKit teardown dispatched to the dead/foreign ctx
    // → self-SIGABRT (verified 2026-10-19, teardown-in-ctx does NOT
    // avoid it). Production contract: the WebKit thread lives for the
    // process's whole lifetime and exit goes through _exit, or the
    // backend drives a coordinated on-worker shutdown itself.
    qInfo("[wpe] worker verdict delivered; _exit(%d) bypasses "
          "main-thread static teardown", rc);
    _exit(rc);
    return rc; // unreachable
}

} // namespace

int main(int argc, char *argv[])
{
    // Loop model must be decided before QApplication — QT_NO_GLIB flips
    // Qt off the glib dispatcher for models 2+3 so they honestly prove
    // they don't depend on it.
    QString loopMode = QStringLiteral("glib");
    QString uri;
    QString shotPath, resizeShotPath;
    QSize resizeTo(640, 480);
    QVector<QRgb> witness;
    int timeoutMs = 30000;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith(QStringLiteral("--loop=")))
            loopMode = a.mid(7);
        else if (a.startsWith(QStringLiteral("--timeout=")))
            timeoutMs = a.mid(10).toInt() * 1000;
        else if (a.startsWith(QStringLiteral("--shot=")))
            shotPath = a.mid(7);
        else if (a.startsWith(QStringLiteral("--resize-shot=")))
            resizeShotPath = a.mid(14);
        else if (a.startsWith(QStringLiteral("--resize="))) {
            const QStringList xy = a.mid(9).split(QLatin1Char('x'));
            if (xy.size() == 2)
                resizeTo = QSize(xy[0].toInt(), xy[1].toInt());
        }
        else if (a.startsWith(QStringLiteral("--witness="))) {
            // comma-separated #rrggbb colors that must each appear
            // >=64x; "--witness=none" disables witness checks entirely
            // (arbitrary URIs) while keeping the density/distinct/
            // dominant-share gates.
            const QString v = a.mid(10);
            if (v == QLatin1String("none")) {
                witness.clear();
                witness << QRgb(0x1ffffff); // out-of-range sentinel:
            }                             // analyzeShot skips it; marks
                                          // "explicitly none"
            else {
                for (const QString &c : v.split(QLatin1Char(','))) {
                    const QString t = c.trimmed();
                    if (!t.isEmpty())
                        witness << QRgb(t.toUInt(nullptr, 16));
                }
            }
        }
        else if (!a.startsWith(QLatin1Char('-')))
            uri = a;
    }
    if (loopMode == QLatin1String("timer") || loopMode == QLatin1String("thread"))
        qputenv("QT_NO_GLIB", "1");

    QApplication app(argc, argv);
    if (uri.isEmpty()) {
        // Fixtures sit next to the sources; in-tree qmake puts the
        // binary there too. Render mode wants the color-witness fixture.
        const QString fixture =
            QCoreApplication::applicationDirPath()
            + (shotPath.isEmpty() ? QStringLiteral("/fixture.html")
                                  : QStringLiteral("/render-fixture.html"));
        uri = QUrl::fromLocalFile(fixture).toString();
    }
    QAbstractEventDispatcher *ed = QAbstractEventDispatcher::instance();

    qInfo("== wpe-harness ==");
    qInfo("Qt %s | dispatcher=%s | glib %u.%u.%u | WPE WebKit %u.%u.%u",
          qVersion(), ed ? ed->metaObject()->className() : "none",
          glib_major_version, glib_minor_version, glib_micro_version,
          webkit_get_major_version(), webkit_get_minor_version(),
          webkit_get_micro_version());
    qInfo("loop=%s timeout=%ds uri=%s", qPrintable(loopMode),
          timeoutMs / 1000, qPrintable(uri));
    if (ed && !strstr(ed->metaObject()->className(), "Glib")
        && loopMode == QLatin1String("glib"))
        qWarning("NOTE: Qt is not on a glib dispatcher — the glib model "
                 "cannot pump WebKit's context; expect a timeout (that "
                 "IS the attempt-1 failure this spike is probing)");

    int rc;
    if (!shotPath.isEmpty()) {
        // WK03 render-path proof — always the glib model (WK02 verdict).
        if (witness.isEmpty())
            witness = { 0xeef4ff, 0x123a6f, 0x22aa77 }; // render fixture
        rc = runRender(app, uri, shotPath, resizeShotPath, resizeTo,
                       witness, timeoutMs);
    } else if (loopMode == QLatin1String("thread"))
        rc = runOnWorkerThread(app, uri, timeoutMs);
    else if (loopMode == QLatin1String("timer"))
        rc = runOnGuiThread(app, uri, timeoutMs, true);
    else
        rc = runOnGuiThread(app, uri, timeoutMs, false);

    qInfo("%s %s", rc == 0 ? "PASS" : "FAIL",
          !shotPath.isEmpty() ? "render"
                              : qPrintable("loop=" + loopMode));
    return rc;
}
