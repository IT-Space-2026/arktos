/* CEF01 spike probe — CefBrowserHost inside a plain QWidget.
 *
 * Standalone qmake project — NOT in the top-level SUBDIRS build.
 *   source .devin/qt-env.sh
 *   export CEF_ROOT=<extracted cef_binary_*_linux64 with the
 *                    libcef_dll_wrapper target built — see
 *                    spikes/cef/README.md>
 *   ./build-probe.sh && ./run-render.sh
 *
 * Two render legs (driven by run-render.sh):
 *
 *   --mode=osr       Offscreen: CefWindowInfo::SetAsWindowless(winId) +
 *                    CefSettings.windowless_rendering_enabled=1. Under
 *                    OSR the browser is always Alloy-style (CEF header
 *                    contract) — this is the Wayland-safe embed path and
 *                    the spec's blessed headless verification.
 *                    CefRenderHandler::OnPaint (BGRA) -> QImage ->
 *                    QWidget -> PNG + witness-color analysis.
 *
 *   --mode=windowed  Windowed X11 (xvfb only — needs a real winId):
 *                    CefWindowInfo::SetAsChild(winId, rect) reparents a
 *                    browser window inside the QWidget. runtime_style
 *                    defaults to CEF_RUNTIME_STYLE_CHROME (--style=alloy
 *                    opts out) — chrome style is the extension-capable
 *                    shape proven in the CEF01 extension test.
 *                    Proof = OnLoadEnd + title delivery + QScreen::
 *                    grabWindow(0) PNG witness-color analysis.
 *
 * Subprocess model: CEF re-executes this binary with --type=<renderer|
 * gpu-process|utility> — the CefExecuteProcess fast-path at the top of
 * main() dispatches those before Qt ever initializes.
 */
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QDebug>
#include <QHash>
#include <QVector>
#include <atomic>
#include <cstring>
#include <functional>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_command_line.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

namespace {

bool gOsr = true;           // --mode=osr|windowed (set before app creation)

/* The one CefApp instance is handed to CefExecuteProcess (subprocess
 * roles) and CefInitialize (browser role). OnBeforeCommandLineProcessing
 * runs in every process — process_type is empty only in the browser
 * process, but --disable-gpu is harmless everywhere.
 *
 * Message-loop model: multi_threaded_message_loop=1. On Linux CEF
 * effectively requires it — external_message_pump made CefInitialize's
 * ContentMainRun exit with code 28 (measured on this host). Consequence
 * for the probe and for CEF02's backend design: every handler below
 * fires on CEF's own UI thread, never the Qt GUI thread — anything
 * Qt-side is reached through QMetaObject::invokeMethod marshals or
 * atomics only. */
class ProbeApp : public CefApp
{
public:
    // DISALLOW_COPY_AND_ASSIGN suppresses the implicit default ctor.
    ProbeApp() = default;

