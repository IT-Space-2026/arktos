// EXT13 Arch-A spike harness — can a hidden QWebEngineView at an
// extension's chrome-extension:// origin host its background script?
//
// Probes (each logs `hostview: <name> PASS|FAIL|INFO`):
//   load/enable          engine accepts the MV3 fixture on a named profile
//   inj-on-ext-page      profile script collection runs on chrome-extension:// pages
//   popup-pre-host       popup sendMessage before any hosted page exists
//                        (real SW attribution — 'real-sw' vs lastError)
//   foreign-profile      chrome-extension:// url on an extension-less profile
//   scheme-intercept     installUrlSchemeHandler("chrome-extension") accepted?
//   host-page            packaged host.html loads + script-tag boots sw.js
//   eval-csp             eval() inside an extension page (MV3 CSP)
//   msg-popup->host      popup sendMessage answered by the hosted listener
//   msg-cs->host         content-script sendMessage answered likewise
//   fetch+storage        chrome.storage round-trip + cross-origin fetch
//   aliveness            hidden view's timers keep running
//   bitwarden            real 3.4MB background.js under gapfill shim in a
//                        rewritten-package host page; dumps __report()
//   bw-msg               sendMessage to hosted BW from a second BW page
//
// Exit 0 when all stages ran (verdicts are data); 1 if any probe FAILED;
// 2 on harness fault.  NOT in the top-level build — see hostview.pro.

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtWebEngineCore/qtwebenginecore-config.h>
#include <QtWebEngineCore/QWebEngineExtensionInfo>
#include <QtWebEngineCore/QWebEngineExtensionManager>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWebEngineCore/QWebEngineScript>
#include <QtWebEngineCore/QWebEngineScriptCollection>
#include <QtWebEngineCore/QWebEngineUrlRequestJob>
#include <QtWebEngineCore/QWebEngineUrlSchemeHandler>
#include <QtWebEngineCore/QWebEnginePage>
#include <QtWebEngineWidgets/QWebEngineView>
#include <functional>
#include <memory>

static int failures = 0;
static void logLine(const QString &tag, const QString &detail = QString())
{
    qInfo().noquote() << "hostview:" << tag << detail;
    fflush(nullptr);
}
static void check(bool ok, const QString &what,
                  const QString &detail = QString())
{
    logLine(what, (ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                      + (detail.isEmpty() ? QString()
                                          : QStringLiteral(" ") + detail));
    if (!ok)
        ++failures;
}
static void note(const QString &what, const QString &detail)
{
    logLine(what, QStringLiteral("INFO ") + detail);
}

/* ------------------------------------------------ runJavaScript utils */

static void evalJS(QWebEnginePage *p, const QString &src,
                   std::function<void(const QVariant &)> cb)
{
    p->runJavaScript(src, [cb](const QVariant &v) { cb(v); });
}

/* Poll a JS expression until it returns non-null or maxTicks*200ms
 * passes.  cb(value,true) on hit, cb(_,false) on timeout. */
static void pollExpr(QWebEnginePage *p, const QString &expr, int maxTicks,
                     std::function<void(const QVariant &, bool)> cb)
{
    auto tries = std::make_shared<int>(0);
    auto *timer = new QTimer;
    QObject::connect(timer, &QTimer::timeout,
                     [p, expr, maxTicks, cb, timer, tries]() {
        p->runJavaScript(expr, [timer, tries, maxTicks, cb](const QVariant &v) {
            const bool hit = v.isValid() && !v.isNull();
            if (hit || ++*tries >= maxTicks) {
                timer->stop();
                timer->deleteLater();
                cb(v, hit);
            }
        });
    });
    timer->start(200);
}

static void loadAndWait(QWebEnginePage *p, const QUrl &url,
                        std::function<void(bool)> cb)
{
    auto done = std::make_shared<bool>(false);
    QObject::connect(p, &QWebEnginePage::loadFinished, p,
                     [cb, done](bool ok) {
        if (*done)
            return;
        *done = true;
        cb(ok);
    });
    p->load(url);
}

/* ---------------------------------------------------- stage machine */

static QList<std::function<void()>> stages;
static int stageIdx = -1;
static std::shared_ptr<bool> stageDone;
static void nextStage()
{
    ++stageIdx;
    if (stageIdx < stages.size()) {
        stageDone = std::make_shared<bool>(false);
        qInfo().noquote() << "hostview: === stage" << stageIdx << "===";
        fflush(nullptr);
        stages.at(stageIdx)();
    } else {
        qInfo().noquote() << "hostview: all stages ran, failures:"
                          << failures;
        fflush(nullptr);
        std::quick_exit(failures ? 1 : 0);
    }
}
/* A stage calls advance() exactly once; stragglers are ignored. */
static void advance()
{
    if (*stageDone)
        return;
    *stageDone = true;
    nextStage();
}
/* Per-stage watchdog — a stall logs FAIL and moves on. */
static void armStageTimeout(int ms, const QString &what)
{
    auto flag = stageDone;
    QTimer::singleShot(ms, [flag, what]() {
        if (*flag)
            return;
        logLine(what, QStringLiteral("FAIL stage-timeout"));
        ++failures;
        advance();
    });
}

/* ---------------------------------------------- scheme-handler probe */

class ProbeSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    bool fired = false;
    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        fired = true;
        auto *buf = new QBuffer;
        buf->setData("<title>synthetic</title>synthetic host page");
        buf->open(QIODevice::ReadOnly);
        job->reply("text/html", buf);
    }
};

