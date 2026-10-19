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
 */

#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>
#include <QDebug>

#include <atomic>
#include <cstring>
#include <future>
#include <thread>
#include <unistd.h>

#include <glib.h>
#include <glib-object.h>
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>

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
    int timeoutMs = 30000;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith(QStringLiteral("--loop=")))
            loopMode = a.mid(7);
        else if (a.startsWith(QStringLiteral("--timeout=")))
            timeoutMs = a.mid(10).toInt() * 1000;
        else if (!a.startsWith(QLatin1Char('-')))
            uri = a;
    }
    if (loopMode == QLatin1String("timer") || loopMode == QLatin1String("thread"))
        qputenv("QT_NO_GLIB", "1");

    QApplication app(argc, argv);
    if (uri.isEmpty()) {
        // fixture.html sits next to the sources; in-tree qmake puts the
        // binary there too.
        const QString fixture =
            QCoreApplication::applicationDirPath() + QStringLiteral("/fixture.html");
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
    if (loopMode == QLatin1String("thread"))
        rc = runOnWorkerThread(app, uri, timeoutMs);
    else if (loopMode == QLatin1String("timer"))
        rc = runOnGuiThread(app, uri, timeoutMs, true);
    else
        rc = runOnGuiThread(app, uri, timeoutMs, false);

    qInfo("%s loop=%s", rc == 0 ? "PASS" : "FAIL", qPrintable(loopMode));
    return rc;
}