    void OnBeforeCommandLineProcessing(
        const CefString &process_type,
        CefRefPtr<CefCommandLine> command_line) override
    {
        // Chrome-bootstrap first-run tasks block CefInitialize on any
        // fresh profile — same suppression cefsimple needs.
        if (process_type.empty()) {
            command_line->AppendSwitch(
                QStringLiteral("no-first-run").toStdString());
            command_line->AppendSwitch(
                QStringLiteral("no-default-browser-check").toStdString());
        }
        if (qgetenv("CEF_NO_DISABLE_GPU").isEmpty())
            command_line->AppendSwitch(
                QStringLiteral("disable-gpu").toStdString());
        // Escape hatch for flag experiments without a rebuild:
        //   CEF_SWITCHES="use-angle=swiftshader enable-unsafe-swiftshader"
        const QStringList extra =
            QString::fromUtf8(qgetenv("CEF_SWITCHES"))
                .split(' ', Qt::SkipEmptyParts);
        for (const QString &s : extra) {
            const int eq = s.indexOf('=');
            if (eq > 0)
                command_line->AppendSwitchWithValue(
                    s.left(eq).toStdString(), s.mid(eq + 1).toStdString());
            else
                command_line->AppendSwitch(s.toStdString());
        }
        if (process_type.empty()
            && !qgetenv("CEF_OZONE").isEmpty()) {
            command_line->AppendSwitchWithValue(
                QStringLiteral("ozone-platform").toStdString(),
                QString::fromUtf8(qgetenv("CEF_OZONE")).toStdString());
        }
    }

private:
    IMPLEMENT_REFCOUNTING(ProbeApp);
    DISALLOW_COPY_AND_ASSIGN(ProbeApp);
};

// ------------------------- probe widget + client -------------------------

/* GUI-thread sink for everything the CEF UI thread reports. The CEF
 * handlers NEVER touch this directly — they marshal through
 * QMetaObject::invokeMethod(..., Qt::QueuedConnection), so every field
 * here is single-threaded (the GUI thread). */
class RasterWidget : public QWidget
{
public:
    void adoptFrame(const QImage &img)
    {
        m_frame = img;
        m_frameSize = img.size();
        ++m_frameCount;
        update();
    }
    void setTitle(const QString &t) { m_title = t; }

    QImage frame() const { return m_frame; }
    QSize  frameSize() const { return m_frameSize; }
    QString title() const { return m_title; }
    int    frameCount() const { return m_frameCount; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // Sentinel background: pixels still magenta in a saved shot are
        // places the imported frame never covered (hole / no frame).
        QPainter p(this);
        p.fillRect(rect(), QColor(0xff, 0x00, 0xff));
        if (!m_frame.isNull())
            p.drawImage(0, 0, m_frame);
    }

private:
    QImage  m_frame;
    QSize   m_frameSize;
    QString m_title;
    int     m_frameCount = 0;
};

struct ProbeState {
    std::atomic<bool> loadEnd{false};
    std::atomic<bool> loadErr{false};
    std::atomic<bool> closed{false};
};

/* CefClient facade. All handlers are invoked on CEF's own UI thread
 * (multi_threaded_message_loop), NOT the Qt GUI thread — Qt objects are
 * only reached through queued marshals. */
class ProbeClient : public CefClient,
                    public CefRenderHandler,
                    public CefLoadHandler,
                    public CefDisplayHandler,
                    public CefLifeSpanHandler
{
public:
    ProbeClient(ProbeState *s, RasterWidget *w) : m_s(s), m_w(w) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

    // -- CefRenderHandler (OSR leg only) --
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) override
    {
        // UI thread: reading widget geometry across threads is unsafe in
        // general, but QWidget::size() is a plain field read and the
        // probe never resizes — acceptable here, flagged for CEF02 to
        // do properly via a posted query.
        const QSize sz = m_w->size();
        rect = CefRect(0, 0, sz.width(), sz.height());
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                 const RectList &, const void *buffer,
                 int width, int height) override
    {
        if (type != PET_VIEW || !buffer || width <= 0 || height <= 0)
            return;
        // CEF delivers BGRA premultiplied; on little-endian that maps
        // 1:1 onto QImage::Format_ARGB32_Premultiplied (same trick the
        // WPE harness uses for its BGRA import). Deep copy, then hand
        // the frame to the GUI thread.
        QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
        memcpy(img.bits(), buffer, size_t(width) * size_t(height) * 4u);
        RasterWidget *w = m_w;
        QMetaObject::invokeMethod(w, [w, img] { w->adoptFrame(img); },
                                  Qt::QueuedConnection);
    }