/* --------------------------------------------------------- globals */

static QString fixtureDir;
static QString shimJsPath;
static QString bwZipPath;
static bool runBitwarden = true;
static QString reportOutPath;

static QWebEngineProfile *prof = nullptr;
static QWebEngineProfile *foreignProf = nullptr;
static QWebEngineExtensionManager *mgr = nullptr;
static QString extId;
static QWebEngineView *hostView = nullptr;
static QWebEngineView *popupView = nullptr;
static QWebEngineView *webView = nullptr;
static QTcpServer *server = nullptr;
static quint16 serverPort = 0;

static QString extUrl(const QString &file)
{
    return QStringLiteral("chrome-extension://%1/%2").arg(extId, file);
}

/* Enable helper — the engine arrives disabled after loadExtension. */
static void enableThen(const QString &id,
                       std::function<void(bool)> cb)
{
    for (const QWebEngineExtensionInfo &i : mgr->extensions())
        if (i.id() == id)
            mgr->setExtensionEnabled(i, true);
    auto tries = std::make_shared<int>(0);
    auto *t = new QTimer;
    QObject::connect(t, &QTimer::timeout, [id, cb, t, tries]() {
        bool enabled = false;
        for (const QWebEngineExtensionInfo &i : mgr->extensions())
            if (i.id() == id && i.isEnabled())
                enabled = true;
        if (enabled || ++*tries > 60) {
            t->stop();
            t->deleteLater();
            cb(enabled);
        }
    });
    t->start(150);
}

/* ------------------------------------------------------------- main */

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    runBitwarden = !args.contains(QLatin1String("--no-bitwarden"));

    fixtureDir = QFileInfo(QStringLiteral(
        "/home/szefi/Documents/arora-rust/spikes/ext/fixture-mv3"))
                         .absoluteFilePath();
    for (const QString &a : args) {
        if (!a.startsWith(QLatin1String("--")))
            fixtureDir = QFileInfo(a).absoluteFilePath();
    }
    shimJsPath = QFileInfo(fixtureDir
                           + QStringLiteral("/../common/chrome-shim.js"))
                     .absoluteFilePath();
    bwZipPath = QFileInfo(QStringLiteral(
        "/home/szefi/Documents/arora-rust/src/data/extensions/"
        "bitwarden-2026.9.3.zip")).absoluteFilePath();
    reportOutPath = QFileInfo(
        fixtureDir + QStringLiteral("/../results/bw-host-report.json"))
            .absoluteFilePath();

#if !QT_CONFIG(webengine_extensions)
    logLine(QStringLiteral("setup"),
            QStringLiteral("FATAL Qt built without webengine_extensions"));
    return 2;