    // -- CefLoadHandler --
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override
    {
        if (frame && frame->IsMain()) {
            qInfo("[cef] OnLoadEnd http=%d", httpStatusCode);
            m_s->loadEnd = true;
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                     ErrorCode errorCode, const CefString &errorText,
                     const CefString &failedUrl) override
    {
        if (frame && frame->IsMain()) {
            qWarning("[cef] OnLoadError %d %s %s", int(errorCode),
                     qPrintable(QString::fromStdString(errorText.ToString())),
                     qPrintable(QString::fromStdString(failedUrl.ToString())));
            m_s->loadErr = true;
        }
    }

    // -- CefDisplayHandler --
    void OnTitleChange(CefRefPtr<CefBrowser>,
                       const CefString &title) override
    {
        const QString t = QString::fromStdString(title.ToString());
        qInfo("[cef] title=\"%s\"", qPrintable(t));
        RasterWidget *w = m_w;
        QMetaObject::invokeMethod(w, [w, t] { w->setTitle(t); },
                                  Qt::QueuedConnection);
    }

    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t,
                          const CefString &message, const CefString &,
                          int) override
    {
        const QString m = QString::fromStdString(message.ToString());
        if (m.startsWith(QStringLiteral("CODEC ")))
            qInfo("[codec] %s", qPrintable(m));
        return false; // still let it hit stderr
    }

    // -- CefLifeSpanHandler --
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        m_browser = browser;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override
    {
        m_browser = nullptr;
        m_s->closed = true;
    }

    CefRefPtr<CefBrowser> browser() const { return m_browser; }

private:
    ProbeState           *m_s;
    RasterWidget         *m_w;
    CefRefPtr<CefBrowser> m_browser;

    IMPLEMENT_REFCOUNTING(ProbeClient);
    DISALLOW_COPY_AND_ASSIGN(ProbeClient);
};

// ------------------------- shot analysis -------------------------

/* Same contract as the WPE harness: a shot counts as a real render if
 * it has enough non-sentinel pixels, >= 6 distinct colors, no >98%
 * dominant fill, and every witness color present in quantity. */
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
            continue;
        const qint64 n = hist.value(0xff000000u | (wc & 0x00ffffffu));
        if (n < 64)
            miss << QStringLiteral("#%1:%2")
                        .arg(wc & 0xffffff, 6, 16, QChar('0')).arg(n);
    }
    const double share = total ? double(dominant) / double(total) : 1.0;
    *report = QStringLiteral("%1x%2 non-sentinel=%3/%4 distinct=%5 "
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

/* CefTask subclass for the UI-thread close — the trimmed-down
 * base::BindOnce that ships in the distro headers does not build
 * against plain GCC (incomplete OnceCallback), so the probe uses the
 * classic CefTask form CEF02 should also prefer. */
class CloseBrowserTask : public CefTask
{
public:
    explicit CloseBrowserTask(CefRefPtr<CefBrowserHost> host)
        : m_host(std::move(host)) {}
    void Execute() override
    {
        CEF_REQUIRE_UI_THREAD();
        m_host->CloseBrowser(true);
    }
private:
    CefRefPtr<CefBrowserHost> m_host;
    IMPLEMENT_REFCOUNTING(CloseBrowserTask);
    DISALLOW_COPY_AND_ASSIGN(CloseBrowserTask);
};

bool gMtl = false;

bool pumpUntil(QApplication &app, int timeoutMs,
               const std::function<bool()> &pred)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        if (!gMtl)
            CefDoMessageLoopWork(); // STML: service CEF on the GUI thread
        app.processEvents(QEventLoop::WaitForMoreEvents, 40);
    }
    return pred();
}

} // namespace