#endif

    // Persistent named profiles — extensions are refused on OTR (EXT04).
    prof = new QWebEngineProfile(QStringLiteral("extspike"), &app);
    foreignProf =
        new QWebEngineProfile(QStringLiteral("extspike-foreign"), &app);
    mgr = prof->extensionManager();
    if (!mgr) {
        logLine(QStringLiteral("setup"),
                QStringLiteral("FATAL no extensionManager"));
        return 2;
    }

    // Loopback fixture for the content-script + fetch probes.
    server = new QTcpServer(&app);
    if (!server->listen(QHostAddress::LocalHost)) {
        logLine(QStringLiteral("setup"),
                QStringLiteral("FATAL cannot listen on loopback"));
        return 2;
    }
    serverPort = server->serverPort();
    QObject::connect(server, &QTcpServer::newConnection, server, [server]() {
        QTcpSocket *s = server->nextPendingConnection();
        QObject::connect(s, &QTcpSocket::readyRead, s, [s]() {
            s->readAll();
            const QByteArray body = "<html><body>cs fixture</body></html>";
            s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                     "Content-Length: " + QByteArray::number(body.size())
                     + "\r\nConnection: close\r\n\r\n" + body);
            s->disconnectFromHost();
        });
    });
    logLine(QStringLiteral("loopback"),
            QStringLiteral("INFO port=%1").arg(serverPort));

    /* ------------------------------------------- stage definitions */

    // S0: load + enable the MV3 fixture.
    stages << []() {
        auto done = std::make_shared<bool>(false);
        QObject::connect(mgr, &QWebEngineExtensionManager::loadFinished,
                         mgr, [done](const QWebEngineExtensionInfo &info) {
            if (*done
                || info.name() != QLatin1String("arora-ext-spike"))
                return;
            *done = true;
            check(info.isLoaded() && info.error().isEmpty(),
                  QStringLiteral("load-fixture"), info.error());
            extId = info.id();
            note(QStringLiteral("ext-id"), extId);
            enableThen(extId, [](bool enabled) {
                check(enabled, QStringLiteral("enable-fixture"));
                advance();
            });
        });
        mgr->loadExtension(fixtureDir);
    };
    // (each stage arms its own watchdog below via armStageTimeout —
    //  see the dispatch loop at the bottom)

    // S1: profile script collection on a chrome-extension:// page, plus
    // the popup's sendMessage BEFORE any hosted page exists — whoever
    // answers here is the real MV3 path (live SW or lastError).
    stages << []() {
        QWebEngineScript probe;
        probe.setName(QStringLiteral("ext13-inj-probe"));
        probe.setInjectionPoint(QWebEngineScript::DocumentCreation);
        probe.setWorldId(QWebEngineScript::MainWorld);
        probe.setRunsOnSubFrames(false);
        probe.setSourceCode(QStringLiteral("window.__injProbe=42;"));
        prof->scripts()->insert(probe);

        popupView = new QWebEngineView;
        auto *page = new QWebEnginePage(prof, popupView);
        popupView->setPage(page);
        loadAndWait(page, QUrl(extUrl(QStringLiteral("popup.html"))),
                    [page](bool ok) {
            check(ok, QStringLiteral("popup-on-own-profile"));
            evalJS(page, QStringLiteral(
                   "JSON.stringify({inj:window.__injProbe||null,"
                   "chrome:typeof chrome,"
                   "runtime:typeof (chrome&&chrome.runtime)})"),
                   [](const QVariant &v) {
                note(QStringLiteral("popup-context"), v.toString());
            });
            pollExpr(page,
                     QStringLiteral("window.__popupReply===undefined"
                                    "?null:"
                                    "JSON.stringify(window.__popupReply)"),
                     50, [](const QVariant &v, bool hit) {
                check(hit, QStringLiteral("popup-pre-host"),
                      v.toString());
                advance();
            });
        });
    };

    // S2: chrome-extension:// url on a profile without the extension.
    stages << []() {
        auto *v = new QWebEngineView;
        auto *page = new QWebEnginePage(foreignProf, v);
        v->setPage(page);
        loadAndWait(page, QUrl(extUrl(QStringLiteral("popup.html"))),
                    [page, v](bool ok) {
            evalJS(page, QStringLiteral(
                   "JSON.stringify({chrome:typeof chrome,"
                   "title:document.title})"),
                   [ok, v](const QVariant &r) {
                const QString s = r.toString();
                const bool noChrome = s.contains(
                    QLatin1String("\"chrome\":\"undefined\""));
                check(!ok || noChrome, QStringLiteral("foreign-profile"),
                      QStringLiteral("loadOk=%1 ctx=%2")
                          .arg(ok).arg(s));
                v->deleteLater();
                advance();
            });
        });
    };

    // S3: does Qt accept a custom handler on chrome-extension?  Probed
    // on the foreign profile so real pages stay reachable on prof.
    stages << []() {
        auto *handler = new ProbeSchemeHandler;
        foreignProf->installUrlSchemeHandler("chrome-extension", handler);
        auto *v = new QWebEngineView;
        auto *page = new QWebEnginePage(foreignProf, v);
        v->setPage(page);
        loadAndWait(page, QUrl(QStringLiteral(
                    "chrome-extension://zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz/"
                    "synthetic.html")),
                    [handler, v](bool ok) {
            check(handler->fired, QStringLiteral("scheme-intercept"),
                  QStringLiteral("requestStarted fired=%1 loadOk=%2")
                      .arg(handler->fired).arg(ok));
            v->deleteLater();
            advance();
        });
    };

    // S4: hosted background — hidden view at the extension origin runs
    // host.html; host-boot.js appends sw.js via <script> (CSP-safe).
    stages << []() {
        hostView = new QWebEngineView;
        auto *page = new QWebEnginePage(prof, hostView);
        hostView->setPage(page);
        loadAndWait(page, QUrl(extUrl(QStringLiteral("host.html"))),
                    [page](bool ok) {
            check(ok, QStringLiteral("host-page-load"));
            pollExpr(page, QStringLiteral(
                     "window.__hostBoot==='started'?null:"
                     "window.__hostBoot"),
                     40, [page](const QVariant &v, bool hit) {
                check(hit && v.toString()
                              == QLatin1String("sw.js-loaded"),
                      QStringLiteral("host-page"), v.toString());
                evalJS(page, QStringLiteral(
                       "JSON.stringify({evalProbe:window.__evalProbe,"
                       "swRan:window.__swRan||null,"
                       "vis:document.visibilityState,"
                       "onMsg:typeof (chrome&&chrome.runtime&&"
                       "chrome.runtime.onMessage),"
                       "storage:typeof (chrome&&chrome.storage&&"
                       "chrome.storage.local)})"),
                       [](const QVariant &r) {
                    const QString s = r.toString();
                    note(QStringLiteral("host-context"), s);
                    check(s.contains(
                              QLatin1String("\"onMsg\":\"object\"")),
                          QStringLiteral("host-onMessage-present"));
                    check(s.contains(
                              QLatin1String("\"storage\":\"object\"")),
                          QStringLiteral("host-storage-present"));
                    note(QStringLiteral("eval-csp"), s);
                    advance();
                });
            });
        });
    };

    // S5: popup -> hosted-background messaging.  The fixture sw.js
    // replies synchronously as 'hosted-page'; a live real SW would
    // answer ~250ms later and lose the first-response race.
    stages << []() {
        popupView->page()->runJavaScript(QStringLiteral(
            "window.__popupReply=undefined;"
            "chrome.runtime.sendMessage({ping:true,via:'popup2'},"
            "function(r){window.__popupReply={r:r,"
            "err:chrome.runtime.lastError?"
            "String(chrome.runtime.lastError.message):null};});"));
        pollExpr(popupView->page(),
                 QStringLiteral("window.__popupReply===undefined?null:"
                                "JSON.stringify(window.__popupReply)"),
                 50, [](const QVariant &v, bool hit) {
            check(hit, QStringLiteral("msg-popup->host"), v.toString());
            if (hit)
                check(v.toString().contains(
                          QLatin1String("hosted-page")),
                      QStringLiteral("msg-attr-hosted-page"),
                      v.toString());
            advance();
        });
    };

    // S6: content script -> hosted-background messaging.
    stages << []() {
        webView = new QWebEngineView;
        auto *page = new QWebEnginePage(prof, webView);
        webView->setPage(page);
        loadAndWait(page, QUrl(QStringLiteral("http://127.0.0.1:%1/")
                               .arg(serverPort)),
                    [page](bool ok) {
            check(ok, QStringLiteral("cs-page-load"));
            pollExpr(page, QStringLiteral(
                     "document.documentElement.getAttribute("
                     "'data-spike-cs')"),
                     50, [](const QVariant &v, bool hit) {
                check(hit, QStringLiteral("msg-cs->host"), v.toString());
                advance();
            });
        });
    };

    // S7: chrome.storage + fetch from the hosted context.
    stages << []() {
        hostView->page()->runJavaScript(QStringLiteral(
            "Promise.all(["
            "chrome.storage.local.set({spike:1}).then(function(){"
            "return chrome.storage.local.get('spike');"
            "}).then(function(r){return 'storage:'+r.spike;})"
            ".catch(function(e){return 'storage-err:'+e;}),"
            "fetch('http://127.0.0.1:%1/x').then(function(r){"
            "return 'fetch:'+r.status;}).catch(function(e){"
            "return 'fetch-err:'+e;})"
            "]).then(function(v){window.__fsResult=v.join(',');});")
            .arg(serverPort));
        pollExpr(hostView->page(),
                 QStringLiteral("window.__fsResult||null"), 50,
                 [](const QVariant &v, bool hit) {
            check(hit, QStringLiteral("fetch+storage"), v.toString());
            advance();
        });
    };

    // S8: hidden-view aliveness — timers, visibility.
    stages << []() {
        QTimer::singleShot(1200, []() {
            evalJS(hostView->page(), QStringLiteral(
                   "JSON.stringify({ticks:window.__ticks||0,"
                   "vis:document.visibilityState,"
                   "focus:document.hasFocus()})"),
                   [](const QVariant &v) {
                note(QStringLiteral("aliveness"), v.toString());
                const QJsonObject o = QJsonDocument::fromJson(
                    v.toString().toUtf8()).object();
                // Throttled timers are data, not failure — the page
                // stayed alive and answerable, which is the actual
                // question.  ticks<1 would mean fully frozen.
                check(o.value(QLatin1String("ticks")).toInt() >= 1,
                      QStringLiteral("aliveness"), v.toString());
                advance();
            });
        });
    };

    // S9: no-rewrite path — a script-collection bootstrap appends
    // sw.js into an EXISTING packaged page (popup.html) in a hidden
    // view.  No install-time package changes needed if this works.
    stages << []() {
        QWebEngineScript boot;
        boot.setName(QStringLiteral("ext13-inj-boot"));
        boot.setInjectionPoint(QWebEngineScript::DocumentCreation);
        boot.setWorldId(QWebEngineScript::MainWorld);
        boot.setRunsOnSubFrames(false);
        boot.setSourceCode(QStringLiteral(
            "(function(){"
            "if(location.protocol!=='chrome-extension:'"
            "||!/popup\\.html$/.test(location.pathname))return;"
            "document.addEventListener('DOMContentLoaded',function(){"
            "var s=document.createElement('script');s.src='sw.js';"
            "document.head.appendChild(s);});})();"));
        prof->scripts()->insert(boot);

        auto *v = new QWebEngineView;
        auto *page = new QWebEnginePage(prof, v);
        v->setPage(page);
        // Fresh view on the already-packaged popup.html — the injected
        // bootstrap must land at DocumentCreation and append sw.js
        // (the DOM append waits for DOMContentLoaded: documentElement
        // does not exist yet at creation time).
        loadAndWait(page, QUrl(extUrl(QStringLiteral("popup.html"))),
                    [page, v](bool ok) {
            check(ok, QStringLiteral("inj-host-page-load"));
            pollExpr(page, QStringLiteral("window.__swRan||null"), 40,
                     [v](const QVariant &r, bool hit) {
                check(hit && r.toString()
                              == QLatin1String("hosted-page"),
                      QStringLiteral("inj-boot-no-rewrite"),
                      r.toString());
                v->deleteLater();
                advance();
            });
        });
    };

    // S10: real Bitwarden background.js in a rewritten-package host page
    // under the gapfill shim — the compat-surface measurement.
    stages << []() {
        if (!runBitwarden) {
            note(QStringLiteral("bitwarden"),
                 QStringLiteral("skipped (--no-bitwarden)"));
            advance();
            return;
        }
        auto *tmp = new QTemporaryDir(
            QDir::temp().filePath(QStringLiteral("ext13-bw-XXXXXX")));
        tmp->setAutoRemove(false);   // quick_exit skips destructors
        const QString bwDir = tmp->path();
        const int rc = QProcess::execute(
            QStringLiteral("unzip"),
            {QStringLiteral("-o"), QStringLiteral("-q"), bwZipPath,
             QStringLiteral("-d"), bwDir});
        if (rc != 0) {
            check(false, QStringLiteral("bitwarden"),
                  QStringLiteral("unzip rc=%1").arg(rc));
            advance();
            return;
        }
        // Install-time rewrite (SEC25-collision flag): drop a host page
        // + the gapfill shim into the package.
        QFile::copy(shimJsPath,
                    bwDir + QStringLiteral("/chrome-shim.js"));
        auto put = [&bwDir](const QString &name, const QByteArray &data) {
            QFile f(bwDir + QLatin1Char('/') + name);
            if (f.open(QIODevice::WriteOnly))
                f.write(data);
        };
        put(QStringLiteral("shim-mode.js"),
            "globalThis.__CHROME_SHIM_MODE='gapfill';\n"
            // background.js was authored for a service worker — it can
            // call SW-only globals (importScripts).  The compat layer
            // has to provide them; map it onto a script-tag append.
            "if(typeof self!=='undefined')"
            "self.importScripts=function(u){var s=document"
            ".createElement('script');s.src=u;"
            "document.documentElement.appendChild(s);};\n");
        put(QStringLiteral("host-boot.js"), R"JS(
window.__hostBoot='started'; window.__ticks=0;
setInterval(function(){window.__ticks++;},50);
window.__bwErrors=[];
window.addEventListener('error',function(e){
  window.__bwErrors.push(String(e.message||e.error||e));});
var files=['shim-mode.js','chrome-shim.js','background.js'];
(function next(){
  var f=files.shift();
  if(!f){window.__hostBoot='all-loaded';return;}
  var s=document.createElement('script');s.src=f;s.onload=next;
  s.onerror=function(){window.__hostBoot='FAILED:'+f;};
  document.head.appendChild(s);
})();
)JS");
        put(QStringLiteral("host.html"),
            "<!doctype html><meta charset=\"utf-8\"><title>bw-host</title>"
            "<body>bw host</body><script src=\"host-boot.js\"></script>\n");

        auto conn = std::make_shared<QMetaObject::Connection>();
        auto bwId = std::make_shared<QString>();
        auto tmpGuard = std::shared_ptr<QTemporaryDir>(tmp);
        *conn = QObject::connect(
            mgr, &QWebEngineExtensionManager::loadFinished, mgr,
            [conn, bwId, tmpGuard, bwDir](
                const QWebEngineExtensionInfo &i) {
            if (!i.name().contains(QLatin1String("itwarden"),
                                   Qt::CaseInsensitive))
                return;
            QObject::disconnect(*conn);
            check(i.isLoaded() && i.error().isEmpty(),
                  QStringLiteral("bw-load"), i.error());
            if (!i.isLoaded()) {
                advance();
                return;
            }
            *bwId = i.id();
            note(QStringLiteral("bw-id"), *bwId);
            enableThen(*bwId, [bwId, tmpGuard](bool enabled) {
                check(enabled, QStringLiteral("bw-enable"));
                auto *v = new QWebEngineView;
                auto *page = new QWebEnginePage(prof, v);
                v->setPage(page);
                const QUrl hostUrl(QStringLiteral(
                    "chrome-extension://%1/host.html").arg(*bwId));
                loadAndWait(page, hostUrl, [page, tmpGuard](bool ok) {
                    check(ok, QStringLiteral("bw-host-page-load"));
                    pollExpr(page, QStringLiteral(
                             "window.__hostBoot==='started'?null:"
                             "window.__hostBoot"),
                             150 /* 30s for the 3.4MB webpack eval */,
                             [page, tmpGuard](const QVariant &v,
                                              bool hit) {
                        note(QStringLiteral("bw-boot"), v.toString());
                        check(hit, QStringLiteral("bw-background-eval"),
                              v.toString());
                        evalJS(page, QStringLiteral(
                               "JSON.stringify({report:(window.__report?"
                               "__report():null),"
                               "bwErrors:window.__bwErrors||[]})"),
                               [tmpGuard](const QVariant &r) {
                            const QString s = r.toString();
                            note(QStringLiteral("bw-report-bytes"),
                                 QString::number(s.size()));
                            const QJsonObject o = QJsonDocument::fromJson(
                                s.toUtf8()).object();
                            const QString inner = o.value(
                                QLatin1String("report")).toString();
                            const QJsonObject rep =
                                QJsonDocument::fromJson(inner.toUtf8())
                                    .object();
                            note(QStringLiteral("bw-apiLog-count"),
                                 QString::number(
                                     rep.value(QLatin1String("apiLog"))
                                         .toArray().size()));
                            note(QStringLiteral("bw-errors"),
                                 QString::number(
                                     rep.value(QLatin1String("errors"))
                                         .toArray().size())
                                 + QLatin1String(" uncaught:")
                                 + QString::number(
                                     o.value(QLatin1String("bwErrors"))
                                         .toArray().size()));
                            QFile out(reportOutPath);
                            if (out.open(QIODevice::WriteOnly)) {
                                QJsonObject wrap;
                                wrap.insert(QStringLiteral("outer"), o);
                                wrap.insert(QStringLiteral("report"),
                                            rep);
                                out.write(QJsonDocument(wrap).toJson());
                                note(QStringLiteral("bw-report-file"),
                                     reportOutPath);
                            }
                            advance();
                        });
                    });
                });
            });
        });
        mgr->loadExtension(bwDir);
    };

    // S10: sendMessage to the hosted BW from a second BW extension page
    // (cross-context proof against the real extension).
    stages << []() {
        QString bwId;
        for (const QWebEngineExtensionInfo &x : mgr->extensions())
            if (x.name().contains(QLatin1String("itwarden"),
                                  Qt::CaseInsensitive))
                bwId = x.id();
        if (!runBitwarden || bwId.isEmpty()) {
            note(QStringLiteral("bw-msg"),
                 QStringLiteral("no bw extension — skipped"));
            advance();
            return;
        }
        auto *v = new QWebEngineView;
        auto *page = new QWebEnginePage(prof, v);
        v->setPage(page);
        loadAndWait(page,
                    QUrl(QStringLiteral(
                             "chrome-extension://%1/popup/index.html")
                             .arg(bwId)),
                    [page](bool ok) {
            note(QStringLiteral("bw-second-page"),
                 QStringLiteral("loadOk=%1").arg(ok));
            page->runJavaScript(QStringLiteral(
                "window.__bwMsg=undefined;"
                "try{chrome.runtime.sendMessage({ping:true,"
                "via:'bw-page'},function(r){window.__bwMsg={"
                "replied:r!==undefined,"
                "err:chrome.runtime.lastError?"
                "String(chrome.runtime.lastError.message):null};});}"
                "catch(e){window.__bwMsg={threw:String(e)};}"));
            pollExpr(page, QStringLiteral(
                     "window.__bwMsg===undefined?null:"
                     "JSON.stringify(window.__bwMsg)"),
                     75, [](const QVariant &v, bool hit) {
                if (hit)
                    note(QStringLiteral("bw-msg"), v.toString());
                else
                    note(QStringLiteral("bw-msg"),
                         QStringLiteral("no reply (pending 15s)"));
                advance();
            });
        });
    };

    /* Stage dispatch: each stage gets a 60s stall watchdog (S9's BW
     * eval gets 120s via its own polls). */
    for (int i = 0; i < stages.size(); ++i) {
        const QString tag = QStringLiteral("stage%1").arg(i);
        const auto orig = stages.at(i);
        const int ms = (i == stages.size() - 2) ? 120000 : 60000;
        stages[i] = [orig, tag, ms]() {
            armStageTimeout(ms, tag);
            orig();
        };
    }

    QTimer::singleShot(300000, &app, []() {
        logLine(QStringLiteral("WATCHDOG"), QStringLiteral("timeout"));
        std::quick_exit(2);
    });

    nextStage();
    return app.exec();
}