int main(int argc, char *argv[])
{
    // ---- subprocess fast-path (renderer/gpu/utility) — BEFORE Qt ----
    CefMainArgs main_args(argc, argv);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mode=windowed") == 0)
            gOsr = false;
    }
    CefRefPtr<ProbeApp> probeApp = new ProbeApp();
    const int subRc = CefExecuteProcess(main_args, probeApp, nullptr);
    if (subRc >= 0)
        return subRc;

    // ---- probe args ----
    const QString appDir = QFileInfo(QString::fromLocal8Bit(argv[0]))
                               .absolutePath();
    QString url, shotPath, style = QStringLiteral("chrome"),
            profileDir = QStringLiteral("/tmp/cefprobe-profile");
    int timeoutMs = 30000;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith("--url="))        url = a.mid(6);
        else if (a.startsWith("--shot="))  shotPath = a.mid(7);
        else if (a.startsWith("--timeout=")) timeoutMs = a.mid(10).toInt() * 1000;
        // NB: named --rtstyle because Qt itself eats a bare --style arg.
        else if (a.startsWith("--rtstyle=")) style = a.mid(10);
        else if (a.startsWith("--profile-dir=")) profileDir = a.mid(14);
        else if (a == "--no-profile-dir") profileDir.clear();
    }
    if (url.isEmpty())
        url = QStringLiteral("file://%1/fixture.html").arg(appDir);

    // ---- CEF init BEFORE Qt: cefsimple initializes first; ordering
    // CefInitialize after QApplication deadlocked on this host. ----
    gMtl = (qgetenv("CEF_MTL") == "1");
    CefSettings settings;
    settings.no_sandbox = 1;          // chrome-sandbox needs setuid root;
                                      // sandbox story is CEF13's problem
    // Message-loop model is env-switchable for the spike:
    //   CEF_MTL=1  multi_threaded_message_loop — CEF's own UI thread
    //   default    single-threaded — UI == Qt GUI thread, pumped by
    //              CefDoMessageLoopWork() from pumpUntil()
    // (external_message_pump was also tried — CefInitialize's
    //  ContentMainRun exits 28 under it on this host.)
    settings.multi_threaded_message_loop = gMtl ? 1 : 0;
    settings.windowless_rendering_enabled = gOsr ? 1 : 0;
    settings.log_severity = LOGSEVERITY_WARNING;
    if (!profileDir.isEmpty()) {
        CefString(&settings.root_cache_path) = profileDir.toStdString();
        CefString(&settings.cache_path) =
            (profileDir + QStringLiteral("/cache")).toStdString();
    }
    CefString(&settings.browser_subprocess_path) =
        QFile::encodeName(
            QFileInfo(QString::fromLocal8Bit(argv[0]))
                .absoluteFilePath()).constData();
    CefString(&settings.log_file) =
        QStringLiteral("/tmp/cefprobe-debug.log").toStdString();
    // Standard-distribution layout: binaries in Release/, resources
    // (icudtl.dat, *.pak, locales/) in Resources/ — two different roots,
    // and icu_util resolves icudtl via resources_dir_path.
    const QString resDir =
        QString::fromUtf8(qgetenv("CEF_RESOURCES_DIR"));
    if (!resDir.isEmpty()) {
        CefString(&settings.resources_dir_path) = resDir.toStdString();
        CefString(&settings.locales_dir_path) =
            (resDir + QStringLiteral("/locales")).toStdString();
    }

    qInfo("[probe] calling CefInitialize (pre-Qt, mtl=%d)", int(gMtl));
    if (!CefInitialize(main_args, settings, probeApp, nullptr)) {
        qWarning("FAIL: CefInitialize returned false");
        return 2;
    }
    qInfo("[cef] initialized (mode=%s style=%s)",
          gOsr ? "osr" : "windowed", qPrintable(style));

    qInfo("[probe] constructing QApplication");
    QApplication app(argc, argv);
    qInfo("[probe] QApplication up (platform=%s)",
          qPrintable(app.platformName()));
    if (!gOsr && app.platformName() != QStringLiteral("xcb")) {
        qWarning("FAIL: --mode=windowed needs a real X11 winId "
                 "(platform=%s) — run under xvfb-run",
                 qPrintable(app.platformName()));
        CefShutdown();
        return 2;
    }

    // ---- widget + browser ----
    ProbeState state;
    RasterWidget w;
    w.resize(800, 600);
    w.show();

    CefRefPtr<ProbeClient> client = new ProbeClient(&state, &w);

    CefWindowInfo wi;
    if (gOsr) {
        wi.SetAsWindowless(static_cast<CefWindowHandle>(w.winId()));
    } else {
        wi.SetAsChild(static_cast<CefWindowHandle>(w.winId()),
                      CefRect(0, 0, 800, 600));
        wi.runtime_style = (style == QStringLiteral("alloy"))
            ? CEF_RUNTIME_STYLE_ALLOY : CEF_RUNTIME_STYLE_CHROME;
    }

    CefBrowserSettings bs;
    bs.windowless_frame_rate = 60;
    bs.background_color = CefColorSetARGB(0xff, 0x40, 0x40, 0x40);

    // CreateBrowser is callable on any browser-process thread (async);
    // CreateBrowserSync is UI-thread-only and returned null under MTML.
    qInfo("[probe] creating browser (mtl=%d)", int(gMtl));
    if (!CefBrowserHost::CreateBrowser(wi, client.get(),
                                       url.toStdString(), bs,
                                       nullptr, nullptr)) {
        qWarning("FAIL: CreateBrowser returned false");
        CefShutdown();
        return 2;
    }
    if (!pumpUntil(app, timeoutMs,
                   [&] { return client->browser() != nullptr; })) {
        qWarning("FAIL: no browser after %d ms", timeoutMs);
        CefShutdown();
        return 2;
    }

    // ---- wait for load + frames ----
    const bool ready = pumpUntil(app, timeoutMs, [&] {
        if (state.loadErr)
            return true;
        return state.loadEnd
               && (gOsr ? w.frameCount() >= 1 : true);
    });
    if (gOsr) // let a second paint land so the shot isn't partial
        pumpUntil(app, 3000, [&] { return w.frameCount() >= 2; });
    if (!gOsr) // windowed: give the child window a moment to composite
        pumpUntil(app, 2000, [] { return false; });
    app.processEvents();

    int rc = 0;
    if (!ready || state.loadErr) {
        qWarning("FAIL: load did not complete (end=%d err=%d frames=%d)",
                 int(state.loadEnd.load()), int(state.loadErr.load()),
                 w.frameCount());
        rc = 1;
    } else {
        qInfo("[cef] load done: title=\"%s\" frames=%d size=%dx%d",
              qPrintable(w.title()), w.frameCount(),
              w.frameSize().width(), w.frameSize().height());
    }

    // ---- screenshot + analysis ----
    if (rc == 0 && !shotPath.isEmpty()) {
        QImage shot;
        if (gOsr) {
            shot = w.frame();
        } else {
            // Root-window grab captures the reparented child window's
            // pixels — proves the CEF view really composited into our
            // QWidget's X window.
            shot = app.primaryScreen()->grabWindow(0).toImage();
        }
        if (shot.isNull() || !shot.save(shotPath)) {
            qWarning("FAIL: shot save %s", qPrintable(shotPath));
            rc = 1;
        } else {
            static const QVector<QRgb> witness = {
                0xff0000u, 0x00ff00u, 0x0000ffu, 0xffffffu, 0x404040u
            };
            QString rep;
            const bool pass = analyzeShot(shot, witness, &rep);
            qInfo("[shot] %s -> %s", qPrintable(shotPath), qPrintable(rep));
            if (!pass) {
                qWarning("FAIL: shot analysis: %s", qPrintable(rep));
                rc = 1;
            }
        }
    }

    // ---- orderly close: CloseBrowser must run on the UI thread ----
    if (client->browser()) {
        CefRefPtr<CefBrowserHost> host = client->browser()->GetHost();
        if (gMtl) {
            CefPostTask(TID_UI, new CloseBrowserTask(host));
        } else {
            host->CloseBrowser(true); // already on the UI thread (STML)
        }
    }
    pumpUntil(app, 5000, [&] { return state.closed.load(); });
    CefShutdown();
    qInfo("qcefprobe rc=%d", rc);
    return rc;
}
