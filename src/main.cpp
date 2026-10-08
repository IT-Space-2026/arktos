/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "adblockpage.h"
#include "adblockresourcehandler.h"
#include "adblockrule.h"
#include "adblockschemeaccesshandler.h"
#include "adblocksubscription.h"
#include "acceptlanguagedialog.h"
#include "aroraicon.h"
#include "autofillmanager.h"
#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "browsertheme.h"
#include "clearprivatedata.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "extensionmanager.h"
#include "history.h"
#include "historymanager.h"
#include "historyparser.h"
#include "locationbar.h"
#include "modelmenu.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "opensearchreader.h"
#include "opensearchwriter.h"
#include "plaintexteditsearch.h"
#include "privacyrequestinterceptor.h"
#include "schemeaccesshandler.h"
#include "securestore.h"
#include "settings.h"
#include "sourcehighlighter.h"
#include "sourceviewer.h"
#include "startupprofile.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "tormanager.h"
#include "torsocks5.h"
#include "webpage.h"
#include "webview.h"
#include "webviewsearch.h"
#include "xbelreader.h"
#include "xbelwriter.h"

#include <QtCore/QBuffer>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QSet>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QXmlStreamReader>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPixmap>
#include <QtGui/QStandardItemModel>
#include <QtGui/QTextDocument>
#include <QtGui/QTextLayout>
#include <QtCore/QCoreApplication>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkCookie>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtWebEngineCore/QWebEngineClientHints>
#include <QtWebEngineCore/QWebEngineDownloadRequest>
#include <QtWebEngineCore/QWebEngineFindTextResult>
#include <QtWebEngineCore/QWebEngineLoadingInfo>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWebEngineCore/QWebEngineUrlRequestInfo>
#include <QtWebEngineCore/QWebEngineUrlRequestInterceptor>
#include <QtWebEngineCore/QWebEngineScript>
#include <QtWebEngineCore/QWebEngineScriptCollection>
#include <QtWebEngineCore/QWebEngineSettings>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QToolButton>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#endif

#if defined(ARORA_ADBLOCK_RUST)
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

// Normalizes a matcher decision for --adblock-rust-smoke:
// 0 allow, 1 block, 2 stub redirect (bundled resource name or data:
// URL), 3 rewritten request URL ($removeparam — the native matcher
// emits Allow + removeParams, the Rust engine emits Redirect to the
// stripped URL).
static int adblockDecisionKind(const AdBlockDecision &decision)
{
    if (decision.action == AdBlockDecision::Redirect) {
        if (!decision.redirectUrl.isEmpty()
            && !decision.redirectUrl.startsWith(QLatin1String("data:")))
            return 3;
        return 2;
    }
    if (decision.action == AdBlockDecision::Allow
        && !decision.removeParams.isEmpty())
        return 3;
    return int(decision.action);
}
#endif

// Records the request headers of every main-frame navigation so
// --sorry-smoke can see exactly what the engine puts on the wire
// (client hints included).  interceptRequest() runs on Chromium's IO
// thread — the captured output is mutex-guarded.
class ProbeHeaderCapture : public QWebEngineUrlRequestInterceptor
{
public:
    ProbeHeaderCapture(QMutex *mutex, QStringList *out)
        : m_mutex(mutex), m_out(out) {}

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        if (info.resourceType()
                != QWebEngineUrlRequestInfo::ResourceTypeMainFrame)
            return;
        const QHash<QByteArray, QByteArray> headers = info.httpHeaders();
        QStringList lines;
        lines.reserve(headers.size() + 1);
        lines << QString::fromLatin1(info.requestMethod())
                + QLatin1Char(' ') + info.requestUrl().toString();
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
            lines << QString::fromLatin1(it.key()) + QLatin1String(": ")
                     + QString::fromLatin1(it.value());
        const QMutexLocker lock(m_mutex);
        m_out->append(lines.join(QLatin1Char('\n')));
    }

private:
    QMutex *m_mutex;
    QStringList *m_out;
};

// TLS01: extracts the cipher-suite ids a TLS ClientHello offers, from
// a captured first record.  Used by --tls-smoke / --tls-off-smoke —
// the loopback listener never completes a handshake, the advertised
// list itself is the verdict.  Returns empty when the bytes are not
// (yet) a complete ClientHello record.
static QList<quint16> tlsClientHelloCiphers(const QByteArray &record)
{
    if (record.size() < 5 || quint8(record.at(0)) != 0x16)
        return {};
    const quint16 recordLength =
        quint16(quint8(record.at(3)) << 8 | quint8(record.at(4)));
    if (record.size() < 5 + recordLength)
        return {};
    const QByteArray body = record.mid(5, recordLength);
    // handshake: type(1)=ClientHello, length(3), version(2),
    // random(32), session-id length(1)+id, cipher-list length(2)+list
    if (body.size() < 39 || quint8(body.at(0)) != 0x01)
        return {};
    int p = 4 + 2 + 32;
    const int sessionIdLength = quint8(body.at(p));
    p += 1 + sessionIdLength;
    if (p + 2 > body.size())
        return {};
    const int listLength =
        quint8(body.at(p)) << 8 | quint8(body.at(p + 1));
    p += 2;
    if (p + listLength > body.size())
        return {};
    QList<quint16> ciphers;
    for (int i = p; i + 2 <= p + listLength; i += 2)
        ciphers.append(
            quint16(quint8(body.at(i)) << 8 | quint8(body.at(i + 1))));
    return ciphers;
}

#if defined(Q_OS_UNIX)
// TELEM01: --telemetry-smoke's capture proxy is a raw loopback socket
// with a blocking accept loop on a detached thread — it must already
// be listening when the BrowserApplication constructor runs, because
// consent-gated adblock fetches and the engine's flag latch can both
// happen that early, and QTcpServer cannot exist before the
// application.  Every accepted connection's first request line is
// recorded (mutex-guarded) and answered 502 so the caller fails fast.
static QMutex s_telemetryMutex;
static QStringList s_telemetryHits;

static void telemetryAcceptLoop(int listenFd)
{
    static const char reply[] =
        "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    for (;;) {
        const int fd = ::accept(listenFd, nullptr, nullptr);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            return;
        }
        char buffer[1024];
        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        const ssize_t n =
            (::poll(&pfd, 1, 3000) > 0)
                ? ::recv(fd, buffer, sizeof(buffer) - 1, 0) : -1;
        if (n > 0) {
            buffer[n] = '\0';
            const QByteArray firstLine =
                QByteArray(buffer, n).split('\n').first().trimmed();
            const QMutexLocker lock(&s_telemetryMutex);
            s_telemetryHits.append(QString::fromLatin1(firstLine));
        }
        ::send(fd, reply, sizeof(reply) - 1, MSG_NOSIGNAL);
        ::close(fd);
    }
}

// Opens the capture proxy: binds + listens on an ephemeral loopback
// port and starts the accept thread.  Returns the port, or 0 on
// failure.
static quint16 startTelemetryCapture()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return 0;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0
        || ::listen(fd, 32) != 0) {
        ::close(fd);
        return 0;
    }
    sockaddr_in bound;
    socklen_t length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound),
                      &length) != 0) {
        ::close(fd);
        return 0;
    }
    std::thread(telemetryAcceptLoop, fd).detach();
    return ntohs(bound.sin_port);
}
#endif

#if defined(Q_OS_LINUX)
// TELEM01: the capture proxy only sees proxy-aware traffic — raw UDP
// (QUIC, DNS) and any accidental direct connect would slip past it.
// /proc/net holds the connection table; matching remote endpoints
// against the socket inodes held by this process tree attributes them
// to us without flagging unrelated box traffic.
static QSet<QString> processRemoteEndpoints()
{
    QSet<qint64> pids;
    pids.insert(QCoreApplication::applicationPid());
    // Descendants (QtWebEngineProcess tree): /proc/<pid>/stat carries
    // ppid right after the last ')' — comm may contain spaces but no
    // closing paren.
    bool grew = true;
    while (grew) {
        grew = false;
        const QStringList procs = QDir(QLatin1String("/proc")).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : procs) {
            bool isPid = false;
            const qint64 pid = entry.toLongLong(&isPid);
            if (!isPid || pids.contains(pid))
                continue;
            QFile statFile(QLatin1String("/proc/") + entry
                           + QLatin1String("/stat"));
            if (!statFile.open(QIODevice::ReadOnly))
                continue;
            const QByteArray stat = statFile.readAll();
            const int paren = stat.lastIndexOf(')');
            if (paren == -1)
                continue;
            const QList<QByteArray> fields =
                stat.mid(paren + 1).simplified().split(' ');
            // fields[0] = state, fields[1] = ppid
            if (fields.count() > 1
                && pids.contains(fields.at(1).toLongLong())) {
                pids.insert(pid);
                grew = true;
            }
        }
    }

    QSet<QString> inodes;
    for (qint64 pid : pids) {
        const QDir fdDir(QStringLiteral("/proc/%1/fd").arg(pid));
        const QFileInfoList entries = fdDir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System);
        for (const QFileInfo &info : entries) {
            const QString target = info.symLinkTarget();
            if (target.startsWith(QLatin1String("socket:[")))
                inodes.insert(
                    target.mid(8, target.size() - 9));
        }
    }
    if (inodes.isEmpty())
        return QSet<QString>();

    QSet<QString> endpoints;
    static const char *const tables[] = { "tcp", "tcp6", "udp", "udp6" };
    for (const char *table : tables) {
        QFile file(QLatin1String("/proc/net/")
                   + QLatin1String(table));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QList<QByteArray> lines = file.readAll().split('\n');
        for (int i = 1; i < lines.count(); ++i) {
            const QList<QByteArray> fields =
                lines.at(i).simplified().split(' ');
            // sl local_address rem_address st tx_queue... inode (index 9)
            if (fields.count() < 10
                || !inodes.contains(
                    QString::fromLatin1(fields.at(9))))
                continue;
            const QByteArray remote = fields.at(2);
            const int colon = remote.indexOf(':');
            if (colon == -1)
                continue;
            bool ok = false;
            const quint16 port =
                remote.mid(colon + 1).toUShort(&ok, 16);
            if (!ok || port == 0)
                continue;
            // Each 8-hex-digit word is a 32-bit field printed
            // little-endian; reversing its bytes yields network order.
            const QByteArray hexAddr = remote.left(colon);
            QByteArray addr;
            for (int w = 0; w + 8 <= hexAddr.size(); w += 8) {
                for (int b = 6; b >= 0; b -= 2)
                    addr.append(
                        QByteArray::fromHex(hexAddr.mid(w + b, 2)).at(0));
            }
            QHostAddress host;
            if (addr.size() == 4) {
                host = QHostAddress(qFromBigEndian<quint32>(
                    reinterpret_cast<const uchar *>(addr.constData())));
            } else if (addr.size() == 16) {
                Q_IPV6ADDR ip6;
                memcpy(&ip6, addr.constData(), 16);
                host = QHostAddress(ip6);
            }
            if (host.isNull() || host.isLoopback())
                continue;
            endpoints.insert(host.toString() + QLatin1Char(':')
                             + QString::number(port));
        }
    }
    return endpoints;
}
#endif

// SEC15: --browseraudit-smoke drives the full browseraudit.com suite
// against the real browsing profile and writes every test's outcome
// to a JSON file for the .devin/SEC15-browseraudit.md baseline report.
// --browseraudit-bare swaps in a fresh unnamed off-the-record profile
// and a plain QWebEngineView — no CookieJar filter, request
// interceptor or WebPage navigation policy — so app-layer effects
// surface as divergences between the two captures.
//
// The injected capture wraps browserAuditTestFramework.start(): the
// suite's own runner (run.js) is fetched over the network only after
// the framework script has executed, so a 5ms in-page poller is
// guaranteed to install the wrapper first.  sendresults=false keeps
// the results local — nothing is posted back to the site.  The flag
// hits the live site and takes minutes, so it is deliberately absent
// from check-coverage's SMOKE_FLAGS.
static const char kBrowserAuditCaptureJs[] = R"JS(
(function () {
    var categoryPath = [];
    var poll = setInterval(function () {
        var framework = window.browserAuditTestFramework;
        if (!framework || framework.__aroraWrapped)
            return;
        var realStart = framework.start;
        framework.start = function (callbacks) {
            var onStartCategory = callbacks.startCategory;
            var onEndCategory = callbacks.endCategory;
            var onEndTest = callbacks.endTest;
            var onEndSuite = callbacks.endSuite;
            callbacks.startCategory = function () {
                categoryPath.push(this.id + "|" + String(this.title));
                if (onStartCategory)
                    return onStartCategory.apply(this, arguments);
            };
            callbacks.endCategory = function () {
                categoryPath.pop();
                if (onEndCategory)
                    return onEndCategory.apply(this, arguments);
            };
            callbacks.endTest = function (duration, result) {
                (window.__aroraResults = window.__aroraResults || [])
                    .push({
                        id: this.id,
                        title: String(this.title),
                        behaviour: String(this.behaviour),
                        outcome: String(result[0]),
                        reason: String(result[1] || ""),
                        duration: duration,
                        categories: categoryPath.slice()
                    });
                if (onEndTest)
                    return onEndTest.apply(this, arguments);
            };
            callbacks.endSuite = function (result) {
                window.__aroraSummary = result;
                window.__aroraDone = true;
                if (onEndSuite)
                    return onEndSuite.apply(this, arguments);
            };
            return realStart.call(this, callbacks);
        };
        framework.__aroraWrapped = true;
        clearInterval(poll);
    }, 5);
    // The suite scripts arrive via network fetch after
    // DOMContentLoaded; ten minutes is generous, and the C++ watchdog
    // ends the run regardless.
    setTimeout(function () { clearInterval(poll); }, 600000);
})();
)JS";

static int browserAuditSmoke(BrowserApplication &application,
                             WebView *appView, bool bare)
{
    const QString mode = bare ? QStringLiteral("bare")
                              : QStringLiteral("app");
    const QString outPath = qEnvironmentVariable(
        "ARORA_AUDIT_OUT",
        QStringLiteral("/tmp/browseraudit-%1.json").arg(mode));

    QWebEngineView *view = appView;
    if (bare) {
        // An unnamed profile is off-the-record — no persisted cookies,
        // cache or storage — and prepareProfile() never sees it, so no
        // app services reach this page: the bare-engine baseline.
        QWebEngineProfile *profile = new QWebEngineProfile(&application);
        QWebEngineView *bareView = new QWebEngineView;

        // SEC15 bisection aid: ARORA_AUDIT_WIRE=<csv> applies selected
        // pieces of prepareProfile()/WebPage wiring onto the OTR profile
        // so a divergent result can be attributed to one component.
        // Values: settings cookies interceptor extensions webpage webview
        // Modifier: noinject (with webview) disables the page's armed
        // DocumentReady scripts so scheduleRulesOnPage()/scheduleOnPage()
        // never inject.  Modifier: contained (without webview) puts the
        // plain view inside a shown QMainWindow like the webview wire
        // does, to separate widget-hierarchy effects from WebView
        // wiring.  Modifier: bar (without webview) adds a hidden child
        // widget like WebView's ScriptBlockInfoBar, to test whether a
        // child widget alone affects paint-gated resource loads.
        const QStringList wire = qEnvironmentVariable("ARORA_AUDIT_WIRE")
            .split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (wire.contains(QLatin1String("settings")))
            BrowserProfile::applySettings(profile);
        if (wire.contains(QLatin1String("cookies")))
            CookieJar::instance(profile);
        if (wire.contains(QLatin1String("interceptor")))
            profile->setUrlRequestInterceptor(new PrivacyRequestInterceptor(
                AdBlockManager::instance()->network(), profile));
        if (wire.contains(QLatin1String("extensions")))
            ExtensionManager::instance()->installOnProfile(profile);
        // webview adds the WebView/WebPage-level per-document scripts
        // (adblock cosmetic injection + autofill attach) that a plain
        // QWebEngineView+QWebEnginePage pairing never runs — SEC16
        // moved them off loadFinished into DocumentReady user scripts.
        if (wire.contains(QLatin1String("webview"))) {
            auto *container = new QMainWindow;
            WebView *webView = new WebView(profile, container);
            container->setCentralWidget(webView);
            container->resize(1024, 768);
            container->show();
            if (wire.contains(QLatin1String("noinject")))
                webView->webPage()->setInjectedScriptsEnabled(false);
            view = webView;
        } else {
            if (wire.contains(QLatin1String("webpage")))
                bareView->setPage(new WebPage(profile, bareView));
            else
                bareView->setPage(new QWebEnginePage(profile, bareView));
            // noinject disarms the page's own DocumentReady scripts so
            // the autofill/cosmetic wires below can re-arm a single
            // script in isolation.
            if (wire.contains(QLatin1String("noinject"))) {
                WebPage *wp = qobject_cast<WebPage*>(bareView->page());
                if (wp)
                    wp->setInjectedScriptsEnabled(false);
            }
            if (wire.contains(QLatin1String("autofill"))) {
                // Run the autofill half alone on the plain view —
                // arms the qwebchannel.js+autofill.js bundle at each
                // commit like WebPage::schedulePageScripts does.
                WebPage *wp = qobject_cast<WebPage*>(bareView->page());
                if (wp)
                    QObject::connect(wp,
                        &QWebEnginePage::urlChanged, bareView,
                        [wp](const QUrl &url) {
                            AutoFillManager::instance()
                                ->scheduleOnPage(wp, url);
                        });
            }
            if (wire.contains(QLatin1String("cosmetic"))) {
                // Run the adblock half alone on the plain view —
                // arms the cosmetic payload per commit like
                // WebPage::schedulePageScripts does.
                WebPage *wp = qobject_cast<WebPage*>(bareView->page());
                if (wp)
                    QObject::connect(wp,
                        &QWebEnginePage::urlChanged, bareView,
                        [wp](const QUrl &url) {
                            AdBlockManager::instance()->page()
                                ->scheduleRulesOnPage(wp, url);
                        });
            }
            if (wire.contains(QLatin1String("bar"))) {
                // Stand-in for the ScriptBlockInfoBar child that
                // WebView::init always creates — an extra child widget
                // over the render surface, even hidden.
                auto *standin = new QFrame(bareView);
                standin->setObjectName(
                    QLatin1String("scriptBlockInfoBar"));
                standin->hide();
            }
            // An unshown page is treated as hidden and Chromium
            // throttles its timers, which would stall the suite.
            if (wire.contains(QLatin1String("contained"))) {
                auto *container = new QMainWindow;
                container->setCentralWidget(bareView);
                container->resize(1024, 768);
                container->show();
            } else {
                bareView->resize(1024, 768);
                bareView->show();
            }
            view = bareView;
        }
    } else {
        // The smoke dispatch returns before main()'s window.show() —
        // show the stub window so the page is not treated as hidden.
        view->window()->show();
    }

    QWebEngineScript capture;
    capture.setName(QStringLiteral("arora-browseraudit-capture"));
    capture.setInjectionPoint(QWebEngineScript::DocumentCreation);
    capture.setWorldId(QWebEngineScript::MainWorld);
    capture.setRunsOnSubFrames(false);
    capture.setSourceCode(QString::fromUtf8(kBrowserAuditCaptureJs));
    view->page()->scripts().insert(capture);

    const QUrl suiteUrl(qEnvironmentVariable("ARORA_AUDIT_URL",
        QStringLiteral("https://browseraudit.com/test"
                       "?categories=*&sendresults=false")));
    qInfo() << "browseraudit-smoke: mode" << mode << "loading"
            << suiteUrl << "->" << outPath;
    view->load(suiteUrl);

    auto *progress = new int(-1);
    auto *finished = new bool(false);
    QTimer *poller = new QTimer(&application);
    poller->setInterval(2000);
    QObject::connect(poller, &QTimer::timeout, &application,
                     [=, &application]() {
        if (*finished)
            return;
        view->page()->runJavaScript(
            QStringLiteral("JSON.stringify({"
                           "done: window.__aroraDone === true,"
                           " n: (window.__aroraResults || []).length,"
                           " fw: typeof browserAuditTestFramework,"
                           " wrapped: !!(window.browserAuditTestFramework"
                           "            && browserAuditTestFramework"
                           "                  .__aroraWrapped),"
                           " ui: typeof browserAuditUI,"
                           " rs: document.readyState})"),
            [=, &application](const QVariant &status) {
                const QJsonObject state = QJsonDocument::fromJson(
                    status.toString().toUtf8()).object();
                const int count =
                    state.value(QLatin1String("n")).toInt(-1);
                if (!state.value(QLatin1String("done")).toBool()) {
                    static int heartbeat = 0;
                    if (count != *progress || ++heartbeat % 15 == 0) {
                        *progress = count;
                        qInfo() << "browseraudit-smoke: progress" << count
                                << "of 431 tests -" << status.toString();
                    }
                    return;
                }
                *finished = true;
                view->page()->runJavaScript(
                    QStringLiteral("JSON.stringify({"
                                   "userAgent: navigator.userAgent,"
                                   "summary: window.__aroraSummary || null,"
                                   "results: window.__aroraResults || []})"),
                    [=, &application](const QVariant &payload) {
                        const QByteArray json =
                            payload.toString().toUtf8();
                        QJsonParseError parseError;
                        const QJsonDocument doc =
                            QJsonDocument::fromJson(json, &parseError);
                        if (parseError.error != QJsonParseError::NoError) {
                            qInfo() << "browseraudit-smoke: FAIL"
                                    << "(capture parse:"
                                    << parseError.errorString()
                                    << ")";
                            application.exit(3);
                            return;
                        }
                        QJsonObject envelope = doc.object();
                        envelope.insert(QStringLiteral("profile"), mode);
                        envelope.insert(
                            QStringLiteral("chromiumVersion"),
                            QLatin1String(qWebEngineChromiumVersion()));
                        QFile out(outPath);
                        if (!out.open(QIODevice::WriteOnly)) {
                            qInfo() << "browseraudit-smoke: FAIL (cannot"
                                       " write" << outPath << ")";
                            application.exit(3);
                            return;
                        }
                        out.write(QJsonDocument(envelope).toJson());
                        const QJsonArray results =
                            envelope.value(QLatin1String("results"))
                                .toArray();
                        int pass = 0, warning = 0, critical = 0,
                            skip = 0;
                        for (const QJsonValue &value : results) {
                            const QString outcome = value.toObject()
                                .value(QLatin1String("outcome"))
                                .toString();
                            if (outcome == QLatin1String("pass"))
                                ++pass;
                            else if (outcome == QLatin1String("warning"))
                                ++warning;
                            else if (outcome == QLatin1String("critical"))
                                ++critical;
                            else
                                ++skip;
                        }
                        qInfo() << "browseraudit-smoke: DONE" << mode
                                << "-" << results.count() << "tests:"
                                << pass << "pass" << warning
                                << "warning" << critical << "critical"
                                << skip << "skip ->" << outPath;
                        application.exit(0);
                    });
            });
    });
    poller->start();

    int timeoutMs = qEnvironmentVariableIntValue("ARORA_AUDIT_TIMEOUT_MS");
    if (timeoutMs <= 0)
        timeoutMs = 30 * 60 * 1000;
    QTimer::singleShot(timeoutMs, &application, [&application]() {
        qInfo() << "browseraudit-smoke: FAIL (timeout)";
        application.exit(2);
    });

    return application.exec();
}

// ANON01: --anon-smoke loads the ipduh.com privacy test
// (https://ipduh.com/anonymity-check/ redirects to /privacy-test/) on
// the real browsing profile, waits for the page's own async probes to
// settle — the DNS-resolver identification (azax'/ms/ds/' polls), the
// response-header anomaly scan, the JS system-info table (doa24), font
// enumeration and the storage/cookie readbacks — then dumps the
// rendered report sections plus a set of direct JS probes (navigator,
// timezone, an RTCPeerConnection ICE-gather to verify LEAK01's armed
// WebRTC policy) to a JSON file for .devin/ANON01-report.md.  The flag
// hits a live site so it stays out of check-coverage's SMOKE_FLAGS,
// same as --browseraudit-smoke.
static const char kAnonIceProbeJs[] = R"JS(
(function () {
    window.__aroraIce = { done: false, candidates: [], error: null };
    window.addEventListener('load', function () {
        try {
            if (typeof RTCPeerConnection !== 'function') {
                window.__aroraIce.error = 'RTCPeerConnection unavailable';
                window.__aroraIce.done = true;
                return;
            }
            var pc = new RTCPeerConnection({ iceServers: [] });
            pc.onicecandidate = function (event) {
                if (event.candidate)
                    window.__aroraIce.candidates.push(
                        String(event.candidate.candidate));
            };
            pc.createDataChannel('x');
            pc.createOffer().then(function (offer) {
                return pc.setLocalDescription(offer);
            });
            setTimeout(function () {
                window.__aroraIce.done = true;
                try { pc.close(); } catch (e) {}
            }, 4000);
        } catch (e) {
            window.__aroraIce.error = String(e);
            window.__aroraIce.done = true;
        }
    });
})();
)JS";

static const char kAnonExtractJs[] = R"JS(
(function () {
    function text(id) {
        var el = document.getElementById(id);
        return el ? el.innerText : null;
    }
    var ids = ['loc_chk', 'doa24', 'srvdiv1', 'srvdiv2', 'srvdiv3',
               'hdrs_ph', 'js_http_headers', 'htm_response',
               'anmls_plchldr', 'local_storage', 'session_storage',
               'cookie_test', 'det_fonts', 'js_show_cookies', 'duhbot'];
    var sections = {};
    for (var i = 0; i < ids.length; ++i)
        sections[ids[i]] = text(ids[i]);
    var body = document.body ? document.body.innerText : '';
    return JSON.stringify({
        url: location.href,
        title: document.title,
        readyState: document.readyState,
        sections: sections,
        body: body.length > 200000 ? body.slice(0, 200000) : body,
        probes: {
            userAgent: navigator.userAgent,
            platform: navigator.platform,
            languages: navigator.languages,
            language: navigator.language,
            hardwareConcurrency: navigator.hardwareConcurrency,
            deviceMemory: navigator.deviceMemory,
            timezone: Intl.DateTimeFormat().resolvedOptions()
                         .timeZone,
            timezoneOffset: new Date().getTimezoneOffset(),
            webdriver: navigator.webdriver,
            cookiesEnabled: navigator.cookieEnabled,
            doNotTrack: navigator.doNotTrack,
            plugins: navigator.plugins.length,
            mimeTypes: navigator.mimeTypes.length,
            historyLength: history.length,
            screen: { w: screen.width, h: screen.height,
                      dpr: window.devicePixelRatio },
            ice: window.__aroraIce || null
        }
    });
})()
)JS";

static const char kAnonSettleJs[] = R"JS(
JSON.stringify({
    ice: !!(window.__aroraIce && window.__aroraIce.done),
    dns: ['srvdiv1', 'srvdiv2', 'srvdiv3'].filter(function (id) {
        var el = document.getElementById(id);
        return el && el.innerText.trim().length > 0;
    }).length,
    doa: !!(document.getElementById('doa24')
            && document.getElementById('doa24').innerText
                       .trim().length > 0),
    fonts: !!(document.getElementById('det_fonts')
              && document.getElementById('det_fonts').innerText
                         .trim().length > 0),
    hdrs: !!(document.getElementById('js_http_headers')
             && document.getElementById('js_http_headers').innerText
                        .trim().length > 0),
    rs: document.readyState
})
)JS";

static int anonSmoke(BrowserApplication &application, WebView *view)
{
    const QString outPath = qEnvironmentVariable(
        "ARORA_ANON_OUT",
        QStringLiteral("/tmp/anon-check.json"));

    view->window()->resize(1024, 768);
    view->window()->show();

    QWebEngineScript iceProbe;
    iceProbe.setName(QStringLiteral("arora-anon-ice-probe"));
    iceProbe.setInjectionPoint(QWebEngineScript::DocumentCreation);
    iceProbe.setWorldId(QWebEngineScript::MainWorld);
    iceProbe.setRunsOnSubFrames(false);
    iceProbe.setSourceCode(QString::fromUtf8(kAnonIceProbeJs));
    view->page()->scripts().insert(iceProbe);

    const QUrl target(qEnvironmentVariable("ARORA_ANON_URL",
        QStringLiteral("https://ipduh.com/privacy-test/")));
    qInfo() << "anon-smoke: loading" << target << "->" << outPath;

    auto *loaded = new bool(false);
    QObject::connect(view, &QWebEngineView::loadFinished,
                     &application, [loaded](bool ok) {
        *loaded = ok;
    });
    view->load(target);

    // Settle poll: the DNS-resolver readouts, header anomaly scan and
    // font/storage probes all fill in asynchronously after load —
    // extract once they look populated, or when the generous deadline
    // passes (a stalled probe must not lose the rest of the report).
    const QElapsedTimer deadline = [] {
        QElapsedTimer timer;
        timer.start();
        return timer;
    }();
    int settleMs = qEnvironmentVariableIntValue("ARORA_ANON_SETTLE_MS");
    if (settleMs <= 0)
        settleMs = 60 * 1000;

    auto *poller = new QTimer(&application);
    poller->setInterval(1500);
    QObject::connect(poller, &QTimer::timeout, &application,
                     [=, &application]() {
        if (!*loaded || deadline.elapsed() > settleMs)
            return;
        view->page()->runJavaScript(
            QString::fromUtf8(kAnonSettleJs),
            [=, &application](const QVariant &status) {
                const QJsonObject state = QJsonDocument::fromJson(
                    status.toString().toUtf8()).object();
                const int dns = state.value(QLatin1String("dns"))
                                    .toInt();
                const bool settled =
                    state.value(QLatin1String("ice")).toBool()
                    && state.value(QLatin1String("doa")).toBool()
                    && state.value(QLatin1String("hdrs")).toBool()
                    && dns > 0;
                const bool giveUp = deadline.elapsed() > 30000;
                if (!settled && !giveUp) {
                    qInfo() << "anon-smoke: waiting" << status.toString();
                    return;
                }
                poller->stop();
                view->page()->runJavaScript(
                    QString::fromUtf8(kAnonExtractJs),
                    [=, &application](const QVariant &payload) {
                        const QByteArray json =
                            payload.toString().toUtf8();
                        QJsonParseError parseError;
                        QJsonObject envelope = QJsonDocument::fromJson(
                            json, &parseError).object();
                        if (parseError.error
                                != QJsonParseError::NoError) {
                            qInfo() << "anon-smoke: FAIL (extract parse:"
                                    << parseError.errorString() << ")";
                            application.exit(3);
                            return;
                        }
                        // Record the effective privacy posture so the
                        // report ties each flagged item to the setting
                        // that covers it.
                        const QSettings settings;
                        QJsonObject posture;
                        static const char *const keys[] = {
                            "privacy/webrtcIpProtection",
                            "privacy/secureDnsMode",
                            "privacy/secureDnsServer",
                            "privacy/httpsFirst",
                            "privacy/trimReferer",
                            "privacy/blockThirdPartyCookies",
                            "privacy/reportUtcTimezone",
                            "privacy/normalizeAcceptLanguage",
                            "privacy/tlsStrictCiphers",
                            "privacy/securityLevel",
                        };
                        for (const char *key : keys)
                            posture.insert(QLatin1String(key),
                                settings.value(QLatin1String(key))
                                    .toJsonValue());
                        envelope.insert(QStringLiteral("settings"),
                                        posture);
                        envelope.insert(
                            QStringLiteral("chromiumVersion"),
                            QLatin1String(
                                qWebEngineChromiumVersion()));
                        QFile out(outPath);
                        if (!out.open(QIODevice::WriteOnly)) {
                            qInfo() << "anon-smoke: FAIL (cannot write"
                                    << outPath << ")";
                            application.exit(3);
                            return;
                        }
                        out.write(QJsonDocument(envelope).toJson());
                        qInfo() << "anon-smoke: DONE dns=" << dns
                                << "settled=" << settled
                                << "->" << outPath;
                        application.exit(0);
                    });
            });
    });
    poller->start();

    int timeoutMs = qEnvironmentVariableIntValue("ARORA_ANON_TIMEOUT_MS");
    if (timeoutMs <= 0)
        timeoutMs = 3 * 60 * 1000;
    QTimer::singleShot(timeoutMs, &application, [&application]() {
        qInfo() << "anon-smoke: FAIL (timeout)";
        application.exit(2);
    });

    return application.exec();
}

// SEC16: the adblock smokes below inject probe filters into the shared
// test-mode custom subscription and AdBlockManager's AutoSaver
// persists whatever the subscription still holds when the process
// exits.  A run that fails to clean up leaves its probes behind for
// every later run — a leftover "##body" element-hide rule restyled
// every page the browseraudit harness loaded.  Drop leftovers from
// earlier runs before snapshotting.
static const char *const kSmokeFilters[] = {
    "||adblock-smoke.invalid^",
    ".invalid^",
    "@@||allowed-smoke.invalid^",
    "##body",
    "||list-smoke.invalid^",
    "||ads.example.com^",
    "||banner.example^$script",
    "@@||banner.example^$script,domain=trusted.example",
    "||tracker.example^$third-party",
    "||cdn.example/lib.js$~third-party",
    "||redir.example/vast.xml$redirect=noop-vast-4.0",
    "||param.example^$removeparam=utm_source",
    "smoke.example##.ad-banner",
};

static void purgeSmokeFilters(AdBlockSubscription *custom)
{
    const QList<AdBlockRule> rules = custom->allRules();
    for (int i = rules.count() - 1; i >= 0; --i) {
        const QString filter = rules.at(i).filter();
        for (const char *smokeFilter : kSmokeFilters) {
            if (filter == QLatin1String(smokeFilter)) {
                custom->removeRule(i);
                break;
            }
        }
    }
}

// Snapshots every subscription's rules and enabled flag plus the
// subscription list itself, and restores them from a post-routine:
// those run inside ~QCoreApplication, before ~QObject tears down the
// children (NetworkAccessManager -> AdBlockManager) whose destructor
// performs the final saveIfNeccessary — so a smoke's mutations never
// reach disk on either the exit() or the early-return path.
struct AdBlockSmokeState {
    AdBlockSubscription *subscription;
    QList<AdBlockRule> rules;
    bool enabled;
};
static QList<AdBlockSmokeState> *s_adBlockSmokeSaved = nullptr;

static void restoreAdBlockState()
{
    if (!s_adBlockSmokeSaved)
        return;
    AdBlockManager *manager = AdBlockManager::instance();
    for (AdBlockSubscription *subscription : manager->subscriptions()) {
        bool savedBefore = false;
        for (const AdBlockSmokeState &state : *s_adBlockSmokeSaved) {
            if (state.subscription == subscription) {
                savedBefore = true;
                break;
            }
        }
        if (!savedBefore)
            manager->removeSubscription(subscription);
    }
    for (const AdBlockSmokeState &state : *s_adBlockSmokeSaved) {
        state.subscription->setEnabled(state.enabled);
        state.subscription->setRules(state.rules);
    }
    delete s_adBlockSmokeSaved;
    s_adBlockSmokeSaved = nullptr;
}

static void restoreAdBlockStateOnExit()
{
    if (s_adBlockSmokeSaved)
        return;
    AdBlockManager *manager = AdBlockManager::instance();
    s_adBlockSmokeSaved = new QList<AdBlockSmokeState>;
    for (AdBlockSubscription *subscription : manager->subscriptions())
        s_adBlockSmokeSaved->append({subscription,
                                     subscription->allRules(),
                                     subscription->isEnabled()});
    qAddPostRoutine(&restoreAdBlockState);
}

int main(int argc, char **argv)
{
    // Zero-cost wall clock for --perf-smoke's cold-start checkpoints.
    QElapsedTimer perfTimer;
    perfTimer.start();
    // PERF03: --profile-startup / ARORA_PROFILE_STARTUP=1 timeline —
    // marks print "profile-startup: <ms> <stage>" on stderr.
    StartupProfile::start();

    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);

    // Custom URL schemes must be registered before the application
    // exists: arora-file:// directory listings (MIG04), abp:subscribe
    // adblock links (MIG09), arora-resource:// bundled $redirect= stubs
    // (ADB01).  Apart from this constraint Qt6 WebEngineWidgets needs
    // no explicit initialize() call — QtWebEngineQuick::initialize()
    // is for the Quick module only; the engine spins up lazily with
    // the first page.
    SchemeAccessHandler::registerUrlSchemes();
    AdBlockSchemeAccessHandler::registerUrlScheme();
    AdBlockResourceHandler::registerUrlScheme();

    // The --*-smoke development runs keep their writes out of the
    // user's real data and settings locations.  argv is scanned before
    // the application exists because the BrowserApplication
    // constructor already brings up the browsing profile and its
    // managers.
    bool smokeRun = false;
    bool telemetrySmoke = false;
    bool dohSmoke = false;
    bool tlsSmoke = false;
    bool tlsOffSmoke = false;
    bool webrtcSmoke = false;
    bool webrtcOffSmoke = false;
    QVariant savedDohMode, savedDohServer, savedTlsStrict;
    QVariant savedWebrtcProtection;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg.startsWith("--") && arg.endsWith("-smoke"))
            smokeRun = true;
        // Modifier for --browseraudit-smoke's bare-engine baseline run;
        // on its own it still takes that path, so it is a smoke run too.
        if (arg == "--browseraudit-bare")
            smokeRun = true;
        if (arg == "--telemetry-smoke")
            telemetrySmoke = true;
        if (arg == "--doh-smoke")
            dohSmoke = true;
        if (arg == "--tls-smoke")
            tlsSmoke = true;
        if (arg == "--tls-off-smoke")
            tlsOffSmoke = true;
        if (arg == "--webrtc-smoke")
            webrtcSmoke = true;
        if (arg == "--webrtc-off-smoke")
            webrtcOffSmoke = true;
        if (arg == "--profile-startup")
            StartupProfile::enable();
    }
    if (smokeRun)
        QStandardPaths::setTestModeEnabled(true);

    // QSettings resolution needs the application identity; the
    // BrowserApplication constructor sets the same values again.
    QCoreApplication::setOrganizationName(QLatin1String("Arora"));
    QCoreApplication::setApplicationName(QLatin1String("Arora"));

    // DOH01: --doh-smoke seeds the strict custom-DoH mode with a dead
    // loopback endpoint BEFORE applyChromiumFlags reads the keys, so
    // the engine latches the DnsOverHttps feature switch exactly as a
    // configured profile would.  The smoke handler rewrites the
    // endpoint per stage.
    if (dohSmoke) {
        QSettings settings;
        settings.beginGroup(QLatin1String("privacy"));
        // Remember the real values — the smoke runs against the live
        // settings store (QSettings does not follow QStandardPaths'
        // test mode), so finish() restores them on the way out.
        savedDohMode = settings.value(QLatin1String("secureDnsMode"));
        savedDohServer = settings.value(QLatin1String("secureDnsServer"));
        settings.setValue(QLatin1String("secureDnsMode"), 3);
        settings.setValue(QLatin1String("secureDnsServer"),
                          QLatin1String("https://127.0.0.1:1/dns-query"));
        settings.endGroup();
    }

    // TLS01: the ClientHello smokes pin privacy/tlsStrictCiphers
    // BEFORE applyChromiumFlags reads it, so each run exercises the
    // setting deterministically regardless of the real store —
    // --tls-smoke forces it on, --tls-off-smoke forces it off for the
    // differential control.  The smoke's finish() restores the real
    // value on the way out (QSettings ignores the test-mode paths).
    if (tlsSmoke || tlsOffSmoke) {
        QSettings settings;
        settings.beginGroup(QLatin1String("privacy"));
        savedTlsStrict = settings.value(QLatin1String("tlsStrictCiphers"));
        settings.setValue(QLatin1String("tlsStrictCiphers"), tlsSmoke);
        settings.endGroup();
    }

    // LEAK01: --webrtc-smoke pins privacy/webrtcIpProtection BEFORE
    // applyChromiumFlags reads it, so the run deterministically
    // exercises the armed policy; --webrtc-off-smoke forces it off for
    // the differential control (unprotected ICE gathering must still
    // produce candidates, else a "no leak" verdict proves nothing).
    // The smoke's finish() restores the real value on the way out —
    // QSettings ignores the test-mode paths.
    if (webrtcSmoke || webrtcOffSmoke) {
        QSettings settings;
        settings.beginGroup(QLatin1String("privacy"));
        savedWebrtcProtection =
            settings.value(QLatin1String("webrtcIpProtection"));
        settings.setValue(QLatin1String("webrtcIpProtection"),
                          webrtcSmoke);
        settings.endGroup();
    }

    // TELEM01: --telemetry-smoke points both network stacks at a
    // loopback capture proxy so an unsolicited outbound attempt is
    // observed, not merely failed.  Everything must be in place before
    // the BrowserApplication constructor: Chromium's --proxy-server
    // latches with the engine flags when the browsing profile is
    // built, and the app-side fetch manager must read the capture
    // proxy settings early because the adblock consent path fetches
    // inside the same constructor.
    quint16 telemetryProxyPort = 0;
#if defined(Q_OS_UNIX)
    if (telemetrySmoke)
        telemetryProxyPort = startTelemetryCapture();
#endif
    if (telemetryProxyPort != 0) {
        const QByteArray endpoint =
            "http://127.0.0.1:" + QByteArray::number(telemetryProxyPort);
        QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        if (!flags.isEmpty())
            flags += ' ';
        flags += "--proxy-server=" + endpoint;
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
        QSettings settings;
        settings.beginGroup(QLatin1String("proxy"));
        settings.setValue(QLatin1String("enabled"), true);
        settings.setValue(QLatin1String("type"), 1);
        settings.setValue(QLatin1String("hostName"),
                          QLatin1String("127.0.0.1"));
        settings.setValue(QLatin1String("port"), int(telemetryProxyPort));
        settings.endGroup();
    }

    // PRIV02: the UTC-timezone normalization is process environment
    // (TZ) and must be in place before ANY engine initialization —
    // the BrowserApplication constructor already brings the browsing
    // profile up, so this runs even earlier.
    BrowserProfile::applyFingerprintEnvironment();

    // PRIV01 + TELEM01: the Chromium switches — WebRTC IP handling,
    // DoH auto-upgrade, and the background-traffic kill-list — are
    // appended to QTWEBENGINE_CHROMIUM_FLAGS, which the engine latches
    // when its context first spins up.  The application constructor
    // already builds the browsing profile, so this must run before it.
    BrowserProfile::applyChromiumFlags();

    BrowserApplication application(argc, argv);
    const qint64 appCtorMs = perfTimer.elapsed();
    StartupProfile::mark("BrowserApplication ctor");

    // A non-standalone run that could not take the single-instance
    // socket already forwarded its url to the running instance and is
    // done.  Standalone runs (any --option) never join the handshake.
    if (!application.isStandalone() && !application.isRunning())
        return 0;

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main",
            "Arora — a lightweight cross-platform web browser."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QLatin1String("url"),
        QCoreApplication::translate("main", "Url to open on startup."),
        QStringLiteral("[url...]"));
    parser.addOption(QCommandLineOption(
        QLatin1String("tor"),
        QCoreApplication::translate("main",
            "Open a Tor window: a separate process that routes all "
            "traffic through a managed tor daemon's SOCKS5 listener "
            "on a dedicated off-the-record profile.")));
    // Internal development/verification flags.
    const char *const internalOptions[] = {
        "quit-after-load",
        "nam-smoke", "history-smoke", "download-smoke", "cookie-smoke",
        "bookmarks-smoke", "search-smoke", "adblock-smoke",
        "adblock-list-smoke", "adblock-rust-smoke", "autofill-smoke",
        "settings-smoke", "find-smoke", "source-smoke", "browser-smoke",
        "app-smoke", "extension-smoke", "ua-smoke", "perf-smoke",
        "session-smoke", "restore-smoke", "tor-smoke", "privacy-smoke",
        "tor-window-smoke", "sorry-smoke", "icons-smoke", "seclvl-smoke",
        "fingerprint-smoke", "fingerprint-child-smoke",
        "telemetry-smoke", "doh-smoke", "tls-smoke", "tls-off-smoke",
        "webrtc-smoke", "webrtc-off-smoke",
        "profile-startup",
        "browseraudit-smoke", "browseraudit-bare", "anon-smoke",
    };
    for (const char *option : internalOptions)
        parser.addOption(QCommandLineOption(QLatin1String(option)));
    parser.process(application);

    if (!application.isStandalone()) {
        // Normal launch: postLaunch() (queued by the constructor)
        // applies the startup behavior — homepage, last-session
        // restore or the url operand — to this first window.
        application.newMainWindow();
        StartupProfile::mark("newMainWindow returned");
        return application.exec();
    }

    // Standalone development harness: a stub window hosting a WebView
    // on the browsing profile drives --quit-after-load and the smokes.
    const QStringList args = application.arguments();

    // TOR02: `arora --tor` is a standalone process running the real
    // browser UI — every request exits through the managed daemon's
    // SOCKS5 listener (the application proxy is process-global, hence
    // the separate process).  postLaunch() defers the first navigation
    // until the listener is live; --tor-window-smoke takes the headless
    // verification path below instead.
    if (BrowserApplication::isTorMode()
            && !args.contains(QLatin1String("--tor-window-smoke"))) {
        application.newMainWindow();
        return application.exec();
    }

    // PERF03: --profile-startup runs the REAL startup path — real
    // data/settings dirs (no test mode), real BrowserMainWindow,
    // postLaunch's startup navigation and the deferred warmups — with
    // the StartupProfile timeline on stderr, then exits.  Unlike the
    // smokes it deliberately skips the stub WebView below so the
    // engine spins up exactly where it does in a normal launch.
    // Wall-clock "startup-to-window" is the delta to the
    // "first window shown" mark.
    if (args.contains(QLatin1String("--profile-startup"))) {
        application.newMainWindow();
        QTimer::singleShot(6000, &application, [&application]() {
            StartupProfile::mark("settle — exiting");
            application.exit(0);
        });
        return application.exec();
    }

    QWebEngineProfile *profile = BrowserApplication::webEngineProfile();
    CookieJar *cookieJar = CookieJar::instance(profile);
    NetworkAccessManager *networkAccessManager =
        BrowserApplication::networkAccessManager();
    DownloadManager *downloadManager = BrowserApplication::downloadManager();

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Arora"));

    WebView *view = new WebView(profile, &window);
    window.setCentralWidget(view);

    QUrl firstUrl(parser.positionalArguments()
        .value(0, QStringLiteral("about:blank")));
    // SEC09: argv urls are untrusted input — a javascript: operand
    // must not reach loadUrl()'s script execution path.
    if (!WebView::isUrlAllowedOnUntrustedInput(firstUrl)) {
        qWarning() << "Ignoring untrusted argv url:" << firstUrl;
        firstUrl = QUrl(QStringLiteral("about:blank"));
    }
    view->loadUrl(firstUrl);

    // Headless verification for MIG15: exercise the real application
    // path — BrowserApplication brings up the profile and services,
    // opens a BrowserMainWindow and a tab that loads a fixture page,
    // the title propagating to the window.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--app-smoke"))) {
        // Suppress postLaunch()'s startup behavior (goHome / session
        // restore) so the fixture load is the only navigation.
        QSettings().setValue(QLatin1String("MainWindow/startupBehavior"), 1);
        BrowserMainWindow *browserWindow = application.newMainWindow();
        WebView *tab = browserWindow->currentTab();

        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-app-smoke.html"));
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "app-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("<html><head><title>app-smoke-page</title>"
                          "</head><body>app</body></html>");
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        QObject::connect(tab, &QWebEngineView::loadFinished, &application,
                         [&application, tab, fixtureUrl, fixturePath,
                          browserWindow](bool ok) {
            if (!ok || tab->url() != fixtureUrl)
                return;
            const bool pass = browserWindow->windowTitle()
                .contains(QLatin1String("app-smoke-page"));
            qInfo() << "app-smoke:" << (pass ? "PASS" : "FAIL")
                    << tab->url() << browserWindow->windowTitle();
            QFile::remove(fixturePath);
            application.exit(pass ? 0 : 1);
        });
        QTimer::singleShot(15000, &application, [&application]() {
            qInfo() << "app-smoke: FAIL (timeout)";
            application.exit(1);
        });
        browserWindow->tabWidget()->loadUrl(fixtureUrl,
                                            TabWidget::CurrentTab);
    }

    // Headless verification hook: exit once the first page load
    // succeeds so CI can prove WebEngine ran (autotests/smoke style).
    // Failed navigations are skipped so the file:// -> arora-file://
    // directory-listing redirect still counts when it completes.
    if (args.contains(QLatin1String("--quit-after-load"))) {
        QObject::connect(view, &QWebEngineView::loadFinished,
                         &application, [view, &application](bool ok) {
            if (!ok)
                return;
            qInfo() << "loadFinished:" << view->url() << view->title();
            application.exit(0);
        });
        QTimer::singleShot(15000, &application,
                           [&application]() { application.exit(1); });
    }

    // SEC15: live-site measurement — runs the complete browseraudit
    // suite on the browsing profile and records per-test outcomes.
    // --browseraudit-bare uses a clean off-the-record profile/plain
    // view for the engine-only baseline the app run diffs against.
    if (args.contains(QLatin1String("--browseraudit-smoke"))
            || args.contains(QLatin1String("--browseraudit-bare")))
        return browserAuditSmoke(application, view,
            args.contains(QLatin1String("--browseraudit-bare")));

    // ANON01: live-site measurement — drives the ipduh.com privacy
    // test on the browsing profile and records the rendered report
    // plus direct JS probes for .devin/ANON01-report.md.
    if (args.contains(QLatin1String("--anon-smoke")))
        return anonSmoke(application, view);

    window.show();

    // Headless verification for MIG04: the app-side NAM performs a
    // local file:// GET through its proxy factory, disk cache, cookie
    // jar and Accept-Language injection.  Exits 0 on success.
    if (args.contains(QLatin1String("--nam-smoke"))) {
        QNetworkRequest request(QUrl::fromLocalFile(QStringLiteral("/etc/hostname")));
        QNetworkReply *reply = networkAccessManager->get(request);
        QObject::connect(reply, &QNetworkReply::finished, &application,
                         [&application, reply]() {
            qInfo() << "nam-smoke:" << reply->error() << reply->url();
            application.exit(reply->error() == QNetworkReply::NoError ? 0 : 1);
        });
    }

    // Headless verification for MIG06: a successful main-frame load
    // must land in the app-side HistoryManager — WebEngine doesn't push
    // visited urls into the app like QWebHistoryInterface did.  Exits 0
    // when the loaded url is found in history.
    if (args.contains(QLatin1String("--history-smoke"))) {
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, &application](bool ok) {
            if (!ok)
                return;
            const QString urlString = view->url().toString();
            const bool found =
                HistoryManager::instance()->historyContains(urlString);
            qInfo() << "history-smoke:" << (found ? "PASS" : "FAIL") << urlString
                    << "entries:" << HistoryManager::instance()->history().count();
            application.exit(found ? 0 : 1);
        });
        QTimer::singleShot(15000, &application,
                           [&application]() { application.exit(1); });
    }

    // Headless verification for MIG05: issue a real WebEngine download
    // of the given URL.  The DownloadManager picks a file name inside a
    // scratch download dir; exit 0 once the file lands on disk.
    const int downloadSmokeIndex = args.indexOf(QLatin1String("--download-smoke"));
    if (downloadSmokeIndex != -1 && args.count() > downloadSmokeIndex + 1) {
        const QUrl downloadUrl(args.at(downloadSmokeIndex + 1));
        const QString smokeDir =
            QDir::temp().filePath(QLatin1String("arora-download-smoke"));
        QDir().mkpath(smokeDir);
        downloadManager->setDownloadDirectory(smokeDir);
        // A save-as prompt can't be answered under offscreen QPA.
        QSettings().setValue(
            QLatin1String("downloadmanager/alwaysPromptForFileName"), false);
        QObject::connect(profile, &QWebEngineProfile::downloadRequested,
                         &application,
                         [&application](QWebEngineDownloadRequest *request) {
            QObject::connect(request, &QWebEngineDownloadRequest::stateChanged,
                             &application,
                             [&application, request](QWebEngineDownloadRequest::DownloadState state) {
                if (state == QWebEngineDownloadRequest::DownloadCompleted) {
                    const QString path = request->downloadDirectory()
                        + QLatin1Char('/') + request->downloadFileName();
                    const bool ok = QFile::exists(path) && QFileInfo(path).size() > 0;
                    qInfo() << "download-smoke:" << (ok ? "PASS" : "FAIL")
                            << path << request->receivedBytes() << "bytes";
                    application.exit(ok ? 0 : 1);
                } else if (state == QWebEngineDownloadRequest::DownloadInterrupted
                           || state == QWebEngineDownloadRequest::DownloadCancelled) {
                    qInfo() << "download-smoke: FAIL"
                            << request->interruptReasonString();
                    application.exit(1);
                }
            });
        });
        QTimer::singleShot(30000, &application,
                           [&application]() { application.exit(1); });
        view->webPage()->download(downloadUrl);
    }

    // Headless verification for MIG03: push a cookie through the jar's
    // app-side API, verify the store mirror picks it up and that a
    // blocked-domain cookie is rejected. Exits 0 on PASS.
    if (args.contains(QLatin1String("--cookie-smoke"))) {
        QTimer::singleShot(0, &application, [cookieJar]() {
            QNetworkCookie allowed("arora_smoke", "1");
            allowed.setDomain(QLatin1String("example.com"));
            cookieJar->setCookiesFromUrl(QList<QNetworkCookie>() << allowed,
                                         QUrl(QLatin1String("http://example.com/")));

            cookieJar->setBlockedCookies(QStringList() << QLatin1String("blocked.example"));
            QNetworkCookie blocked("arora_blocked", "1");
            blocked.setDomain(QLatin1String("blocked.example"));
            cookieJar->setCookiesFromUrl(QList<QNetworkCookie>() << blocked,
                                         QUrl(QLatin1String("http://blocked.example/")));
        });
        // The cookie store mirror updates asynchronously via
        // cookieAdded — poll for the allowed cookie instead of
        // checking once at a fixed delay (marginal on slow builds).
        QTimer *cookiePoll = new QTimer(&application);
        auto cookiePollTicks = std::make_shared<int>(0);
        QObject::connect(cookiePoll, &QTimer::timeout, &application,
                         [&application, cookieJar, cookiePoll,
                          cookiePollTicks]() {
            const QList<QNetworkCookie> allowed =
                cookieJar->cookiesForUrl(QUrl(QLatin1String("http://example.com/")));
            const QList<QNetworkCookie> blocked =
                cookieJar->cookiesForUrl(QUrl(QLatin1String("http://blocked.example/")));
            bool found = false;
            for (const QNetworkCookie &cookie : allowed)
                found |= (cookie.name() == "arora_smoke");
            const bool pass = blocked.isEmpty() && found;
            if (!pass && ++*cookiePollTicks <= 20)
                return; // retry for ~10s
            qInfo() << "cookie-smoke:" << (pass ? "PASS" : "FAIL")
                    << "(allowed:" << allowed.count() << "blocked:" << blocked.count() << ")";
            cookiePoll->stop();
            // leave no test residue in the saved exception list
            cookieJar->setBlockedCookies(QStringList());
            application.exit(pass ? 0 : 1);
        });
        cookiePoll->start(500);
    }

    // Headless verification for PRIV01: a loopback e2e over two
    // 127.0.0.x hosts (distinct sites to Chromium, both exempt from
    // the https-first upgrade).  The first-party page on 127.0.0.1
    // must load un-upgraded (a redirect to https would fail — nothing
    // serves TLS), embeds an <img> on 127.0.0.2 whose request must
    // arrive with its Referer trimmed to the *target's* origin, and
    // its Set-Cookie must be rejected as third-party while the
    // first-party Set-Cookie lands.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--privacy-smoke"))) {
        QTcpServer *server = new QTcpServer(&application);
        // AnyIPv4 so both 127.0.0.1 and 127.0.0.2 reach the listener.
        if (!server->listen(QHostAddress::AnyIPv4)) {
            qInfo() << "privacy-smoke: FAIL (listen)" << server->errorString();
            return 1;
        }
        const quint16 port = server->serverPort();
        auto observed = std::make_shared<QHash<QString, QByteArray>>();
        QObject::connect(server, &QTcpServer::newConnection, &application,
                         [server, observed, port]() {
            QTcpSocket *client = server->nextPendingConnection();
            QObject::connect(client, &QTcpSocket::readyRead, client,
                             [client, observed, port]() {
                const QByteArray request = client->readAll();
                const int splitAt = request.indexOf("\r\n\r\n");
                const QByteArray head = splitAt == -1 ? request
                    : request.left(splitAt);
                const QList<QByteArray> lines = head.split('\n');
                QString path;
                for (const QByteArray &line : lines) {
                    if (line.startsWith("GET ")) {
                        path = QString::fromLatin1(
                            line.mid(4, line.indexOf(" HTTP/") - 4).trimmed());
                    } else if (line.startsWith("Referer: ")) {
                        (*observed)[QLatin1String("referer:") + path] =
                            line.mid(9).trimmed();
                    }
                }
                (*observed)[QLatin1String("path:") + path] = "1";
                QByteArray body;
                QByteArray extra;
                if (path == QLatin1String("/page")) {
                    extra = "Set-Cookie: arora_priv_first=1\r\n";
                    body = "<html><body><img src=\"http://127.0.0.2:"
                        + QByteArray::number(port) + "/img\"></body></html>";
                } else {
                    extra = "Set-Cookie: arora_priv_third=1\r\n";
                    body = "GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00"
                           "\x00\x00\x00!\xf9\x04\x00\x00\x00\x00\x00,\x00"
                           "\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01"
                           "\x00;";
                }
                QByteArray reply =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                    + extra
                    + "Content-Length: " + QByteArray::number(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body;
                client->write(reply);
                client->disconnectFromHost();
            });
        });

        // Force permissive accept policy so the result isolates the
        // PRIV01 third-party gate, not the navigation-domain policy.
        cookieJar->setAcceptPolicy(CookieJar::AcceptAlways);
        cookieJar->setBlockThirdPartyCookies(true);

        QTimer *poll = new QTimer(&application);
        auto ticks = std::make_shared<int>(0);
        const QUrl pageUrl(QStringLiteral("http://127.0.0.1:%1/page").arg(port));
        const QUrl thirdUrl(QStringLiteral("http://127.0.0.2:%1/").arg(port));
        QObject::connect(poll, &QTimer::timeout, &application,
                         [&application, cookieJar, poll, ticks, observed,
                          thirdUrl]() {
            const QByteArray imgReferer =
                observed->value(QLatin1String("referer:/img"));
            const QByteArray navReferer =
                observed->value(QLatin1String("referer:/nav"));
            const bool sawImg =
                observed->contains(QLatin1String("path:/img"));
            bool first = false;
            for (const QNetworkCookie &c :
                 cookieJar->cookiesForUrl(QUrl(QLatin1String("http://127.0.0.1/"))))
                first |= (c.name() == "arora_priv_first");
            bool third = false;
            for (const QNetworkCookie &c : cookieJar->cookiesForUrl(thirdUrl))
                third |= (c.name() == "arora_priv_third");
            const bool done = sawImg && !navReferer.isEmpty() && first;
            if (!done && ++*ticks <= 40)
                return; // retry for ~20s
            // Navigation referer carries the *target* origin only —
            // the referring host 127.0.0.1 must not appear in it.
            // (Subresource referers are Chromium's own policy — the
            // interceptor header write is ignored there; printed for
            // transparency, not gated.)
            const bool navTrimmed = navReferer.startsWith("http://127.0.0.2:")
                && !navReferer.contains("127.0.0.1");
            const bool pass = sawImg && navTrimmed && first && !third;
            qInfo() << "privacy-smoke:" << (pass ? "PASS" : "FAIL")
                    << "imgReferer:" << imgReferer
                    << "navReferer:" << navReferer << "navTrimmed:" << navTrimmed
                    << "firstPartyCookie:" << first
                    << "thirdPartyCookie:" << third;
            poll->stop();
            application.exit(pass ? 0 : 1);
        });
        poll->start(500);
        view->loadUrl(pageUrl);
        // Second probe: a renderer-initiated cross-site navigation —
        // does setHttpHeader("Referer") take effect there?
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, port](bool ok) {
            if (!ok)
                return;
            if (view->url().path() == QLatin1String("/page"))
                view->webPage()->runJavaScript(QStringLiteral(
                    "location.href='http://127.0.0.2:%1/nav'").arg(port));
        });
    }

    // SECLVL e2e: the Mullvad-style security tiers against a local
    // http fixture.  A non-loopback LAN IPv4 literal is the insecure
    // origin — private-net literals are not https-first upgrade
    // candidates, so the script drop (not the upgrade) is what shows;
    // loopback exercises the secure-context exemption.  Phases:
    //   0 Standard  — external script on plain http fetches + runs
    //   1 Safer     — the same fetch never reaches the server
    //   2 Safer     — loopback http page keeps its (inline) JS
    //   3 Safest    — JavascriptEnabled off: inline JS cannot run
    //   4 Standard  — toggling back restores script execution
    if (args.contains(QLatin1String("--seclvl-smoke"))) {
        QTcpServer *server = new QTcpServer(&application);
        if (!server->listen(QHostAddress::AnyIPv4)) {
            qInfo() << "seclvl-smoke: FAIL (listen)" << server->errorString();
            return 1;
        }
        const quint16 port = server->serverPort();
        auto observed = std::make_shared<QSet<QString>>();
        QObject::connect(server, &QTcpServer::newConnection, &application,
                         [server, observed]() {
            QTcpSocket *client = server->nextPendingConnection();
            QObject::connect(client, &QTcpSocket::readyRead, client,
                             [client, observed]() {
                const QByteArray request = client->readAll();
                QString path;
                for (const QByteArray &line : request.split('\n')) {
                    if (line.startsWith("GET ")) {
                        path = QString::fromLatin1(
                            line.mid(4, line.indexOf(" HTTP/") - 4).trimmed());
                    }
                }
                if (path.isEmpty()) {
                    client->disconnectFromHost();
                    return;
                }
                observed->insert(path);
                QByteArray body;
                QByteArray type = "text/html";
                if (path == QLatin1String("/page")) {
                    body = "<html><head><title>p1-title</title>"
                           "<script src=\"/s.js\"></script></head>"
                           "<body>1</body></html>";
                } else if (path == QLatin1String("/s.js")) {
                    type = "text/javascript";
                    body = "document.title='ext-ran';";
                } else if (path == QLatin1String("/page2")) {
                    body = "<html><head><title>p2-title</title>"
                           "<script src=\"/s2.js\"></script></head>"
                           "<body>2</body></html>";
                } else if (path == QLatin1String("/s2.js")) {
                    type = "text/javascript";
                    body = "document.title='ext2-ran';";
                } else if (path == QLatin1String("/inline")) {
                    body = "<html><head><title>nojs</title></head><body>"
                           "<script>document.title='inline-ran';</script>"
                           "</body></html>";
                }
                client->write("HTTP/1.1 200 OK\r\nContent-Type: " + type
                    + "\r\nContent-Length: "
                    + QByteArray::number(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body);
                client->disconnectFromHost();
            });
        });

        // An http origin that is NOT a secure context: any
        // non-loopback IPv4 literal this machine owns.  Without one
        // the Safer-drop phase cannot be exercised honestly.
        QString lan;
        for (const QHostAddress &address : QNetworkInterface::allAddresses()) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol
                && !address.isLoopback() && !address.isMulticast()) {
                lan = address.toString();
                break;
            }
        }
        if (lan.isEmpty()) {
            qInfo() << "seclvl-smoke: FAIL (no non-loopback IPv4)";
            return 1;
        }

        QSettings settings;
        const QVariant savedLevel =
            settings.value(QLatin1String("privacy/securityLevel"));
        auto applyLevel = [profile](int level) {
            QSettings().setValue(QLatin1String("privacy/securityLevel"), level);
            PrivacyRequestInterceptor::loadSettings();
            BrowserProfile::applySettings(profile);
        };

        auto phase = std::make_shared<int>(0);
        auto loaded = std::make_shared<bool>(false);
        auto settle = std::make_shared<int>(0);
        auto ticks = std::make_shared<int>(0);
        auto wantPath = std::make_shared<QString>(QStringLiteral("/page"));
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, loaded, wantPath](bool ok) {
            if (ok && view->url().path() == *wantPath)
                *loaded = true;
        });

        QTimer *poll = new QTimer(&application);
        QObject::connect(poll, &QTimer::timeout, &application,
                         [&application, view, poll, profile, phase,
                          loaded, settle, ticks, wantPath, observed,
                          savedLevel, applyLevel, lan, port]() {
            auto finish = [&](bool ok, const QString &why) {
                if (savedLevel.isValid())
                    QSettings().setValue(QLatin1String("privacy/securityLevel"),
                                         savedLevel);
                else
                    QSettings().remove(QLatin1String("privacy/securityLevel"));
                PrivacyRequestInterceptor::loadSettings();
                BrowserProfile::applySettings(profile);
                qInfo() << "seclvl-smoke:" << (ok ? "PASS" : "FAIL") << why;
                poll->stop();
                application.exit(ok ? 0 : 1);
            };
            if (++*ticks > 300) {
                finish(false, QStringLiteral("timeout phase %1").arg(*phase));
                return;
            }
            QWebEngineSettings *engineSettings = profile->settings();
            switch (*phase) {
            case 0:  // Standard baseline — script fetched and ran.
                if (!*loaded || view->title() != QLatin1String("ext-ran"))
                    return;
                if (!observed->contains(QLatin1String("/s.js"))) {
                    finish(false, QStringLiteral("standard: /s.js not fetched"));
                    return;
                }
                *phase = 1;
                *loaded = false;
                *wantPath = QStringLiteral("/page2");
                applyLevel(PrivacyRequestInterceptor::Safer);
                view->loadUrl(QStringLiteral("http://%1:%2/page2")
                              .arg(lan).arg(port));
                return;
            case 1:  // Safer — the script fetch must never arrive.
                if (!*loaded)
                    return;
                if (++*settle < 4)   // give a would-be fetch ~800ms
                    return;
                if (observed->contains(QLatin1String("/s2.js"))
                    || view->title() == QLatin1String("ext2-ran")) {
                    finish(false, QStringLiteral("safer: http script ran"));
                    return;
                }
                if (!engineSettings->testAttribute(
                        QWebEngineSettings::PlaybackRequiresUserGesture)) {
                    finish(false, QStringLiteral("safer: autoplay not gated"));
                    return;
                }
                *phase = 2;
                *loaded = false;
                *settle = 0;
                *wantPath = QStringLiteral("/inline");
                view->loadUrl(QStringLiteral("http://127.0.0.1:%1/inline")
                              .arg(port));
                return;
            case 2:  // Safer — loopback is a secure context: JS stays.
                if (!*loaded || view->title() != QLatin1String("inline-ran"))
                    return;
                *phase = 3;
                *loaded = false;
                applyLevel(PrivacyRequestInterceptor::Safest);
                if (engineSettings->testAttribute(
                        QWebEngineSettings::JavascriptEnabled)) {
                    finish(false, QStringLiteral("safest: JS still on"));
                    return;
                }
                view->loadUrl(QStringLiteral("http://127.0.0.1:%1/inline")
                              .arg(port));
                return;
            case 3:  // Safest — inline JS cannot run either.
                if (!*loaded)
                    return;
                if (++*settle < 4)
                    return;
                if (view->title() != QLatin1String("nojs")) {
                    finish(false, QStringLiteral("safest: inline JS ran"));
                    return;
                }
                *phase = 4;
                *loaded = false;
                *settle = 0;
                applyLevel(PrivacyRequestInterceptor::Standard);
                view->loadUrl(QStringLiteral("http://127.0.0.1:%1/inline")
                              .arg(port));
                return;
            case 4:  // Toggling back down restores scripts.
                if (!*loaded || view->title() != QLatin1String("inline-ran"))
                    return;
                if (!engineSettings->testAttribute(
                        QWebEngineSettings::JavascriptEnabled)) {
                    finish(false, QStringLiteral("restore: JS still off"));
                    return;
                }
                finish(true, QStringLiteral(
                    "standard fetch ok, safer dropped http script, "
                    "loopback exempt, safest scriptless, restored"));
                return;
            }
        });
        poll->start(200);
        applyLevel(PrivacyRequestInterceptor::Standard);
        view->loadUrl(QStringLiteral("http://%1:%2/page").arg(lan).arg(port));
    }

    // Headless verification for DOH01: custom DNS-over-HTTPS wiring
    // end-to-end through the real QSettings -> applySecureDns ->
    // QWebEngineGlobalSettings::setDnsMode path.  The run was seeded
    // pre-app with strict mode + a dead loopback endpoint, so the
    // engine latched the DnsOverHttps feature switch exactly like a
    // configured profile.
    //   stage 0 — strict + unreachable endpoint: EVERY lookup must
    //     fail.  Fully local and offline-proof; a page that loads
    //     here means setDnsMode was silently ignored and plain system
    //     DNS went through.
    //   stage 1 — strict + a live resolver (Cloudflare's 1.1.1.1 as
    //     an IP literal so no bootstrap lookup is needed): a https
    //     page must load — under SecureOnly the resolution could only
    //     have come from DoH.  Needs real outbound connectivity; a
    //     dead control GET downgrades a failure here to a SKIP.
    //   stage 2 — mode back to Off: a further https load must succeed,
    //     proving the mode toggles down at runtime.
    if (args.contains(QLatin1String("--doh-smoke"))) {
        auto stage = std::make_shared<int>(0);
        auto done = std::make_shared<bool>(false);
        auto controlOk = std::make_shared<bool>(false);

        const auto setDoh = [](int mode, const QString &server) {
            QSettings settings;
            settings.beginGroup(QLatin1String("privacy"));
            settings.setValue(QLatin1String("secureDnsMode"), mode);
            settings.setValue(QLatin1String("secureDnsServer"), server);
            settings.endGroup();
            BrowserProfile::applySecureDns();
        };

        // Engine-free connectivity control — Qt's NAM does not route
        // through Chromium's resolver, so this GET to the resolver
        // host proves outbound https is up regardless of DoH state.
        QNetworkReply *controlReply = networkAccessManager->get(
            QNetworkRequest(QUrl(QLatin1String("https://1.1.1.1/"))));
        QObject::connect(controlReply, &QNetworkReply::finished,
                         &application, [controlReply, controlOk]() {
            *controlOk = (controlReply->error() == QNetworkReply::NoError);
            controlReply->deleteLater();
        });

        const auto finish = [&application, done, &savedDohMode,
                             &savedDohServer](int rc, const QString &line) {
            if (*done)
                return;
            *done = true;
            qInfo().noquote() << "doh-smoke:" << line;
            // Put the caller's own settings back before the store
            // syncs at exit.
            QSettings settings;
            settings.beginGroup(QLatin1String("privacy"));
            const auto restore = [&settings](const QString &key,
                                             const QVariant &saved) {
                if (saved.isValid())
                    settings.setValue(key, saved);
                else
                    settings.remove(key);
            };
            restore(QLatin1String("secureDnsMode"), savedDohMode);
            restore(QLatin1String("secureDnsServer"), savedDohServer);
            settings.endGroup();
            application.exit(rc);
        };

        // Per-stage verdicts read the FIRST terminal status for that
        // stage's host — the injected notfound page may emit a second
        // Succeeded for the failed url afterwards.
        QObject::connect(view->webPage(), &QWebEnginePage::loadingChanged,
            &application,
            [view, stage, finish, setDoh, controlOk]
            (const QWebEngineLoadingInfo &info) {
            if (info.status() != QWebEngineLoadingInfo::LoadSucceededStatus
                && info.status() != QWebEngineLoadingInfo::LoadFailedStatus)
                return;
            const QString host = info.url().host();
            switch (*stage) {
            case 0:
                if (host != QLatin1String("qt.io"))
                    return;   // stale event (about:blank, error page)
                if (info.status()
                        == QWebEngineLoadingInfo::LoadSucceededStatus) {
                    finish(1, QStringLiteral(
                        "FAIL stage0: strict + dead endpoint still resolved")
                        + host);
                    return;
                }
                qInfo() << "doh-smoke: stage0 ok — dead endpoint failed"
                           " resolution:" << info.errorString();
                *stage = 1;
                setDoh(3, QLatin1String("https://1.1.1.1/dns-query"));
                view->loadUrl(QUrl(QLatin1String("https://example.com/")));
                return;
            case 1:
                if (!host.endsWith(QLatin1String("example.com")))
                    return;
                if (info.status()
                        == QWebEngineLoadingInfo::LoadSucceededStatus) {
                    *stage = 2;
#if defined(Q_OS_LINUX)
                    // Best-effort extra: the resolve-time DoH socket
                    // can idle-close before the page finishes, so a
                    // "false" here is normal and carries no verdict.
                    const QSet<QString> endpoints = processRemoteEndpoints();
                    qInfo() << "doh-smoke: stage1 ok — loaded under strict"
                               " DoH (1.1.1.1 socket still open:"
                            << (endpoints.contains(QLatin1String("1.1.1.1:443"))
                                || endpoints.contains(
                                    QLatin1String("1.0.0.1:443")))
                            << ")";
#else
                    qInfo() << "doh-smoke: stage1 ok — loaded under strict"
                               " DoH";
#endif
                    setDoh(0, QLatin1String("https://1.1.1.1/dns-query"));
                    view->loadUrl(QUrl(QLatin1String("https://www.iana.org/")));
                    return;
                }
                finish(*controlOk ? 1 : 0, QStringLiteral(
                    "%1 stage1: https://example.com failed under DoH — %2"
                    " (control GET %3)")
                        .arg(*controlOk ? QLatin1String("FAIL")
                                        : QLatin1String("SKIP"),
                             info.errorString(),
                             *controlOk ? QLatin1String("ok")
                                        : QLatin1String("dead")));
                return;
            case 2:
                if (!host.endsWith(QLatin1String("iana.org")))
                    return;
                if (info.status()
                        == QWebEngineLoadingInfo::LoadSucceededStatus) {
                    finish(0, QStringLiteral("PASS"));
                    return;
                }
                finish(1, QStringLiteral(
                    "FAIL stage2: system-DNS restore load failed — %1")
                        .arg(info.errorString()));
                return;
            }
        });

        QTimer::singleShot(90000, &application,
                           [stage, finish, controlOk]() {
            if (*stage == 0)
                finish(1, QStringLiteral(
                    "FAIL stage0: dead-endpoint resolution never"
                    " concluded"));
            else
                finish(*controlOk ? 1 : 0, QStringLiteral(
                    "%1: network stage timed out (control GET %2)")
                        .arg(*controlOk ? QLatin1String("FAIL")
                                        : QLatin1String("SKIP"),
                             *controlOk ? QLatin1String("ok")
                                        : QLatin1String("dead")));
        });

        // Stage 0's strict+dead configuration was already applied by
        // the pre-app settings seed and prepareProfile's
        // applySecureDns — navigating is all that is left.
        view->loadUrl(QUrl(QLatin1String("https://qt.io/")));
    }

    // TLS01: --tls-smoke / --tls-off-smoke inspect the cipher suites
    // the engine advertises by capturing the raw ClientHello off a
    // loopback listener — the server never completes the handshake,
    // the navigation fails on purpose and the capture is the verdict.
    // --tls-smoke pins privacy/tlsStrictCiphers on and also attempts a
    // best-effort live https load to prove ordinary sites still
    // negotiate the reduced list: a fast LoadFailed on a live network
    // is the shape a real cipher regression takes and fails the run,
    // while a silent stall is harness/network flake (the engine has
    // been observed wedging navigations in this stub window) and only
    // downgrades the stage to a SKIP — the local capture alone
    // carries the assertion.
    // The capture navigation runs on its OWN WebView/page: the
    // broken handshake's LoadFailed commits an error page that
    // cancels any other in-flight navigation on the same page, so
    // sharing the smoke view could wedge or cancel the capture
    // (observed as LoadStarted-then-silence and teardown crashes
    // during development).  A dedicated view also keeps whatever
    // state the live stage leaves from reaching the capture.
    // --tls-off-smoke pins the setting off and asserts the weak suites
    // ARE advertised — a differential control attributing the
    // stripping to the flag, not to a Chromium default.
    if (tlsSmoke || tlsOffSmoke) {
        QTcpServer *server = new QTcpServer(&application);
        if (!server->listen(QHostAddress::LocalHost)) {
            qInfo() << "tls-smoke: FAIL (listen)" << server->errorString();
            if (savedTlsStrict.isValid())
                QSettings().setValue(
                    QLatin1String("privacy/tlsStrictCiphers"), savedTlsStrict);
            else
                QSettings().remove(
                    QLatin1String("privacy/tlsStrictCiphers"));
            return 1;
        }

        auto done = std::make_shared<bool>(false);
        auto capturing = std::make_shared<bool>(false);
        const auto finish = [&application, done, &savedTlsStrict]
                            (int rc, const QString &line) {
            if (*done)
                return;
            *done = true;
            qInfo().noquote() << "tls-smoke:" << line;
            // Put the caller's own setting back before the store
            // syncs at exit (the pre-app seed pinned it).
            if (savedTlsStrict.isValid())
                QSettings().setValue(
                    QLatin1String("privacy/tlsStrictCiphers"), savedTlsStrict);
            else
                QSettings().remove(
                    QLatin1String("privacy/tlsStrictCiphers"));
            application.exit(rc);
        };

        // Engine-free connectivity control for the real-https stage —
        // Qt's NAM does not use Chromium's TLS stack, so it probes
        // plain network liveness like doh-smoke's control does.
        auto controlOk = std::make_shared<bool>(false);
        QNetworkReply *controlReply = networkAccessManager->get(
            QNetworkRequest(QUrl(QLatin1String("https://example.com/"))));
        QObject::connect(controlReply, &QNetworkReply::finished,
                         &application, [controlReply, controlOk]() {
            *controlOk = (controlReply->error() == QNetworkReply::NoError);
            controlReply->deleteLater();
        });

        const auto verdict = [finish, tlsOffSmoke]
                             (const QList<quint16> &ciphers) {
            QStringList listed;
            for (quint16 cipher : ciphers)
                listed << QStringLiteral("0x%1")
                    .arg(cipher, 4, 16, QLatin1Char('0'));
            qInfo() << "tls-smoke: advertised"
                    << listed.join(QLatin1Char(','));
            static const quint16 weak[] = {
                0x009c, 0x009d, 0x002f, 0x0035, 0xc013, 0xc014 };
            int weakSeen = 0;
            for (quint16 cipher : weak)
                weakSeen += ciphers.contains(cipher) ? 1 : 0;
            if (tlsOffSmoke) {
                finish(weakSeen == 6 ? 0 : 1, QStringLiteral(
                    "%1 (off-mode control: %2/6 weak suites advertised)")
                    .arg(weakSeen == 6 ? QLatin1String("PASS")
                                       : QLatin1String("FAIL"))
                    .arg(weakSeen));
                return;
            }
            const bool strongKept =
                ciphers.contains(quint16(0x1301))
                && (ciphers.contains(quint16(0xc02f))
                    || ciphers.contains(quint16(0xc02b))
                    || ciphers.contains(quint16(0xcca9)));
            if (weakSeen != 0 || !strongKept) {
                finish(1, QStringLiteral(
                    "FAIL (%1 weak suite(s) advertised, strong set %2)")
                    .arg(weakSeen)
                    .arg(strongKept ? QLatin1String("intact")
                                    : QLatin1String("damaged")));
                return;
            }
            finish(0, QStringLiteral(
                "PASS (0 weak suites advertised, TLS 1.3 + ECDHE/AEAD"
                " intact)"));
        };

        QObject::connect(server, &QTcpServer::newConnection,
                         &application, [server, capturing, verdict]() {
            QTcpSocket *client = server->nextPendingConnection();
            // One capture is the whole point of the listener —
            // refusing the port afterwards makes Chromium's
            // connection-error retry fail fast instead of parking the
            // navigation on a second unanswered handshake.
            server->close();
            auto buffer = std::make_shared<QByteArray>();
            auto parsed = std::make_shared<bool>(false);
            const auto tryParse =
                [client, buffer, capturing, parsed, verdict]() {
                if (!*capturing || *parsed)
                    return;
                const QList<quint16> ciphers =
                    tlsClientHelloCiphers(*buffer);
                if (ciphers.isEmpty())
                    return;
                // The handshake is never answered — abort the socket
                // once the ClientHello is captured so the engine's
                // pending navigation fails fast.  The sentinel keeps
                // a re-entrant disconnected() out of the verdict.
                *parsed = true;
                client->abort();
                verdict(ciphers);
            };
            QObject::connect(client, &QTcpSocket::readyRead, client,
                             [client, buffer, tryParse]() {
                buffer->append(client->readAll());
                tryParse();
            });
            QObject::connect(client, &QTcpSocket::disconnected, client,
                             [client, tryParse]() {
                tryParse();
                client->deleteLater();
            });
        });

        const auto startCapture = [&window, profile, server,
                                   capturing]() {
            if (*capturing)
                return;
            *capturing = true;
            // Hidden child view — the engine only needs its network
            // stack for the ClientHello, and a separate page isolates
            // the deliberately-failed navigation's error-page commit
            // from whatever load the smoke view is still carrying.
            auto *captureView = new WebView(profile, &window);
            captureView->loadUrl(QUrl(QStringLiteral("https://127.0.0.1:%1/")
                .arg(server->serverPort())));
        };
        // Deferred so a just-failed live navigation's error-page
        // commit can settle before the capture view is created.
        const auto queueCapture = [&application, startCapture]() {
            QTimer::singleShot(250, &application, startCapture);
        };

        if (tlsSmoke) {
            // Live https under the reduced list — best-effort per the
            // block comment; the capture below is the actual verdict.
            // Like the capture, this runs on its own view created
            // up front: a SECOND real navigation on the shared stub
            // view reliably wedges in this harness (LoadStarted then
            // silence) while a view's first navigation does not.
            auto *liveView = new WebView(profile, &window);
            const auto onLoadingChanged =
                [finish, controlOk, queueCapture]
                (const QWebEngineLoadingInfo &info) {
                if (info.status() != QWebEngineLoadingInfo::LoadSucceededStatus
                    && info.status() != QWebEngineLoadingInfo::LoadFailedStatus)
                    return;
                if (!info.url().host().endsWith(QLatin1String("example.com")))
                    return;
                if (info.status() == QWebEngineLoadingInfo::LoadFailedStatus
                    && *controlOk) {
                    finish(1, QStringLiteral(
                        "FAIL: https://example.com failed under strict"
                        " ciphers — %1 (control GET ok)")
                        .arg(info.errorString()));
                    return;
                }
                qInfo() << "tls-smoke: external https"
                        << (info.status()
                                == QWebEngineLoadingInfo::LoadSucceededStatus
                            ? "ok" : "skipped (network dead)")
                        << "— proceeding to local capture";
                queueCapture();
            };
            QObject::connect(liveView->webPage(),
                             &QWebEnginePage::loadingChanged,
                             &application, onLoadingChanged);
            liveView->loadUrl(QUrl(QLatin1String("https://example.com/")));
        }

        // The live stage gets 40s, then the rest of the run belongs to
        // the capture.  A stalled live load is not a cipher verdict.
        QTimer::singleShot(40000, &application,
                           [done, capturing, controlOk, startCapture]() {
            if (*done || *capturing)
                return;
            qInfo() << "tls-smoke: external https stalled"
                    << (*controlOk ? "(control GET ok — infra flake,"
                                     " not a cipher verdict)"
                                   : "(control GET dead — offline)")
                    << "— proceeding to local capture";
            startCapture();
        });
        QTimer::singleShot(90000, &application, [done, finish]() {
            finish(1, QStringLiteral(
                "FAIL (timeout: no ClientHello captured)"));
        });

        if (tlsOffSmoke)
            // No live-network stage — the capture alone is the
            // differential control.
            QTimer::singleShot(0, &application, startCapture);
    }

    // LEAK01: --webrtc-smoke proves the WebRTC IP-handling switch
    // actually latched — the user's leak report.  The flag lands in
    // QTWEBENGINE_CHROMIUM_FLAGS before the BrowserApplication
    // constructor runs, but Chromium reads that variable once when its
    // context spins up and ignores it afterwards, so env-presence
    // alone is not the verdict: a real RTCPeerConnection's ICE
    // candidate enumeration must surface NO IP literal while the
    // policy is armed (disable_non_proxied_udp + no proxy leaves no
    // UDP transport at all).  --webrtc-off-smoke is the differential
    // control: unprotected gathering must still produce candidates,
    // otherwise a "no leak" on-mode result proves nothing.  An
    // mdns *.local candidate is counted separately — it is the
    // deliberate obfuscation shape, not a raw-IP leak.
    if (webrtcSmoke || webrtcOffSmoke) {
        QTcpServer *server = new QTcpServer(&application);
        if (!server->listen(QHostAddress::LocalHost)) {
            qInfo() << "webrtc-smoke: FAIL (listen)"
                    << server->errorString();
            if (savedWebrtcProtection.isValid())
                QSettings().setValue(
                    QLatin1String("privacy/webrtcIpProtection"),
                    savedWebrtcProtection);
            else
                QSettings().remove(
                    QLatin1String("privacy/webrtcIpProtection"));
            return 1;
        }
        static const QByteArray rtcPage = QByteArray(
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<!doctype html><html><body><script>"
            "window.__rtc={cands:[],done:false,err:null};"
            "try{"
            "var pc=new RTCPeerConnection({iceServers:[]});"
            "pc.onicecandidate=function(e){"
            "if(e.candidate&&e.candidate.candidate)"
            "window.__rtc.cands.push(e.candidate.candidate);"
            "else window.__rtc.done=true;};"
            "pc.onicegatheringstatechange=function(){"
            "if(pc.iceGatheringState==='complete')"
            "window.__rtc.done=true;};"
            "pc.createDataChannel('x');"
            "pc.createOffer().then(function(o){"
            "return pc.setLocalDescription(o);}).catch(function(e){"
            "window.__rtc.err=String(e);window.__rtc.done=true;});"
            "window.setTimeout(function(){window.__rtc.done=true;},9000);"
            "}catch(e){window.__rtc.err=String(e);"
            "window.__rtc.done=true;}"
            "</script></body></html>");
        QObject::connect(server, &QTcpServer::newConnection, &application,
                         [server]() {
            QTcpSocket *client = server->nextPendingConnection();
            // Consume the request before replying — closing with
            // unread bytes in the receive queue turns into a TCP RST
            // (ERR_CONNECTION_RESET) instead of a clean close.
            QObject::connect(client, &QTcpSocket::readyRead, client,
                             [client]() {
                client->readAll();
                client->write(rtcPage);
                client->disconnectFromHost();
            });
        });

        auto done = std::make_shared<bool>(false);
        const auto finish = [&application, done, &savedWebrtcProtection]
                            (int rc, const QString &line) {
            if (*done)
                return;
            *done = true;
            qInfo().noquote() << "webrtc-smoke:" << line;
            // Put the caller's own setting back before the store
            // syncs at exit (the pre-app seed pinned it).
            if (savedWebrtcProtection.isValid())
                QSettings().setValue(
                    QLatin1String("privacy/webrtcIpProtection"),
                    savedWebrtcProtection);
            else
                QSettings().remove(
                    QLatin1String("privacy/webrtcIpProtection"));
            application.exit(rc);
        };

        const bool flagInEnv = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS")
            .contains("force-webrtc-ip-handling-policy="
                      "disable_non_proxied_udp");
        if (webrtcSmoke && !flagInEnv) {
            finish(1, QStringLiteral(
                "FAIL (policy enabled but the engine flag never"
                " reached QTWEBENGINE_CHROMIUM_FLAGS)"));
        }
        if (webrtcOffSmoke && flagInEnv) {
            finish(1, QStringLiteral(
                "FAIL (policy disabled but the engine flag is still"
                " advertised)"));
        }

        // Dedicated probe view — the shared stub view may still be
        // carrying the firstUrl navigation (tls-smoke hit the same
        // wedge and isolates its capture view for that reason).  It
        // must be shown: a hidden WebContents is backgrounded, which
        // throttles the in-page settle timer and can stall the whole
        // gather (the same reason COV03's findText test shows its
        // view under offscreen).
        auto *rtcView = new WebView(profile, &window);
        rtcView->show();

        const auto verdict = [finish, webrtcOffSmoke]
                             (const QStringList &cands,
                              const QString &err) {
            if (!err.isEmpty()) {
                finish(1, QStringLiteral(
                    "FAIL (RTCPeerConnection error: %1)").arg(err));
                return;
            }
            int ipCount = 0, mdnsCount = 0;
            for (const QString &cand : cands) {
                // candidate:<f> <comp> <tp> <prio> <addr> <port> typ ..
                const QString addr =
                    cand.split(QLatin1Char(' '), Qt::SkipEmptyParts)
                        .value(4);
                if (QHostAddress(addr).isNull()) {
                    if (addr.endsWith(QLatin1String(".local")))
                        ++mdnsCount;
                } else {
                    ++ipCount;
                    qInfo() << "webrtc-smoke: candidate with IP"
                            << addr;
                }
            }
            if (webrtcOffSmoke) {
                // Control: gathering must work unprotected — raw IPs
                // are the expected leak shape, mdns names still count
                // as real candidates.
                if (cands.count() >= 1) {
                    finish(0, QStringLiteral(
                        "PASS (off-mode control: %1 candidates,"
                        " %2 raw-IP, %3 mdns — leak reproduced)")
                        .arg(cands.count()).arg(ipCount).arg(mdnsCount));
                } else {
                    finish(1, QStringLiteral(
                        "FAIL (off-mode control: 0 candidates — probe"
                        " cannot discriminate)"));
                }
                return;
            }
            if (ipCount > 0) {
                finish(1, QStringLiteral(
                    "FAIL (%1 IP-literal candidate(s) leaked despite"
                    " disable_non_proxied_udp)")
                    .arg(ipCount));
            } else {
                finish(0, QStringLiteral(
                    "PASS (0 IP-literal candidates under"
                    " disable_non_proxied_udp; %1 mdns-obfuscated,"
                    " %2 total candidates)")
                    .arg(mdnsCount).arg(cands.count()));
            }
        };

        QTimer *poll = new QTimer(&application);
        QObject::connect(poll, &QTimer::timeout, &application,
                         [rtcView, poll, verdict]() {
            rtcView->webPage()->runJavaScript(
                QStringLiteral("JSON.stringify(window.__rtc||null)"),
                [poll, verdict](const QVariant &result) {
                const QJsonObject rtc = QJsonDocument::fromJson(
                    result.toString().toUtf8()).object();
                if (rtc.isEmpty())
                    return;   // probe page not up yet
                const QStringList cands =
                    rtc.value(QLatin1String("cands")).toVariant()
                        .toStringList();
                const QString err =
                    rtc.value(QLatin1String("err")).toString();
                if (rtc.value(QLatin1String("done")).toBool()
                    || !err.isEmpty()) {
                    poll->stop();
                    verdict(cands, err);
                }
            });
        });
        QObject::connect(rtcView->webPage(),
                         &QWebEnginePage::loadingChanged, &application,
                         [poll, finish]
                         (const QWebEngineLoadingInfo &info) {
            if (info.status() != QWebEngineLoadingInfo::LoadFailedStatus)
                return;
            poll->stop();
            finish(1, QStringLiteral(
                "FAIL (probe page load failed: %1 %2)")
                .arg(info.errorString()).arg(info.errorDomain()));
        });
        rtcView->loadUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/")
            .arg(server->serverPort())));
        poll->start(500);
        QTimer::singleShot(45000, &application, [done, finish]() {
            finish(1, QStringLiteral(
                "FAIL (timeout: ICE gathering never settled)"));
        });
    }

    // Headless verification for MIG07: exercise the app-wide bookmarks
    // store — load (falls back to the bundled default XBEL), add /
    // rename / remove a bookmark through the undo-stack API while the
    // model watches, plus an XBEL round-trip and &nbsp; entity
    // expansion (Qt6 dropped QXmlStreamEntityResolver).  Exits 0 on PASS.
    if (args.contains(QLatin1String("--bookmarks-smoke"))) {
        QTimer::singleShot(0, &application, [&application]() {
            BookmarksManager *manager = BookmarksManager::instance();
            BookmarksModel *model = manager->bookmarksModel();
            BookmarkNode *menu = manager->menu();
            const int before = menu->children().count();

            BookmarkNode *node = new BookmarkNode(BookmarkNode::Bookmark);
            node->title = QStringLiteral("smoke");
            node->url = QStringLiteral("http://example.com/");
            manager->addBookmark(menu, node);
            const bool added = menu->children().count() == before + 1
                && model->data(model->index(node), Qt::DisplayRole)
                       .toString() == QLatin1String("smoke");
            manager->setTitle(node, QStringLiteral("smoke2"));
            const bool renamed = node->title == QLatin1String("smoke2");
            manager->removeBookmark(node);
            const bool removed = menu->children().count() == before;

            // XBEL round-trip through a temp file
            const QString tmpFile = QDir::temp().filePath(
                QLatin1String("arora-bookmarks-smoke.xbel"));
            XbelWriter writer;
            const bool wrote = writer.write(tmpFile, manager->bookmarks());
            XbelReader reader;
            BookmarkNode *copy = reader.read(tmpFile);
            const bool roundtrip = wrote
                && reader.error() == QXmlStreamReader::NoError
                && copy->children().count()
                       == manager->bookmarks()->children().count();
            delete copy;
            QFile::remove(tmpFile);

            // &nbsp; expansion (the pre-Qt6 entity resolver's job);
            // it expands to a plain space, matching the Qt4 resolver and
            // the autotests/xbel/all.xbel fixture expectation.
            QByteArray xbel =
                "<xbel><folder folded=\"no\"><title>a&nbsp;b</title>"
                "</folder></xbel>";
            QBuffer buffer(&xbel);
            buffer.open(QIODevice::ReadOnly);
            XbelReader entityReader;
            BookmarkNode *entityRoot = entityReader.read(&buffer);
            const bool entity = entityReader.error() == QXmlStreamReader::NoError
                && entityRoot->children().count() == 1
                && entityRoot->children().first()->title
                       == QLatin1String("a b");
            delete entityRoot;

            const bool ok = added && renamed && removed && roundtrip && entity;
            qInfo() << "bookmarks-smoke:" << (ok ? "PASS" : "FAIL")
                    << "(added:" << added << "renamed:" << renamed
                    << "removed:" << removed << "xbel:" << roundtrip
                    << "entity:" << entity << ")";
            application.exit(ok ? 0 : 1);
        });
    }

    // Headless verification for MIG08: the bundled OpenSearch
    // descriptions load from the resource, template substitution
    // produces a valid search url, the XML round-trips through
    // writer+reader, keyword search resolves, and a suggestion query
    // against a local file:// reply is parsed via QJsonDocument (the
    // QtScript eval path is gone).  Exits 0 on PASS.
    if (args.contains(QLatin1String("--search-smoke"))) {
        OpenSearchManager *manager = ToolbarSearch::openSearchManager();
        OpenSearchEngine *google = manager->engine(QLatin1String("Google"));
        OpenSearchEngine *ddg = manager->engine(QLatin1String("DuckDuckGo"));
        bool ok = manager->enginesCount() >= 7 && google && google->isValid()
                && ddg && ddg->isValid() && manager->currentEngine();

        // Fresh profiles default to DuckDuckGo; a saved choice is
        // always respected (a stale saved name falls back to the first
        // available engine).
        QSettings engineSettings;
        const QVariant savedEngine = engineSettings.value(
            QLatin1String("openSearch/engine"));
        ok = ok && (savedEngine.isValid()
                        ? manager->currentEngineName()
                                    == savedEngine.toString()
                              || !manager->engineExists(
                                      savedEngine.toString())
                        : manager->currentEngineName()
                                  == QLatin1String("DuckDuckGo"));
        if (google) {
            const QUrl url = google->searchUrl(QLatin1String("hello world"));
            ok = ok && url.isValid()
                 && QString::fromUtf8(url.toEncoded())
                        .contains(QLatin1String("hello%20world"));
        }

        // Writer + reader round-trip of a bundled engine.
        QByteArray xml;
        QBuffer writeBuffer(&xml);
        writeBuffer.open(QIODevice::WriteOnly);
        OpenSearchWriter writer;
        bool wrote = google && writer.write(&writeBuffer, google);
        writeBuffer.close();
        QBuffer readBuffer(&xml);
        OpenSearchReader reader;
        OpenSearchEngine *copy = reader.read(&readBuffer);
        ok = ok && wrote && reader.error() == QXmlStreamReader::NoError
             && copy->isValid()
             && copy->name() == google->name()
             && copy->searchUrlTemplate() == google->searchUrlTemplate();
        delete copy;

        // Location-bar keyword search ("g terms" -> engine search url).
        manager->setEngineForKeyword(QLatin1String("g"), google);
        ok = ok && manager->convertKeywordSearchToUrl(
                QLatin1String("g arora")).isValid();

        // The widget side constructs offscreen.
        LocationBar locationBar(&window);
        locationBar.setWebView(view);
        ToolbarSearch toolbarSearch(&window);
        toolbarSearch.setWebView(view);
        ok = ok && toolbarSearch.openSearchManager() == manager;

        if (!ok) {
            qInfo() << "search-smoke: FAIL (engines:" << manager->enginesCount() << ")";
            return 1;
        } else {
            // Suggestions: a throwaway engine pointed at a local file
            // reply exercises the JSON suggestion parser end-to-end.
            const QString fixturePath = QDir::temp().filePath(
                QLatin1String("arora-suggest-smoke.json"));
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "search-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("[\"arora\",[\"arora browser\",\"arora git\"]]");
            fixture.close();

            OpenSearchEngine *suggest = new OpenSearchEngine(&application);
            suggest->setName(QLatin1String("smoke"));
            suggest->setSuggestionsUrlTemplate(
                QLatin1String("file://") + fixturePath
                + QLatin1String("?q={searchTerms}"));
            suggest->setNetworkAccessManager(networkAccessManager);
            QObject::connect(suggest, &OpenSearchEngine::suggestions,
                             &application,
                             [&application, fixturePath](const QStringList &suggestions) {
                const bool pass = suggestions.count() == 2
                    && suggestions.at(0) == QLatin1String("arora browser");
                qInfo() << "search-smoke:" << (pass ? "PASS" : "FAIL")
                        << "suggestions:" << suggestions;
                QFile::remove(fixturePath);
                application.exit(pass ? 0 : 1);
            });
            suggest->requestSuggestions(QLatin1String("arora"));
            QTimer::singleShot(15000, &application,
                               [&application]() { application.exit(1); });
        }
    }

    // Headless verification for MIG09: the profile url request
    // interceptor (the only request-blocking surface under WebEngine)
    // must fail a request matching a custom rule (info.block() ->
    // net::ERR_ACCESS_DENIED/ERR_BLOCKED_BY_CLIENT); an @@ exception
    // must let the request through to fail on its own (any other net
    // error proves the interceptor did not stop it); and a ## cosmetic
    // filter must be injected as a <style> element into a loaded page.
    // Exits 0 on
    // PASS for all three stages.  App-data writes are isolated by the
    // QStandardPaths test mode enabled earlier in main().
    // Lives at function scope: the loadFinished/loadingChanged lambdas
    // below capture it by reference and fire inside exec(), after the
    // smoke's if-block has already closed.
    int adblockSmokeStage = 0;
    if (args.contains(QLatin1String("--adblock-smoke"))) {
        AdBlockManager *manager = AdBlockManager::instance();
        AdBlockSubscription *custom = manager->customRules();
        purgeSmokeFilters(custom);
        restoreAdBlockStateOnExit();
        custom->addRule(AdBlockRule(QLatin1String("||adblock-smoke.invalid^")));
        custom->addRule(AdBlockRule(QLatin1String(".invalid^")));
        custom->addRule(AdBlockRule(QLatin1String("@@||allowed-smoke.invalid^")));
        custom->addRule(AdBlockRule(QLatin1String("##body")));

        // Snapshot-level check of what the IO-thread matcher sees.
        AdBlockNetwork *network = manager->network();
        const bool blockOk =
            network->shouldBlock(QUrl(QLatin1String("http://adblock-smoke.invalid/banner.js")));
        const bool exceptionOk =
            !network->shouldBlock(QUrl(QLatin1String("http://allowed-smoke.invalid/page.js")));
        const bool cleanOk =
            !network->shouldBlock(QUrl(QLatin1String("http://example.com/page.js")));
        const bool matcherOk = blockOk && exceptionOk && cleanOk;
        qInfo() << "adblock-smoke: matcher" << (matcherOk ? "PASS" : "FAIL")
                << "block" << blockOk << "exception" << exceptionOk
                << "clean" << cleanOk;
        if (!matcherOk)
            return 1;

        QObject::connect(view->webPage(), &QWebEnginePage::loadingChanged,
                         &application,
                         [view, &application, &adblockSmokeStage](const QWebEngineLoadingInfo &info) {
            if (info.status() != QWebEngineLoadingInfo::LoadFailedStatus)
                return;
            const QString host = info.url().host();
            const bool blockedByInterceptor =
                info.errorString().contains(QLatin1String("ERR_BLOCKED_BY_CLIENT"))
                || info.errorString().contains(QLatin1String("ERR_ACCESS_DENIED"));
            if (adblockSmokeStage == 0 && host == QLatin1String("adblock-smoke.invalid")) {
                qInfo() << "adblock-smoke: blocked navigation"
                        << (blockedByInterceptor ? "PASS" : "FAIL") << info.errorString();
                if (!blockedByInterceptor) {
                    application.exit(1);
                    return;
                }
                adblockSmokeStage = 1;
                view->loadUrl(QUrl(QLatin1String("http://allowed-smoke.invalid/")));
            } else if (adblockSmokeStage == 1 && host == QLatin1String("allowed-smoke.invalid")) {
                const bool pass = !blockedByInterceptor;
                qInfo() << "adblock-smoke: exception navigation"
                        << (pass ? "PASS" : "FAIL") << info.errorString();
                if (!pass) {
                    application.exit(1);
                    return;
                }
                adblockSmokeStage = 2;
                view->loadUrl(QUrl(QLatin1String("about:blank")));
            }
        });
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, &application, &adblockSmokeStage](bool ok) {
            if (adblockSmokeStage != 2 || !ok
                || view->url() != QUrl(QLatin1String("about:blank")))
                return;
            adblockSmokeStage = 3;
            // WebPage::schedulePageScripts armed the "arora:adblock-
            // cosmetic" DocumentReady script at commit, so the style
            // element must already exist by loadFinished.
            view->webPage()->runJavaScript(
                QLatin1String("!!document.getElementById('arora-adblock')"),
                [&application](const QVariant &result) {
                    const bool pass = result.toBool();
                    qInfo() << "adblock-smoke: cosmetic injection"
                            << (pass ? "PASS" : "FAIL");
                    application.exit(pass ? 0 : 1);
                });
        });
        QTimer::singleShot(20000, &application, [&application]() {
            qInfo() << "adblock-smoke: FAIL (timeout)";
            application.exit(1);
        });
        view->loadUrl(QUrl(QLatin1String("http://adblock-smoke.invalid/")));
    }

    // Headless verification for ADB01: subscribe to a real filter list
    // (default: live EasyList over https), let AdBlockSubscription
    // download it through the app-side NetworkAccessManager, then check
    // that the parsed rules actually reach the IO-thread matcher —
    // including a URL derived from a rule taken out of the downloaded
    // list itself — alongside a second (custom) subscription.  Exits 0
    // on PASS.
    const int listSmokeIndex = args.indexOf(QLatin1String("--adblock-list-smoke"));
    if (listSmokeIndex != -1) {
        QUrl listUrl(QStringLiteral("https://easylist.to/easylist/easylist.txt"));
        if (args.count() > listSmokeIndex + 1
            && !args.at(listSmokeIndex + 1).startsWith(QLatin1Char('-')))
            listUrl = QUrl(args.at(listSmokeIndex + 1));

        AdBlockManager *manager = AdBlockManager::instance();
        AdBlockNetwork *network = manager->network();

        // Earlier runs persisted their smoke subscription into the
        // shared test-mode settings list; drop leftovers before the
        // exit-restore snapshot is taken.
        for (AdBlockSubscription *existing : manager->subscriptions()) {
            const QUrlQuery query(existing->url());
            if (query.queryItemValue(QLatin1String("title"),
                                     QUrl::PrettyDecoded)
                    == QLatin1String("list-smoke"))
                manager->removeSubscription(existing);
        }

        // A second subscription proves multi-subscription matching.
        AdBlockSubscription *custom = manager->customRules();
        purgeSmokeFilters(custom);
        restoreAdBlockStateOnExit();
        custom->addRule(AdBlockRule(QLatin1String("||list-smoke.invalid^")));

        QUrl subscribeUrl;
        subscribeUrl.setScheme(QLatin1String("abp"));
        subscribeUrl.setPath(QLatin1String("subscribe"));
        QUrlQuery subscribeQuery;
        subscribeQuery.addQueryItem(QLatin1String("location"),
                                    QString::fromUtf8(listUrl.toEncoded()));
        subscribeQuery.addQueryItem(QLatin1String("title"),
                                    QStringLiteral("list-smoke"));
        subscribeUrl.setQuery(subscribeQuery);
        AdBlockSubscription *subscription =
            new AdBlockSubscription(subscribeUrl, manager);
        manager->addSubscription(subscription);
        // TELEM01: remote downloads are consent-gated; the smoke is an
        // explicit user action, so grant and kick the fetch.
        AdBlockManager::setRemoteListsConsent(
            AdBlockManager::RemoteListsGranted);
        subscription->updateNow();

        QObject::connect(subscription, &AdBlockSubscription::rulesChanged,
                         &application,
                         [&application, subscription, network]() {
            const QList<AdBlockRule> rules = subscription->allRules();
            int cosmetic = 0;
            int probes = 0;
            QUrl derivedUrl;
            bool foundRule = false;
            for (const AdBlockRule &rule : rules) {
                if (!rule.isEnabled())
                    continue;
                if (rule.isCSSRule()) {
                    ++cosmetic;
                    continue;
                }
                if (foundRule || probes >= 25 || rule.isException()
                    || !rule.isSupported())
                    continue;
                ++probes;
                // Turn "||host^..." / substring patterns into a probe
                // URL the rule must match.
                QString pattern = rule.filter();
                const int dollar = pattern.indexOf(QLatin1Char('$'));
                if (dollar != -1)
                    pattern = pattern.left(dollar);
                pattern.remove(QLatin1Char('|')).remove(QLatin1Char('^'))
                    .remove(QLatin1Char('*'));
                if (pattern.size() < 4)
                    continue;
                if (!pattern.contains(QLatin1Char('/'))
                    && pattern.contains(QLatin1Char('.'))) {
                    derivedUrl = QUrl(QLatin1String("http://")
                                      + pattern + QLatin1Char('/'));
                } else {
                    derivedUrl = QUrl(QLatin1String("http://example.com/")
                                      + pattern);
                }
                if (network->match(derivedUrl).action
                    != AdBlockDecision::Allow)
                    foundRule = true;
            }
            const bool customOk = network->shouldBlock(
                QUrl(QLatin1String("http://list-smoke.invalid/x")));
            const bool pass = rules.count() > 5000 && cosmetic > 0
                && foundRule && customOk;
            qInfo() << "adblock-list-smoke:" << (pass ? "PASS" : "FAIL")
                    << "rules:" << rules.count() << "cosmetic:" << cosmetic
                    << "derived:" << derivedUrl << "found:" << foundRule
                    << "custom:" << customOk;
            application.exit(pass ? 0 : 1);
        });
        QTimer::singleShot(90000, &application, [&application]() {
            qInfo() << "adblock-list-smoke: FAIL (timeout)";
            application.exit(1);
        });
    }

    // Headless verification for MIG10: stored form data is filled into
    // a loaded page by the injected autofill.js; a submit is reported
    // back through the aroraAutofill channel object and merged into the
    // store; an off-the-record page is still filled (old private-mode
    // parity) but must never be captured; and the store round-trips
    // through autofill.dat.  Exits 0 on PASS for all stages.
    // Same lifetime reason as adblockSmokeStage above — the lambdas
    // fire inside exec() after this if-block has closed.
    int autofillSmokeStage = 0;
    if (args.contains(QLatin1String("--autofill-smoke"))) {
        AutoFillManager *autoFill = AutoFillManager::instance();

        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-autofill-smoke.html"));
        QFile fixture(fixturePath);
        if (!fixture.open(QIODevice::WriteOnly)) {
            qInfo() << "autofill-smoke: FAIL (cannot write fixture)";
            return 1;
        }
        fixture.write("<html><body><form name=\"login\""
                      " onsubmit=\"return false\">"
                      "<input id=\"u\" name=\"user\" type=\"text\">"
                      "<input id=\"p\" name=\"pass\" type=\"password\">"
                      "<input type=\"submit\" value=\"go\">"
                      "</form></body></html>");
        fixture.close();
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        // A stored entry for the fixture url means later captures take
        // the replace path and never hit the interactive
        // save-password prompt (which cannot be answered offscreen).
        AutoFillManager::Form seed;
        seed.url = fixtureUrl;
        seed.name = QLatin1String("login");
        seed.hasAPassword = true;
        seed.elements
            << qMakePair(QStringLiteral("user"), QStringLiteral("seeduser"))
            << qMakePair(QStringLiteral("pass"), QStringLiteral("seedpass"));
        autoFill->setForms(QList<AutoFillManager::Form>() << seed);

        auto hasElement = [autoFill](const QString &name, const QString &value) {
            const QList<AutoFillManager::Form> forms = autoFill->forms();
            for (const AutoFillManager::Form &form : forms)
                for (const AutoFillManager::Element &element : form.elements)
                    if (element.first == name && element.second == value)
                        return true;
            return false;
        };

        // Capture on the main (persistent) profile: a requestSubmit()
        // fires the submit event, the injected listener serializes the
        // form and reports it through the aroraAutofill bridge, which
        // replaces the seeded entry via autoFillChanged.
        QObject::connect(autoFill, &AutoFillManager::autoFillChanged,
                         &application,
                         [&application, autoFill, hasElement,
                          &autofillSmokeStage]() {
            if (autofillSmokeStage != 1)
                return;
            const bool pass = autoFill->forms().count() == 1
                && hasElement(QLatin1String("user"), QLatin1String("newuser"))
                && hasElement(QLatin1String("pass"), QLatin1String("newpass"));
            qInfo() << "autofill-smoke: capture" << (pass ? "PASS" : "FAIL")
                    << "forms:" << autoFill->forms().count();
            if (!pass) {
                application.exit(1);
                return;
            }
            autofillSmokeStage = 2;

            // Off-the-record profile: fill still applies (parity with
            // the old global private mode) but the bridge must drop
            // every submit report.
            QWebEngineProfile *otrProfile = new QWebEngineProfile(&application);
            WebView *otrView = new WebView(otrProfile);
            otrView->setAttribute(Qt::WA_DeleteOnClose);
            otrView->show();
            const QUrl fixtureUrl =
                autoFill->forms().first().url; // same fixture page
            QObject::connect(otrView, &QWebEngineView::loadFinished,
                             &application,
                             [&application, otrView, hasElement, fixtureUrl](bool ok) {
                if (!ok || otrView->url() != fixtureUrl)
                    return;
                otrView->webPage()->runJavaScript(
                    QLatin1String("document.getElementById('u').value"),
                    [&application, otrView, hasElement](const QVariant &result) {
                    // The stored entry holds the values captured in the
                    // previous stage (the seed was replaced).
                    const bool filled =
                        result.toString() == QLatin1String("newuser");
                    qInfo() << "autofill-smoke: otr fill"
                            << (filled ? "PASS" : "FAIL");
                    if (!filled) {
                        delete otrView;
                        application.exit(1);
                        return;
                    }
                    // Give the (supposedly absent) capture hook no
                    // chance: submit and check nothing was stored.
                    otrView->webPage()->runJavaScript(QLatin1String(
                        "document.getElementById('u').value='otruser';"
                        "document.getElementById('p').value='otrpass';"
                        "document.forms[0].requestSubmit();"));
                    QTimer::singleShot(2000, &application,
                                       [&application, hasElement,
                                        otrView]() {
                        const bool pass = !hasElement(
                            QLatin1String("user"), QLatin1String("otruser"));
                        qInfo() << "autofill-smoke: otr capture dropped"
                                << (pass ? "PASS" : "FAIL");
                        if (!pass) {
                            delete otrView;
                            application.exit(1);
                            return;
                        }
                        // autofill.dat round-trip through a fresh
                        // manager reading the same data dir.
                        AutoFillManager *autoFill =
                            AutoFillManager::instance();
                        QMetaObject::invokeMethod(autoFill, "save",
                                                  Qt::DirectConnection);
                        AutoFillManager probe(&application);
                        bool stored = false;
                        for (const AutoFillManager::Form &form : probe.forms())
                            for (const AutoFillManager::Element &e : form.elements)
                                stored |= (e.first == QLatin1String("user")
                                           && e.second == QLatin1String("newuser"));
                        qInfo() << "autofill-smoke: persistence"
                                << (stored ? "PASS" : "FAIL")
                                << "forms:" << probe.forms().count();

                        // SEC03 at-rest checks: autofill.dat must be a
                        // sealed SecureStore blob — magic header, no
                        // plaintext credential bytes — and the seal
                        // must round-trip and reject tampering.
                        bool sealed = false;
                        {
                            QFile storeFile(BrowserPaths::dataFilePath(
                                QLatin1String("autofill.dat")));
                            if (storeFile.open(QIODevice::ReadOnly)) {
                                const QByteArray raw =
                                    storeFile.readAll();
                                sealed = SecureStore::isSealed(raw)
                                    && !raw.contains("newpass")
                                    && !raw.contains("newuser");
                            }
                        }
                        qInfo() << "autofill-smoke: sealed-at-rest"
                                << (sealed ? "PASS" : "FAIL");

                        bool crypto = sealed;
                        if (sealed) {
                            const QByteArray blob =
                                SecureStore::seal("round-trip");
                            bool ok = false;
                            crypto = !blob.isEmpty()
                                && SecureStore::open(blob, &ok)
                                    == "round-trip" && ok;
                            QByteArray tampered = blob;
                            tampered[tampered.size() - 1] =
                                tampered[tampered.size() - 1] ^ 0xff;
                            bool tamperOk = true;
                            SecureStore::open(tampered, &tamperOk);
                            bool strOk = false;
                            const QString str = SecureStore::openString(
                                SecureStore::sealString(
                                    QStringLiteral("p@ss")), &strOk);
                            crypto = crypto && !tamperOk
                                && strOk && str == QLatin1String("p@ss");
                            qInfo() << "autofill-smoke: securestore"
                                    << (crypto ? "PASS" : "FAIL");
                        }

                        // Delete the OTR view before exit — its page
                        // must die before the OTR profile is released.
                        delete otrView;
                        application.exit(stored && sealed && crypto
                                         ? 0 : 1);
                    });
                });
            });
            otrView->loadUrl(fixtureUrl);
        });

        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [view, &application, fixtureUrl,
                          &autofillSmokeStage](bool ok) {
            if (autofillSmokeStage != 0 || !ok || view->url() != fixtureUrl)
                return;
            autofillSmokeStage = 1;
            // Fill check, then rewrite the fields and submit once the
            // channel handshake has had time to install the listener.
            view->webPage()->runJavaScript(
                QLatin1String("document.getElementById('u').value"),
                [&application, view](const QVariant &result) {
                const bool filled =
                    result.toString() == QLatin1String("seeduser");
                qInfo() << "autofill-smoke: fill" << (filled ? "PASS" : "FAIL")
                        << "got:" << result.toString();
                if (!filled) {
                    application.exit(1);
                    return;
                }
                QTimer::singleShot(700, &application, [view]() {
                    view->webPage()->runJavaScript(QLatin1String(
                        "document.getElementById('u').value='newuser';"
                        "document.getElementById('p').value='newpass';"
                        "document.forms[0].requestSubmit();"));
                });
            });
        });
        QTimer::singleShot(30000, &application, [&application]() {
            qInfo() << "autofill-smoke: FAIL (timeout)";
            application.exit(1);
        });
        view->loadUrl(fixtureUrl);
    }

    // ADB02 coverage comparison (CONFIG+=adblock_rust builds only):
    // feed a fixed corpus through the subscriptions and diff the
    // adblock-rust engine's decisions against both the native matcher
    // and the expected outcome.  Decisions normalize to
    // 0=allow 1=block 2=stub-redirect 3=url-rewrite ($removeparam).
    if (args.contains(QLatin1String("--adblock-rust-smoke"))) {
#if defined(ARORA_ADBLOCK_RUST)
        AdBlockManager *manager = AdBlockManager::instance();
        AdBlockSubscription *custom = manager->customRules();
        purgeSmokeFilters(custom);
        restoreAdBlockStateOnExit();
        // Test-mode app data persists between runs — an earlier
        // --adblock-list-smoke leaves a full EasyList subscription
        // behind.  Disable everything but the custom corpus so the
        // probes are deterministic.
        for (AdBlockSubscription *s : manager->subscriptions()) {
            if (s != custom)
                s->setEnabled(false);
        }
        const char *corpus[] = {
            "||ads.example.com^",
            "||banner.example^$script",
            "@@||banner.example^$script,domain=trusted.example",
            "||tracker.example^$third-party",
            "||cdn.example/lib.js$~third-party",
            "||redir.example/vast.xml$redirect=noop-vast-4.0",
            "||param.example^$removeparam=utm_source",
            "smoke.example##.ad-banner",
        };
        for (const char *rule : corpus)
            custom->addRule(AdBlockRule(QLatin1String(rule)));

        AdBlockNetwork *network = manager->network();
        struct Probe {
            const char *url;
            const char *firstParty;
            int resourceType;
            int expected;
        };
        const Probe probes[] = {
            { "http://ads.example.com/a.js", "http://site.example/", 3, 1 },
            { "http://sub.ads.example.com/a", "http://site.example/", 3, 1 },
            { "http://other.example/ads.example.com.js",
              "http://site.example/", 3, 0 },
            { "http://banner.example/b.js", "http://site.example/", 3, 1 },
            { "http://banner.example/b.js", "http://trusted.example/", 3, 0 },
            { "http://banner.example/b.png", "http://site.example/", 4, 0 },
            { "http://tracker.example/t.js", "http://site.example/", 3, 1 },
            { "http://tracker.example/t.js",
              "http://tracker.example/", 3, 0 },
            { "http://cdn.example/lib.js", "http://cdn.example/", 3, 1 },
            { "http://cdn.example/lib.js", "http://site.example/", 3, 0 },
            { "http://redir.example/vast.xml", "http://site.example/", 13, 2 },
            { "http://param.example/x?utm_source=a&keep=1",
              "http://site.example/", 0, 3 },
            { "http://innocent.example/x.js", "http://site.example/", 3, 0 },
        };

        int rustExpected = 0;
        int agree = 0;
        const int total = int(sizeof(probes) / sizeof(probes[0]));
        for (const Probe &probe : probes) {
            const QUrl url(QLatin1String(probe.url));
            const QUrl firstParty(QLatin1String(probe.firstParty));
            const int native = adblockDecisionKind(
                network->matchNative(url, firstParty, probe.resourceType));
            const int rust = adblockDecisionKind(
                network->match(url, firstParty, probe.resourceType));
            agree += (native == rust);
            rustExpected += (rust == probe.expected);
            if (native != rust || rust != probe.expected)
                qInfo() << "adblock-rust-smoke: diff" << probe.url
                        << "native" << native << "rust" << rust
                        << "expected" << probe.expected;
        }

        const QJsonObject cosmetic = network->rustCosmetic(
            QUrl(QLatin1String("http://smoke.example/")));
        const bool cosmeticOk = cosmetic.value(QLatin1String("hide"))
            .toArray().contains(QLatin1String(".ad-banner"));

        const bool pass = rustExpected == total && cosmeticOk;
        qInfo() << "adblock-rust-smoke:" << (pass ? "PASS" : "FAIL")
                << "rust-matches-expected" << rustExpected << "/" << total
                << "native-agrees" << agree << "/" << total
                << "cosmetic" << cosmeticOk;
        return pass ? 0 : 1;
#else
        qInfo() << "adblock-rust-smoke: SKIP"
                << "(built without CONFIG+=adblock_rust)";
        return 0;
#endif
    }

    // Headless verification for MIG11: the settings dialog's
    // websettings map must land on the profile's QWebEngineSettings
    // (fonts, WebAttribute toggles, user style sheet injected as a
    // QWebEngineScript — setUserStyleSheetUrl is gone), the cookie and
    // network groups must reach the profile cookie jar and http cache
    // settings, the shared Accept-Language helpers must emit a valid
    // header, and ClearPrivateData must wipe history, cookies and the
    // icon cache.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--settings-smoke"))) {
        bool ok = true;
        const auto check = [&ok](bool condition, const char *what) {
            if (!condition)
                qInfo() << "settings-smoke: FAIL at" << what;
            ok = ok && condition;
        };

        const QString cssPath = QDir::temp().filePath(
            QLatin1String("arora-settings-smoke.css"));
        {
            QFile css(cssPath);
            if (css.open(QIODevice::WriteOnly))
                css.write("body { background: red; }");
        }

        SettingsDialog dialog;
        dialog.enableJavascript->setChecked(false);
        dialog.enableImages->setChecked(false);
        dialog.acceptCombo->setCurrentIndex(1);   // CookieJar::AcceptNever
        dialog.networkCache->setChecked(false);
        dialog.userStyleSheet->setText(cssPath);
        dialog.accept();

        QWebEngineSettings *engineSettings = profile->settings();
        check(!engineSettings->testAttribute(QWebEngineSettings::JavascriptEnabled),
              "enableJavascript");
        check(!engineSettings->testAttribute(QWebEngineSettings::AutoLoadImages),
              "enableImages");
        check(profile->httpCacheType() == QWebEngineProfile::NoCache,
              "httpCacheType");
        check(cookieJar->acceptPolicy() == CookieJar::AcceptNever,
              "acceptPolicy");
        check(!profile->httpAcceptLanguage().isEmpty(),
              "httpAcceptLanguage");
        bool foundStyleScript = false;
        const QList<QWebEngineScript> scripts = profile->scripts()->toList();
        for (const QWebEngineScript &script : scripts)
            foundStyleScript |= script.name() == QLatin1String("aroraUserStyleSheet");
        check(foundStyleScript, "userStyleSheet script");

        check(AcceptLanguageDialog::httpString(
                  QStringList() << QLatin1String("English (United States) [en-us]")
                                << QLatin1String("French [fr]"))
              == "en-us, fr;q=0.9", "httpString");
        check(!AcceptLanguageDialog::acceptLanguages().isEmpty(),
              "acceptLanguages");

        // ClearPrivateData: seed history, an icon and a cookie, then
        // clear and verify everything is gone.
        HistoryManager *history = HistoryManager::instance();
        const QUrl seededUrl(QLatin1String("http://settings-smoke.example/"));
        history->addHistoryEntry(seededUrl.toString());
        const QIcon seededIcon(QPixmap(4, 4));
        history->setIcon(seededUrl, seededIcon);
        cookieJar->setAcceptPolicy(CookieJar::AcceptAlways);
        cookieJar->setCookiesFromUrl(
            QList<QNetworkCookie>() << QNetworkCookie("smoke", "1"),
            seededUrl);

        ClearPrivateData clearDialog;
        clearDialog.accept();

        check(!history->historyContains(seededUrl.toString()),
              "browsing history cleared");
        check(cookieJar->cookies().isEmpty(), "cookies cleared");
        check(history->icon(seededUrl).cacheKey() != seededIcon.cacheKey(),
              "icons cleared");

        // Leave no residue: drop the keys the dialog wrote and
        // re-apply defaults (also exercises the reset path).
        QSettings().clear();
        BrowserProfile::applySettings(profile);
        QFile::remove(cssPath);

        qInfo() << "settings-smoke:" << (ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    // Headless verification for MIG12: the in-page find bar drives
    // QWebEngineView::findText — forward/backward wrap freely
    // (WebEngine always wraps, the FindWrapsAroundDocument flag is
    // gone), a miss sets the "Not Found" info label, and the
    // Highlight-All toggle reduces to re-find/clear since WebEngine
    // highlights every match anyway.  The render-side selection is
    // read back through window.getSelection().  Exits 0 on PASS.
    if (args.contains(QLatin1String("--find-smoke"))) {
        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-find-smoke.html"));
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "find-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("<html><body><p>needle one</p>"
                          "<p>haystack</p><p>needle two</p></body></html>");
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        // A leftover ##body cosmetic rule in the shared test-mode
        // settings hides the whole page (display:none text is
        // unfindable); suspend adblocking for the duration and put the
        // persisted flag back on the way out.
        AdBlockManager *adblock = AdBlockManager::instance();
        const bool adblockWasEnabled = adblock->isEnabled();
        adblock->setEnabled(false);
        QObject::connect(&application, &QCoreApplication::aboutToQuit,
                         &application, [adblock, adblockWasEnabled]() {
            adblock->setEnabled(adblockWasEnabled);
        });

        WebViewSearch *searchBar = new WebViewSearch(view, &window);
        QLineEdit *searchEdit =
            searchBar->findChild<QLineEdit*>(QLatin1String("searchLineEdit"));
        QLabel *searchInfo =
            searchBar->findChild<QLabel*>(QLatin1String("searchInfo"));
        QToolButton *highlightAll =
            searchBar->findChild<QToolButton*>(QLatin1String("highlightAllButton"));
        if (!searchEdit || !searchInfo || !highlightAll) {
            qInfo() << "find-smoke: FAIL (search bar widgets missing)";
            return 1;
        }

        // findText answers asynchronously; every reply lands here.
        // (The find highlight is renderer-internal — window.getSelection()
        // does not observe it — so the result object is the readback.)
        auto resultsSeen = std::make_shared<int>(0);
        auto lastMatches = std::make_shared<int>(-1);
        auto lastActive = std::make_shared<int>(-1);
        QObject::connect(view->webPage(), &QWebEnginePage::findTextFinished,
                         &application,
                         [resultsSeen, lastMatches, lastActive]
                         (const QWebEngineFindTextResult &result) {
            *lastMatches = result.numberOfMatches();
            *lastActive = result.activeMatch();
            ++*resultsSeen;
        });

        // Runs ready() once a findText reply newer than 'before' has
        // arrived (or after ~5s — the check then fails on stale data).
        auto awaitResult = [resultsSeen](int before,
                                         std::function<void()> ready) {
            auto ticks = std::make_shared<int>(0);
            QTimer *poll = new QTimer(qApp);
            QObject::connect(poll, &QTimer::timeout, qApp,
                [resultsSeen, before, ready, ticks, poll]() {
                if (*resultsSeen > before || ++*ticks > 100) {
                    poll->stop();
                    poll->deleteLater();
                    ready();
                }
            });
            poll->start(50);
        };
        // A short grace period after each reply lets the search bar's
        // own callback (which owns the info label) settle.
        auto settle = [](std::function<void()> fn) {
            QTimer::singleShot(200, qApp, [fn]() { fn(); });
        };

        QObject::connect(view, &QWebEngineView::loadFinished, &application,
            [view, fixtureUrl, fixturePath, searchBar, searchEdit,
             searchInfo, highlightAll, resultsSeen, lastMatches, lastActive,
             awaitResult, settle](bool ok) {
            if (!ok || view->url() != fixtureUrl)
                return;

            // Stage 1: forward find selects the first of two matches.
            searchEdit->setText(QLatin1String("needle"));
            const int base1 = *resultsSeen;
            searchBar->findNext();
            awaitResult(base1, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 2 && *lastActive >= 0
                    && searchInfo->text().isEmpty();
                qInfo() << "find-smoke: next" << (pass ? "PASS" : "FAIL")
                        << "matches:" << *lastMatches
                        << "active:" << *lastActive;
                if (!pass) { qApp->exit(1); return; }
                const int active1 = *lastActive;

                // Stage 2: forward again moves to the second match.
                const int base2 = *resultsSeen;
                searchBar->findNext();
                awaitResult(base2, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 2
                    && *lastActive != active1;
                qInfo() << "find-smoke: next-wrap"
                        << (pass ? "PASS" : "FAIL")
                        << "active:" << *lastActive;
                if (!pass) { qApp->exit(1); return; }

                // Stage 3: backward returns to the first match.
                const int base3 = *resultsSeen;
                searchBar->findPrevious();
                awaitResult(base3, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 2
                    && *lastActive == active1;
                qInfo() << "find-smoke: previous"
                        << (pass ? "PASS" : "FAIL")
                        << "active:" << *lastActive;
                if (!pass) { qApp->exit(1); return; }

                // Stage 4: a miss reports Not Found on the info label.
                searchEdit->setText(QLatin1String("zzz-absent"));
                const int base4 = *resultsSeen;
                searchBar->findNext();
                awaitResult(base4, [=]() {
                settle([=]() {
                const bool pass = *lastMatches == 0
                    && !searchInfo->text().isEmpty();
                qInfo() << "find-smoke: not-found"
                        << (pass ? "PASS" : "FAIL")
                        << "info:" << searchInfo->text();
                if (!pass) { qApp->exit(1); return; }

                // Stage 5: toggling Highlight-All on re-runs the find
                // (WebEngine always highlights every match).
                searchEdit->setText(QLatin1String("needle"));
                const int base5 = *resultsSeen;
                highlightAll->setChecked(true);
                awaitResult(base5, [=]() {
                const bool pass = *lastMatches == 2;
                qInfo() << "find-smoke: highlight-all"
                        << (pass ? "PASS" : "FAIL")
                        << "matches:" << *lastMatches;
                if (!pass) { qApp->exit(1); return; }

                // Stage 6: toggling off clears the find (no reply is
                // emitted for an empty needle — verify the next find
                // still works afterwards).
                highlightAll->setChecked(false);
                const int base6 = *resultsSeen;
                searchBar->findNext();
                awaitResult(base6, [=]() {
                const bool pass = *lastMatches == 2;
                qInfo() << "find-smoke:"
                        << (pass ? "PASS" : "FAIL")
                        << "(refind-after-clear:" << pass << ")";
                QFile::remove(fixturePath);
                qApp->exit(pass ? 0 : 1);
                });
                });
                });
                });
                });
                });
                });
                });
                });
            });
        });
        QTimer::singleShot(20000, &application, []() {
            qInfo() << "find-smoke: FAIL (timeout)";
            qApp->exit(1);
        });
        view->loadUrl(fixtureUrl);
    }

    // Headless verification for MIG12 (view source): the viewer
    // re-fetches the page through the app-side NAM and shows the raw
    // wire bytes when they parse to the same DOM the page serialized
    // — otherwise the DOM dump is shown.  A second viewer fed a bogus
    // dump exercises the fallback branch.  The syntax highlighter is
    // checked on a standalone document (no renderer needed) and the
    // in-viewer find bar exercises PlainTextEditSearch.  Exits 0 on
    // PASS.
    if (args.contains(QLatin1String("--source-smoke"))) {
        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-source-smoke.html"));
        const QByteArray bytes =
            "<html><!--c--><body><p class=\"x\">needle &amp; more</p>"
            "</body></html>";
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "source-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write(bytes);
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);

        // Cosmetic adblock rules would be injected into the serialized
        // DOM the viewer compares against (see --find-smoke); suspend
        // adblocking for the duration.
        AdBlockManager *adblock = AdBlockManager::instance();
        const bool adblockWasEnabled = adblock->isEnabled();
        adblock->setEnabled(false);
        QObject::connect(&application, &QCoreApplication::aboutToQuit,
                         &application, [adblock, adblockWasEnabled]() {
            adblock->setEnabled(adblockWasEnabled);
        });

        // The ported QRegularExpression state machine must mark up a
        // document without any WebEngine involvement.
        QTextDocument document;
        SourceHighlighter highlighter(&document);
        document.setPlainText(QString::fromUtf8(bytes));
        // Highlighting lands in the block layout, which is computed lazily.
        document.documentLayout()->documentSize();
        if (document.firstBlock().layout()->formats().isEmpty()) {
            qInfo() << "source-smoke: FAIL (highlighter produced no formats)";
            return 1;
        }
        qInfo() << "source-smoke: highlighter PASS";

        auto pending = std::make_shared<int>(0);
        auto failures = std::make_shared<int>(0);
        auto finish = [&application, pending, failures,
                       fixturePath](bool ok) {
            *failures += ok ? 0 : 1;
            if (--*pending == 0) {
                qInfo() << "source-smoke:"
                        << (*failures == 0 ? "PASS" : "FAIL");
                QFile::remove(fixturePath);
                application.exit(*failures == 0 ? 0 : 1);
            }
        };

        // Waits out the re-fetch + probe-page comparison, then checks
        // the shown text and drives the viewer's find bar.
        auto checkViewer = [finish](SourceViewer *viewer,
                                    const QString &expected,
                                    const char *what) {
            QPlainTextEdit *edit = viewer->findChild<QPlainTextEdit*>();
            PlainTextEditSearch *search =
                viewer->findChild<PlainTextEditSearch*>();
            QLineEdit *searchEdit = search
                ? search->findChild<QLineEdit*>(QLatin1String("searchLineEdit"))
                : nullptr;
            if (!edit || !search || !searchEdit) {
                finish(false);
                return;
            }
            auto ticks = std::make_shared<int>(0);
            QTimer *poll = new QTimer(viewer);
            QObject::connect(poll, &QTimer::timeout, viewer,
                [edit, search, searchEdit, expected, what,
                 ticks, poll, finish]() {
                if (edit->toPlainText() == QLatin1String("Loading...")) {
                    if (++*ticks > 100) {
                        poll->stop();
                        qInfo() << "source-smoke:" << what
                                << "FAIL (probe timeout)";
                        finish(false);
                    }
                    return;
                }
                poll->stop();
                const bool contentOk = edit->toPlainText() == expected;
                if (!contentOk)
                    qInfo() << "source-smoke:" << what << "shown was:"
                            << edit->toPlainText().left(200)
                            << "| expected:" << expected.left(200);
                searchEdit->setText(QLatin1String("needle"));
                search->findNext();
                const bool findOk =
                    !expected.contains(QLatin1String("needle"))
                    || edit->textCursor().selectedText()
                           == QLatin1String("needle");
                qInfo() << "source-smoke:" << what
                        << (contentOk && findOk ? "PASS" : "FAIL")
                        << "(content:" << contentOk << "find:" << findOk << ")";
                finish(contentOk && findOk);
            });
            poll->start(100);
        };

        QObject::connect(view, &QWebEngineView::loadFinished, &application,
            [view, fixtureUrl, bytes, pending, checkViewer](bool ok) {
            if (!ok || view->url() != fixtureUrl)
                return;
            // Serialize the loaded DOM exactly like
            // BrowserMainWindow::viewPageSource() does.
            view->webPage()->toHtml(
                [view, fixtureUrl, bytes, pending, checkViewer](const QString &markup) {
                // The faithful DOM dump: the probe must judge the raw
                // bytes equivalent and show them verbatim.
                *pending += 2;
                checkViewer(new SourceViewer(markup, view->title(),
                                             fixtureUrl, view),
                            QString::fromUtf8(bytes), "raw");
                // A mismatched dump must be shown as-is (probe's
                // toHtml never equals it).
                checkViewer(new SourceViewer(QLatin1String("bogus-dom-dump"),
                                             view->title(), fixtureUrl, view),
                            QLatin1String("bogus-dom-dump"), "fallback");
            });
        });
        QTimer::singleShot(30000, &application, [&application]() {
            qInfo() << "source-smoke: FAIL (timeout)";
            application.exit(1);
        });
        view->loadUrl(fixtureUrl);
    }

    // Headless verification for MIG14: a real BrowserMainWindow must
    // construct with one tab, gain a second through the window-level
    // new-tab action, load a file:// page and propagate its title to
    // the window title, and route a script-driven window.open()
    // through WebPage::createWindow -> TabWidget::getView into a third
    // tab.  The harness runs a plain QApplication, so
    // BrowserApplication::instance() is null and the chrome must
    // degrade gracefully.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--browser-smoke"))) {
        // A leftover ##body cosmetic rule from earlier test-mode runs
        // would restyle the page; suspend adblock for determinism and
        // put the persisted flag back on the way out — the test-mode
        // settings file is shared with the other smokes, and leaving
        // enabled=false behind silently breaks --adblock-smoke's
        // matcher check on the next run.
        AdBlockManager *adblock = AdBlockManager::instance();
        const bool adblockWasEnabled = adblock->isEnabled();
        adblock->setEnabled(false);
        QObject::connect(&application, &QCoreApplication::aboutToQuit,
                         &application, [adblock, adblockWasEnabled]() {
            adblock->setEnabled(adblockWasEnabled);
        });

        // The window is deleted before application.exit() below: an
        // unregistered window would otherwise outlive the profile at
        // teardown and crash inside QtWebEngine's shutdown.
        BrowserMainWindow *browserWindow = new BrowserMainWindow();
        browserWindow->show();
        TabWidget *tabWidget = browserWindow->tabWidget();

        bool ok = tabWidget && tabWidget->count() == 1
            && tabWidget->currentWebView()
            && browserWindow->toolbarSearch()
            && browserWindow->menuBar();
        if (!ok) {
            qInfo() << "browser-smoke: FAIL (window construction)";
            return 1;
        }
        qInfo() << "browser-smoke: window PASS (1 tab)";

        tabWidget->newTabAction()->trigger();
        if (tabWidget->count() != 2) {
            qInfo() << "browser-smoke: FAIL (newTabAction)"
                    << "tabs:" << tabWidget->count();
            return 1;
        }
        qInfo() << "browser-smoke: new-tab action PASS (2 tabs)";

        const QString fixturePath = QDir::temp().filePath(
            QLatin1String("arora-browser-smoke.html"));
        {
            QFile fixture(fixturePath);
            if (!fixture.open(QIODevice::WriteOnly)) {
                qInfo() << "browser-smoke: FAIL (cannot write fixture)";
                return 1;
            }
            fixture.write("<html><head><title>browser-smoke-page</title>"
                          "</head><body>chrome"
                          "<a id=\"l\" href=\"about:blank\" target=\"_blank\">x</a>"
                          "</body></html>");
        }
        const QUrl fixtureUrl = QUrl::fromLocalFile(fixturePath);
        WebView *firstTab = tabWidget->webView(0);
        tabWidget->setCurrentIndex(0);

        QObject::connect(firstTab, &QWebEngineView::loadFinished,
                         &application,
                         [&application, browserWindow, tabWidget, firstTab,
                          fixtureUrl, fixturePath](bool ok) {
            if (!ok || firstTab->url() != fixtureUrl)
                return;
            const bool titleOk = browserWindow->windowTitle()
                .contains(QLatin1String("browser-smoke-page"));
            qInfo() << "browser-smoke: load+title"
                    << (titleOk ? "PASS" : "FAIL")
                    << "title:" << browserWindow->windowTitle();
            if (!titleOk) {
                QFile::remove(fixturePath);
                delete browserWindow;
                application.exit(1);
                return;
            }
            // A real click on the target=_blank link routes through
            // WebPage::createWindow -> TabWidget::getView and must grow
            // the tab strip to three tabs.  (Synthesized JS has no user
            // activation, so Chromium would block it as a popup.)
            firstTab->webPage()->runJavaScript(
                QLatin1String("JSON.stringify("
                              "document.getElementById('l').getBoundingClientRect())"),
                [tabWidget](const QVariant &rectVar) {
                const QJsonObject rect =
                    QJsonDocument::fromJson(rectVar.toString().toUtf8()).object();
                const QPointF pos(rect[QLatin1String("x")].toDouble()
                                      + rect[QLatin1String("width")].toDouble() / 2,
                                  rect[QLatin1String("y")].toDouble()
                                      + rect[QLatin1String("height")].toDouble() / 2);
                WebView *view = tabWidget->currentWebView();
                QWidget *proxy = view->focusProxy() ? view->focusProxy() : view;
                QMouseEvent press(QEvent::MouseButtonPress, pos,
                                  proxy->mapToGlobal(pos.toPoint()),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, pos,
                                    proxy->mapToGlobal(pos.toPoint()),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(proxy, &press);
                QCoreApplication::sendEvent(proxy, &release);
            });
            QTimer::singleShot(3000, &application,
                               [&application, tabWidget, browserWindow,
                                fixturePath]() {
                const bool pass = tabWidget->count() == 3;
                qInfo() << "browser-smoke: target=_blank tab"
                        << (pass ? "PASS" : "FAIL")
                        << "tabs:" << tabWidget->count();
                qInfo() << "browser-smoke:" << (pass ? "PASS" : "FAIL");
                QFile::remove(fixturePath);
                delete browserWindow;
                application.exit(pass ? 0 : 1);
            });
        });
        QTimer::singleShot(20000, &application,
                           [&application, browserWindow]() {
            qInfo() << "browser-smoke: FAIL (timeout)";
            delete browserWindow;
            application.exit(1);
        });
        tabWidget->loadUrl(fixtureUrl, TabWidget::CurrentTab);
    }

    // Headless verification for EXT01: the QWebEngineExtensionManager
    // preview must drive a Manifest-V3 fixture through the whole
    // lifecycle — load (arrives disabled), enable, unload, install
    // into the profile's installPath, uninstall — while the manifest
    // inspector and the user-scripts QWebEngineScriptCollection path
    // are checked synchronously.  Exits 0 on PASS.
    // Function scope on purpose: the async lifecycle connects inside
    // the smoke capture [&] and run in exec() after its if-block has
    // closed — block-local state would dangle (ASan use-after-scope).
    ExtensionManager *extensions = nullptr;
    int failures = 0;
    const auto check = [&failures](bool ok, const char *what) {
        qInfo() << "extension-smoke:" << what << (ok ? "PASS" : "FAIL");
        if (!ok)
            ++failures;
    };
    const auto die = [&application](const QString &why) {
        qInfo() << "extension-smoke: FAIL" << why;
        application.exit(1);
    };
    QString extensionId;
    QString extDir;
    QTimer *enablePoll = nullptr;
    std::shared_ptr<int> pollTicks;
    if (args.contains(QLatin1String("--extension-smoke"))) {
        extensions = ExtensionManager::instance();

        check(ExtensionManager::isSupported(), "webengine_extensions feature");
        check(!extensions->extensions().isEmpty(),
              "built-in components listed");

        // Manifest-V3 fixture on disk.
        extDir = QDir::temp().filePath(
            QLatin1String("arora-ext-smoke"));
        QDir().mkpath(extDir);
        {
            QFile manifestFile(extDir + QLatin1String("/manifest.json"));
            if (!manifestFile.open(QIODevice::WriteOnly)) {
                qInfo() << "extension-smoke: FAIL (cannot write manifest)";
                return 1;
            }
            manifestFile.write(
                "{\"manifest_version\":3,"
                "\"name\":\"arora-smoke-ext\","
                "\"version\":\"0.1\","
                "\"description\":\"EXT01 smoke fixture\","
                "\"permissions\":[\"storage\",\"tabs\",\"nativeMessaging\"],"
                "\"host_permissions\":[\"https://*.example.com/*\"],"
                "\"action\":{\"default_title\":\"smoke\"},"
                "\"background\":{\"service_worker\":\"sw.js\"}}");
            QFile worker(extDir + QLatin1String("/sw.js"));
            if (!worker.open(QIODevice::WriteOnly)) {
                qInfo() << "extension-smoke: FAIL (cannot write worker)";
                return 1;
            }
            worker.write("chrome.runtime.onInstalled.addListener(function(){});\n");
        }

        // Synchronous checks: manifest inspector + permission
        // classification + the user-scripts script-collection path.
        const ExtensionManager::Manifest manifest =
            ExtensionManager::inspectManifest(extDir);
        check(manifest.valid && manifest.manifestVersion == 3
              && manifest.name == QLatin1String("arora-smoke-ext")
              && manifest.hasBackground && manifest.hasAction
              && manifest.permissions.size() == 3
              && manifest.hostPermissions.size() == 1,
              "manifest parsed");
        check(manifest.unsupported.contains(QLatin1String("tabs"))
              && manifest.unsupported.contains(QLatin1String("nativeMessaging"))
              && !manifest.unsupported.contains(QLatin1String("storage")),
              "unsupported chrome.* APIs flagged");
        const ExtensionManager::Manifest mv2 =
            ExtensionManager::inspectManifest(QString());
        check(!mv2.valid && !mv2.error.isEmpty(),
              "missing manifest reported");

        const QString scriptDir = ExtensionManager::userScriptsPath();
        const QString scriptPath = scriptDir
            + QLatin1String("/smoke-user.js");
        {
            QFile script(scriptPath);
            if (!script.open(QIODevice::WriteOnly)) {
                qInfo() << "extension-smoke: FAIL (cannot write user script)";
                return 1;
            }
            script.write("// smoke\n");
        }
        extensions->reloadUserScripts();
        bool scriptInstalled = false;
        const QList<QWebEngineScript> profileScripts = profile->scripts()->toList();
        for (const QWebEngineScript &script : profileScripts) {
            if (script.name() == QLatin1String("userscript:smoke-user.js"))
                scriptInstalled = true;
        }
        check(scriptInstalled, "user script injected into profile");
        check(extensions->userScriptNames().contains(QLatin1String("smoke-user.js")),
              "user script listed");
        QFile::remove(scriptPath);
        extensions->reloadUserScripts();

        // Async lifecycle driven by the manager's finished signals.
        extensionId.clear();
        enablePoll = new QTimer(&application);
        pollTicks = std::make_shared<int>(0);

        QObject::connect(extensions, &ExtensionManager::extensionLoaded,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.name != QLatin1String("arora-smoke-ext"))
                return;
            check(info.loaded && info.error.isEmpty(),
                  "loadExtension finished");
            check(!info.enabled, "extension loads disabled");
            extensionId = info.id;
            extensions->setExtensionEnabled(extensionId, true);
            *pollTicks = 0;
            enablePoll->start(200);
        });

        QObject::connect(enablePoll, &QTimer::timeout, &application, [&]() {
            for (const ExtensionManager::ExtensionInfo &info
                 : extensions->extensions()) {
                if (info.id == extensionId && info.enabled) {
                    enablePoll->stop();
                    qInfo() << "extension-smoke: enable PASS";
                    // Loaded-but-not-installed removes via unload.
                    extensions->removeExtension(extensionId);
                    return;
                }
            }
            if (++*pollTicks > 25) {
                enablePoll->stop();
                die(QStringLiteral("setExtensionEnabled never applied"));
            }
        });

        QObject::connect(extensions, &ExtensionManager::extensionUnloaded,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.id != extensionId)
                return;
            qInfo() << "extension-smoke: unload PASS";
            extensions->installExtension(extDir);
        });

        QObject::connect(extensions, &ExtensionManager::extensionInstalled,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.name != QLatin1String("arora-smoke-ext")) {
                qInfo() << "extension-smoke: installFinished ignored"
                        << "name:" << info.name << "id:" << info.id
                        << "installed:" << info.installed
                        << "loaded:" << info.loaded
                        << "error:" << info.error;
                return;
            }
            check(info.installed && info.error.isEmpty(),
                  "installExtension finished");
            check(!extensions->installPath().isEmpty()
                  && info.path.startsWith(extensions->installPath()),
                  "install persisted under profile installPath");
            extensionId = info.id;
            extensions->removeExtension(extensionId);
        });

        QObject::connect(extensions, &ExtensionManager::extensionUninstalled,
            &application, [&](const ExtensionManager::ExtensionInfo &info) {
            if (info.id != extensionId)
                return;
            bool gone = true;
            for (const ExtensionManager::ExtensionInfo &rest
                 : extensions->extensions()) {
                if (rest.id == extensionId)
                    gone = false;
            }
            check(gone, "uninstall removes extension");
            qInfo() << "extension-smoke:"
                    << (failures == 0 ? "PASS" : "FAIL")
                    << "failures:" << failures;
            QDir(extDir).removeRecursively();
            application.exit(failures == 0 ? 0 : 1);
        });

        QObject::connect(extensions, &ExtensionManager::errorOccurred,
            &application, [die](const QString &message) {
            die(QStringLiteral("errorOccurred: %1").arg(message));
        });

        QTimer::singleShot(20000, &application, [die]() {
            die(QStringLiteral("timeout"));
        });
        extensions->loadExtension(extDir);

        // exec() must run while this block's locals are still alive:
        // the finished-signal lambdas above capture them by reference.
        return application.exec();
    }

    // Headless verification for UA01: the browsing profile must send a
    // vanilla Chrome UA — Qt's factory default minus the
    // "QtWebEngine/<ver>" product token Google's /sorry/ bot check
    // fingerprints — while the UserAgentMenu override still wins and
    // clearing it restores the vanilla UA on both the named and the
    // off-the-record private profile.  The refreshed useragents.xml
    // presets are sanity-checked too.  The live Google search at the
    // end is report-only: offline runs SKIP it and a /sorry/ landing
    // page means IP reputation, not the UA, tripped bot detection.
    // Exits 0 when all local checks PASS.
    if (args.contains(QLatin1String("--ua-smoke"))) {
        int failures = 0;
        const auto check = [&failures](bool ok, const char *what) {
            qInfo() << "ua-smoke:" << what << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++failures;
        };

        const QString vanilla = BrowserProfile::defaultHttpUserAgent();
        check(!vanilla.isEmpty(), "default UA non-empty");
        check(!vanilla.contains(QLatin1String("QtWebEngine")),
              "no QtWebEngine token");
        check(vanilla.contains(QLatin1String("Chrome/"))
              && vanilla.contains(QLatin1String("Safari/")),
              "vanilla UA is Chrome-shaped");
        // UA03: the Chrome milestone presented to sniffers is the
        // presentation version, and the client hints must tell the
        // same story on both brands — a 140-hints/155-UA split is
        // itself a fingerprint.
        const QString presentedMajor =
            QString::number(BrowserProfile::presentedChromeMajor());
        check(vanilla.contains(QLatin1String("Chrome/")
                               + presentedMajor + QLatin1Char('.')),
              "UA presents the bumped Chrome milestone");
        {
            const QVariantMap hints =
                profile->clientHints()->fullVersionList();
            const QString chromiumHint =
                hints.value(QLatin1String("Chromium")).toString();
            const QString chromeHint =
                hints.value(QLatin1String("Google Chrome")).toString();
            check(chromiumHint.startsWith(presentedMajor + QLatin1Char('.'))
                  && chromeHint == chromiumHint,
                  "client hints brands carry the presented version");
            check(profile->clientHints()->fullVersion() == chromiumHint,
                  "fullVersion hint agrees with the brands");
        }
        check(profile->httpUserAgent() == vanilla,
              "browsing profile sends vanilla UA");

        // The UserAgentMenu override wins; clearing it must restore
        // the vanilla UA (an empty string used to be written back).
        WebPage::setUserAgent(QLatin1String("smoke-ua/1.0"));
        check(profile->httpUserAgent() == QLatin1String("smoke-ua/1.0"),
              "override reaches browsing profile");
        WebPage::setUserAgent(QString());
        check(profile->httpUserAgent() == vanilla,
              "clearing override restores vanilla UA");

        // The off-the-record private profile gets the same treatment.
        QWebEngineProfile *otr = BrowserProfile::privateProfile();
        BrowserProfile::applySettings(otr);
        check(otr->httpUserAgent() == vanilla,
              "private profile sends vanilla UA");
        WebPage::setUserAgent(QLatin1String("smoke-ua/1.0"));
        check(otr->httpUserAgent() == QLatin1String("smoke-ua/1.0"),
              "override reaches private profile");
        WebPage::setUserAgent(QString());
        check(otr->httpUserAgent() == vanilla,
              "private profile restores vanilla UA");

        // Preset file: parses, has entries, carries no dead-engine
        // (MSIE/Presto/WebKit-era) or self-badged strings.
        int presetCount = 0;
        bool stalePreset = false;
        {
            QFile presets(QLatin1String(":/useragents/useragents.xml"));
            check(presets.open(QIODevice::ReadOnly),
                  "useragents.xml opens");
            QXmlStreamReader xml(&presets);
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()
                    || xml.name() != QLatin1String("useragent"))
                    continue;
                ++presetCount;
                const QString preset = xml.attributes()
                    .value(QLatin1String("useragent")).toString();
                stalePreset |= preset.contains(QLatin1String("MSIE"))
                    || preset.contains(QLatin1String("Presto"))
                    || preset.contains(QLatin1String("QtWebKit"))
                    || preset.contains(QLatin1String("QtWebEngine"));
            }
            check(xml.error() == QXmlStreamReader::NoError,
                  "useragents.xml parses");
            check(presetCount >= 8, "preset count");
            check(!stalePreset, "no stale presets");
        }

        // Live check (report-only): a real Google search must not be
        // diverted to the /sorry/ interstitial.
        QObject::connect(view, &QWebEngineView::loadFinished, &application,
            [&application, view, failures](bool ok) {
            if (!ok) {
                qInfo() << "ua-smoke: live google SKIP"
                           " (load failed — offline?)";
                application.exit(failures ? 1 : 0);
                return;
            }
            const QString url = view->url().toString();
            const bool sorry = url.contains(QLatin1String("/sorry/"));
            qInfo() << "ua-smoke: live google"
                    << (sorry ? "WARN /sorry/ redirect (IP reputation,"
                               " not the UA)" : "PASS")
                    << url;
            application.exit(failures ? 1 : 0);
        });
        QTimer::singleShot(30000, &application,
            [&application, failures]() {
            qInfo() << "ua-smoke: live google SKIP (timeout — offline?)";
            application.exit(failures ? 1 : 0);
        });
        view->loadUrl(QUrl(QLatin1String(
            "https://www.google.com/search?q=arora+browser")));

        // exec() must run while 'failures' is still alive: the lambdas
        // above capture it by reference.
        return application.exec();
    }

    // UA02 empirical probe.  A fresh off-the-record profile reproduces
    // a first-run user's exact state — no cookies, no cache — while
    // applySettings gives it the production vanilla UA.  A
    // cookie-warming load of google.com runs first (consent/SOCS
    // cookies get set naturally), then a real search.  The interceptor
    // dumps the wire request headers (client hints included) and a
    // parallel engine-free NAM GET shows the response status/headers
    // for the same URL + UA + IP — together they separate browser-side
    // fingerprinting from IP-reputation flagging.  Report-only like
    // the ua-smoke live check: a /sorry/ landing prints a verdict
    // instead of failing, and offline runs SKIP with exit 0.
    if (args.contains(QLatin1String("--sorry-smoke"))) {
        QWebEngineProfile *probeProfile = new QWebEngineProfile(&application);
        BrowserProfile::applySettings(probeProfile);

        QWebEngineClientHints *hints = probeProfile->clientHints();
        QStringList brands;
        const QVariantMap brandList = hints->fullVersionList();
        for (auto it = brandList.constBegin(); it != brandList.constEnd(); ++it)
            brands << it.key() + QLatin1Char('/') + it.value().toString();
        qInfo().noquote() << "sorry-smoke: UA:" << probeProfile->httpUserAgent();
        qInfo().noquote() << "sorry-smoke: client-hints platform="
            << hints->platform() << "arch=" << hints->arch()
            << "model=" << hints->model() << "mobile=" << hints->isMobile()
            << "fullVersion=" << hints->fullVersion()
            << "platformVersion=" << hints->platformVersion()
            << "bitness=" << hints->bitness()
            << "brands=" << brands.join(QLatin1Char(','));

        int failures = 0;
        const auto check = [&failures](bool ok, const char *what) {
            qInfo() << "sorry-smoke:" << what << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++failures;
        };
        check(!probeProfile->httpUserAgent()
                .contains(QLatin1String("QtWebEngine")),
              "UA carries no QtWebEngine token");
        check(brandList.contains(QLatin1String("Google Chrome")),
              "client hints carry the Google Chrome brand");

        QMutex headerMutex;
        QStringList captured;
        ProbeHeaderCapture interceptor(&headerMutex, &captured);
        probeProfile->setUrlRequestInterceptor(&interceptor);

        WebView *probeView = new WebView(probeProfile, &window);
        window.setCentralWidget(probeView);

        // Engine-free GET of the same URL + UA + egress IP: its status
        // and response headers are the "is it the browser or the IP"
        // control measurement.
        int namStatus = -1;
        QStringList namHeaders;
        QNetworkRequest namRequest(QUrl(QLatin1String(
            "https://www.google.com/search?q=arora+browser")));
        namRequest.setHeader(QNetworkRequest::UserAgentHeader,
                             BrowserProfile::defaultHttpUserAgent());
        QNetworkReply *namReply = networkAccessManager->get(namRequest);
        QObject::connect(namReply, &QNetworkReply::finished, &application,
            [namReply, &namStatus, &namHeaders]() {
            namStatus = namReply->attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QList<QNetworkReply::RawHeaderPair> raw =
                namReply->rawHeaderPairs();
            for (const QNetworkReply::RawHeaderPair &pair : raw)
                namHeaders.append(QString::fromLatin1(pair.first)
                    + QLatin1String(": ") + QString::fromLatin1(pair.second));
            namReply->deleteLater();
        });

        const QUrl warmUrl(QLatin1String("https://www.google.com/"));
        const QUrl searchUrl(QLatin1String(
            "https://www.google.com/search?q=arora+browser"));
        QElapsedTimer timer;
        timer.start();
        int stage = 0;
        const auto finish = [&application, probeView, &headerMutex,
                             &captured, &namStatus, &namHeaders, &timer,
                             &failures]() {
            const QUrl finalUrl = probeView->url();
            const bool sorry = WebPage::isRateLimitInterstitialUrl(finalUrl);
            qInfo().noquote() << "sorry-smoke: final" << finalUrl
                              << "in" << timer.elapsed() << "ms";
            qInfo().noquote() << "sorry-smoke: control status" << namStatus
                << "\n" << namHeaders.join(QLatin1Char('\n'));
            {
                const QMutexLocker lock(&headerMutex);
                for (const QString &capture : captured)
                    qInfo().noquote() << "sorry-smoke: request\n" << capture;
            }
            if (sorry) {
                qInfo() << "sorry-smoke: WARN /sorry/ landing —"
                        << (namStatus != 200
                            ? "control request also non-200: IP reputation,"
                              " not fixable client-side"
                            : "control request got 200: browser-side"
                              " fingerprint (headers/cookies) — actionable");
            } else {
                qInfo() << "sorry-smoke: PASS";
            }
            application.exit(failures ? 1 : 0);
        };
        QObject::connect(probeView, &QWebEngineView::loadFinished,
            &application, [&](bool ok) {
            if (!ok)
                return;  // timeout reports the SKIP
            if (stage == 0) {
                // Cookie warming done — the consent cookies google.com
                // just set now ride along on the real search.
                stage = 1;
                qInfo() << "sorry-smoke: warmup landed" << probeView->url()
                        << (WebPage::isRateLimitInterstitialUrl(probeView->url())
                            ? "(already /sorry/)" : "");
                probeView->loadUrl(searchUrl);
                return;
            }
            finish();
        });
        QTimer::singleShot(45000, &application, [&application]() {
            qInfo() << "sorry-smoke: SKIP (timeout — offline?)";
            application.exit(0);
        });
        probeView->loadUrl(warmUrl);
        return application.exec();
    }

    // Headless verification for SESS01 — the real session save/restore
    // cycle across two process runs.
    //
    // `--session-smoke` (save phase): three tabs on fixture urls with
    // the middle tab current, then a private window whose tabs must
    // never reach the session blob, then a real window close — the
    // AutoSaver in ~BrowserMainWindow is what saves the session on a
    // user-close before quitOnLastWindowClosed ends the run.  The
    // persisted blob is parsed in aboutToQuit.
    //
    // `--restore-smoke` (restore phase): startupBehavior=2 so the
    // postLaunch() queued by the BrowserApplication ctor runs
    // restoreLastSession() on the window created here; a poll then
    // verifies every tab url in order and the restored current index.
    const bool sessionSaveSmoke = args.contains(QLatin1String("--session-smoke"));
    const bool sessionRestoreSmoke = args.contains(QLatin1String("--restore-smoke"));
    if (sessionSaveSmoke || sessionRestoreSmoke) {
        int sessionFailures = 0;
        const auto sessionCheck =
            [&sessionFailures](bool ok, const char *what) {
            qInfo() << "session-smoke:" << what << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++sessionFailures;
        };

        // The stub window above only hosts the WebView smokes — hide it
        // so closing the real browser window still counts as the last
        // window and ends the run.
        window.hide();
        QSettings().setValue(
            QLatin1String("tabs/confirmClosingMultipleTabs"), false);
        // A leftover crash-loop flag would open a modal prompt.
        QSettings().setValue(QLatin1String("MainWindow/restoring"), false);

        QStringList fixtureUrls;
        for (int i = 1; i <= 3; ++i) {
            const QString path = QDir::temp().filePath(
                QStringLiteral("arora-session-%1.html").arg(i));
            QFile fixture(path);
            if (fixture.open(QIODevice::WriteOnly)) {
                fixture.write(QStringLiteral(
                    "<html><head><title>arora-session-%1</title>"
                    "</head><body>%1</body></html>").arg(i).toUtf8());
            }
            fixtureUrls << QUrl::fromLocalFile(path).toString();
        }

        if (sessionSaveSmoke) {
            // postLaunch runs inside exec() — pin the startup behavior
            // to "blank" so it does not navigate the window away.
            QSettings().setValue(QLatin1String("MainWindow/startupBehavior"), 1);
            QSettings().remove(QLatin1String("sessions"));

            BrowserMainWindow *browserWindow = application.newMainWindow();
            TabWidget *tabWidget = browserWindow->tabWidget();
            tabWidget->loadUrl(QUrl(fixtureUrls.at(0)), TabWidget::CurrentTab);
            tabWidget->loadUrl(QUrl(fixtureUrls.at(1)), TabWidget::NewNotSelectedTab);
            tabWidget->loadUrl(QUrl(fixtureUrls.at(2)), TabWidget::NewNotSelectedTab);
            tabWidget->setCurrentIndex(1);

            // A private window's tabs live on the off-the-record
            // profile: serializing its tab widget must produce an
            // empty tab list, and saveSession() while private must not
            // touch the blob at all.
            BrowserApplication::setPrivate(true);
            BrowserMainWindow *privateWindow = application.newMainWindow();
            const QUrl privateUrl = QUrl::fromLocalFile(
                QDir::temp().filePath(
                    QLatin1String("arora-session-private.html")));
            privateWindow->tabWidget()->loadUrl(privateUrl,
                                                TabWidget::CurrentTab);
            {
                QByteArray privateState =
                    privateWindow->tabWidget()->saveState();
                QDataStream privateStream(privateState);
                qint32 marker = 0, version = 0;
                QStringList privateTabs;
                privateStream >> marker >> version >> privateTabs;
                sessionCheck(privateTabs.isEmpty(),
                             "private window serializes zero tabs");
            }
            application.saveSession();
            sessionCheck(
                QSettings().value(QLatin1String("sessions/lastSession"))
                    .isNull(),
                "saveSession while private writes nothing");
            privateWindow->close();
            BrowserApplication::setPrivate(false);

            QObject::connect(&application, &QCoreApplication::aboutToQuit,
                             &application,
                             [sessionCheck, fixtureUrls,
                              &sessionFailures]() {
                // Parse the blob the user's window-close just wrote:
                // magic, version, window count, then per-window states
                // carrying a tab state of url list + current index.
                const QByteArray blob =
                    QSettings()
                        .value(QLatin1String("sessions/lastSession"))
                        .toByteArray();
                QDataStream stream(blob);
                qint32 marker = 0, version = 0, windowCount = 0;
                stream >> marker >> version >> windowCount;
                sessionCheck(marker == 0xec && version == 2
                                 && windowCount == 1,
                             "session blob header");
                QStringList restoredUrls;
                qint32 restoredCurrent = -1;
                for (qint32 i = 0; i < windowCount; ++i) {
                    QByteArray windowState;
                    stream >> windowState;
                    QDataStream windowStream(windowState);
                    qint32 wmarker = 0, wversion = 0;
                    QSize size;
                    bool b1 = false, b2 = false, b3 = false;
                    QByteArray tabState;
                    windowStream >> wmarker >> wversion >> size
                        >> b1 >> b2 >> b3 >> tabState;
                    QDataStream tabStream(tabState);
                    qint32 tmarker = 0, tversion = 0;
                    tabStream >> tmarker >> tversion
                        >> restoredUrls >> restoredCurrent;
                    sessionCheck(tmarker == 0xaa && tversion == 1,
                                 "tab-state blob header");
                }
                sessionCheck(restoredUrls == fixtureUrls,
                             "session blob tab urls in order");
                sessionCheck(restoredCurrent == 1,
                             "session blob current index");
                if (sessionFailures)
                    qInfo() << "session-smoke: FAIL";
            });

            // Close the real window once the loads have settled; the
            // quit path saves the session, then lastWindowClosed quits.
            QTimer::singleShot(2500, &application, [browserWindow]() {
                browserWindow->close();
            });
            QTimer::singleShot(30000, &application, [&application]() {
                qInfo() << "session-smoke: FAIL (save phase timeout)";
                fflush(nullptr);
                std::quick_exit(1);
            });
            const int rc = application.exec();
            fflush(nullptr);
            std::quick_exit(sessionFailures ? 1 : rc);
        }

        if (sessionRestoreSmoke) {
            QSettings().setValue(QLatin1String("MainWindow/startupBehavior"), 2);
            BrowserMainWindow *browserWindow = application.newMainWindow();

            QTimer *poll = new QTimer(&application);
            auto ticks = std::make_shared<int>(0);
            QObject::connect(poll, &QTimer::timeout, &application,
                             [sessionCheck, &sessionFailures,
                              browserWindow, fixtureUrls, ticks, poll]() {
                TabWidget *tabWidget = browserWindow->tabWidget();
                QStringList got;
                bool titlesOk = true;
                for (int i = 0; i < tabWidget->count(); ++i) {
                    WebView *tab = tabWidget->webView(i);
                    if (!tab)
                        continue;
                    got << tab->url().toString();
                    // A fixture title proves the tab actually loaded,
                    // not just that its url was scheduled.
                    titlesOk &= tab->title().contains(
                        QStringLiteral("arora-session-%1").arg(i + 1));
                }
                if (got == fixtureUrls && tabWidget->currentIndex() == 1
                    && titlesOk) {
                    poll->stop();
                    sessionCheck(true, "restored tab urls in order");
                    sessionCheck(true, "restored current index");
                    sessionCheck(true, "restored tabs loaded");
                    qInfo() << "session-smoke: restore"
                            << (sessionFailures ? "FAIL" : "PASS");
                    fflush(nullptr);
                    std::quick_exit(sessionFailures ? 1 : 0);
                }
                if (++*ticks > 100) {
                    poll->stop();
                    sessionCheck(false, "restore completed");
                    qInfo() << "  got urls:" << got
                            << "index:" << tabWidget->currentIndex()
                            << "urlsMatch:" << (got == fixtureUrls)
                            << "titlesOk:" << titlesOk;
                    fflush(nullptr);
                    std::quick_exit(1);
                }
            });
            poll->start(200);
            return application.exec();
        }
    }

    // Headless measurement for PERF01 — report-only timings for the
    // three audited hot paths: cold start, tab-open latency and
    // large-history model load, plus the profile-tree permission sweep
    // (SEC12) that runs on the startup path.  Always exits 0; the
    // numbers go into task notes rather than a pass/fail gate.
    // Workload sizes are overridable through ARORA_PERF_HISTORY_N and
    // ARORA_PERF_TREE_N.
    if (args.contains(QLatin1String("--perf-smoke"))) {
        qInfo() << "perf-smoke: app-ctor" << appCtorMs << "ms"
                << "(single-instance, profile bring-up, services)";

        // Seed the on-disk history format BEFORE the first window is
        // built: the window's chrome (location-bar completer, history
        // menu) lazily pulls in HistoryManager during construction, so
        // the seed must already exist for the menu bench below to see
        // a populated history.  Oldest->newest QByteArray blocks,
        // newest-first in memory.
        const int envHistoryN = qEnvironmentVariableIntValue("ARORA_PERF_HISTORY_N");
        const int historyN = envHistoryN > 0 ? envHistoryN : 40000;
        const QString historyPath =
            BrowserPaths::dataFilePath(QLatin1String("history"));
        {
            QFile seed(historyPath);
            if (!seed.open(QIODevice::WriteOnly)) {
                qInfo() << "perf-smoke: FAIL (cannot write history seed)";
                return 1;
            }
            QDataStream out(&seed);
            const QDateTime base =
                QDateTime::currentDateTime().addDays(-7);
            for (int i = 0; i < historyN; ++i) {
                QByteArray data;
                QDataStream stream(&data, QIODevice::WriteOnly);
                // PERF03: 50-second spacing spreads the entries across
                // ~23 calendar days so the HistoryMenu bench below
                // exercises the date-folder submenu path too.
                stream << quint32(HistoryParser::Version)
                       << QStringLiteral("http://example.com/%1").arg(i)
                       << base.addSecs(i * 50)
                       << QStringLiteral("page %1").arg(i);
                out << data;
            }
        }

        // First window construction ends in the first tab; extra tabs
        // measure the steady-state tab-open path.
        const qint64 windowStart = perfTimer.elapsed();
        BrowserMainWindow *perfWindow = application.newMainWindow();
        const qint64 windowMs = perfTimer.elapsed() - windowStart;
        qInfo() << "perf-smoke: first-window" << windowMs << "ms";

        const int extraTabs = 8;
        TabWidget *perfTabs = perfWindow->tabWidget();
        const qint64 tabsStart = perfTimer.elapsed();
        for (int i = 0; i < extraTabs; ++i)
            perfTabs->makeNewTab(false);
        const qint64 tabsMs = perfTimer.elapsed() - tabsStart;
        qInfo() << "perf-smoke: new-tab avg"
                << qRound(tabsMs / double(extraTabs) * 10) / 10.0
                << "ms over" << extraTabs << "tabs";

        // Large-history load (seeded above, before the window pulled
        // the shared manager in): measure a fresh manager's parse and
        // its two lazy model warmups, plus the per-visit prepend cost
        // at scale.
        QElapsedTimer step;
        step.start();
        HistoryManager bench;
        const qint64 historyCtorMs = step.elapsed();
        step.restart();
        bench.historyFilterModel()->rowCount();
        const qint64 historyFilterMs = step.elapsed();
        step.restart();
        bench.historyTreeModel()->rowCount(QModelIndex());
        const qint64 historyTreeMs = step.elapsed();
        step.restart();
        bench.addHistoryEntry(QStringLiteral("http://example.com/new"));
        const qint64 historyAddMs = step.elapsed();
        qInfo() << "perf-smoke: history" << historyN << "entries —"
                << "load" << historyCtorMs << "ms,"
                << "filter-model" << historyFilterMs << "ms,"
                << "tree-model" << historyTreeMs << "ms,"
                << "add-entry" << historyAddMs << "ms";

        // PERF03: menu-open latency on a populated history — the
        // HistoryMenu walks the shared manager's tree model on show.
        // Previously every open rebuilt all actions; with the dirty
        // flag repeats are O(1) while the model is unchanged.
        step.restart();
        HistoryMenu historyMenu;
        emit static_cast<QMenu *>(&historyMenu)->aboutToShow();
        const qint64 menuFirstMs = step.elapsed();
        step.restart();
        emit static_cast<QMenu *>(&historyMenu)->aboutToShow();
        const qint64 menuSecondMs = step.elapsed();
        step.restart();
        emit static_cast<QMenu *>(&historyMenu)->aboutToShow();
        const qint64 menuThirdMs = step.elapsed();
        qInfo() << "perf-smoke: history-menu" << historyN << "entries —"
                << "first open" << menuFirstMs << "ms,"
                << "reopen" << menuSecondMs << "ms,"
                << "reopen" << menuThirdMs << "ms";

        // Worst case: a flat model the size of a large bookmark tree —
        // before PERF03 every open rebuilt all 10000 actions.
        QStandardItemModel flatModel;
        for (int i = 0; i < 10000; ++i)
            flatModel.appendRow(new QStandardItem(
                QStringLiteral("entry %1").arg(i)));
        ModelMenu flatMenu;
        flatMenu.setModel(&flatModel);
        step.restart();
        emit static_cast<QMenu *>(&flatMenu)->aboutToShow();
        const qint64 flatFirstMs = step.elapsed();
        step.restart();
        emit static_cast<QMenu *>(&flatMenu)->aboutToShow();
        const qint64 flatSecondMs = step.elapsed();
        qInfo() << "perf-smoke: flat-menu 10000 rows —"
                << "first open" << flatFirstMs << "ms,"
                << "reopen" << flatSecondMs << "ms";

        // SEC12 profile-tree sweep: recursive owner-only enforcement
        // runs on the startup path (applySettings) — measure it on a
        // synthetic tree.  Files are created with the default umask so
        // the first pass repairs and the second verifies.
        const int envTreeN = qEnvironmentVariableIntValue("ARORA_PERF_TREE_N");
        const int treeN = envTreeN > 0 ? envTreeN : 5000;
        QTemporaryDir tree(QDir::temp().filePath(
            QLatin1String("arora-perf-XXXXXX")));
        if (!tree.isValid()) {
            qInfo() << "perf-smoke: FAIL (cannot create tree dir)";
            return 1;
        }
        for (int i = 0; i < treeN; ++i) {
            QDir().mkpath(tree.path() + QStringLiteral("/d%1")
                          .arg(i % 25));
            QFile file(tree.path() + QStringLiteral("/d%1/f%2")
                       .arg(i % 25).arg(i));
            if (file.open(QIODevice::WriteOnly))
                file.close();
        }
        step.restart();
        BrowserProfile::ensureUserOnlyPermissions(tree.path());
        const qint64 treeFirstMs = step.elapsed();
        step.restart();
        BrowserProfile::ensureUserOnlyPermissions(tree.path());
        const qint64 treeSecondMs = step.elapsed();
        qInfo() << "perf-smoke: perm-sweep" << treeN << "files —"
                << "first" << treeFirstMs << "ms,"
                << "second" << treeSecondMs << "ms";

        // Give the WebEngine child one event-loop spin so teardown
        // follows the normal path.
        QTimer::singleShot(0, &application,
                           [&application]() { application.exit(0); });
        return application.exec();
    }

    // Headless verification for TOR01: the TorManager daemon layer —
    // resolve a tor binary, spawn it with a scratch DataDirectory, wait
    // for bootstrap over the real tor control protocol (cookie auth,
    // TAKEOWNERSHIP, STATUS_CLIENT events), prove the SOCKS5 listener
    // speaks the protocol against a local echo target, and check the
    // daemon is reaped by stop().  Exits 0 on PASS.  Bootstrap needs
    // real network access to the tor directory authorities.
    if (args.contains(QLatin1String("--tor-smoke"))) {
        auto torFail = [&application](const QString &why) {
            qInfo() << "tor-smoke: FAIL" << why;
            application.exit(1);
        };

        const QString binary = TorManager::resolveBinary();
        if (binary.isEmpty()) {
            torFail(QLatin1String("no tor binary — set ARORA_TOR_BINARY "
                                  "or run BuildProcess/fetch-tor.sh"));
            return application.exec();
        }
        qInfo() << "tor-smoke: using" << binary;

        QTemporaryDir torDir(QDir::temp().filePath(
            QLatin1String("arora-tor-smoke-XXXXXX")));
        if (!torDir.isValid()) {
            torFail(QLatin1String("cannot create data directory"));
            return application.exec();
        }

        // Local echo target for the SOCKS5 CONNECT — exits cannot
        // reach 127.0.0.1, so a well-formed refusal reply already
        // proves the listener speaks the protocol.
        QTcpServer *echoServer = new QTcpServer(&application);
        if (!echoServer->listen(QHostAddress::LocalHost, 0)) {
            torFail(QLatin1String("echo server failed to listen"));
            return application.exec();
        }
        QObject::connect(echoServer, &QTcpServer::newConnection,
                         &application, [echoServer]() {
            QTcpSocket *client = echoServer->nextPendingConnection();
            QObject::connect(client, &QTcpSocket::readyRead, client,
                             [client]() {
                client->write(client->readAll());
            });
        });
        const quint16 echoPort = echoServer->serverPort();

        TorManager *torManager = new TorManager(&application);
        torManager->setDataDirectory(torDir.path());
        // Log tor's own notices so bootstrap stalls are diagnosable.
        QObject::connect(torManager, &TorManager::logLine,
                         &application, [](const QString &line) {
            qInfo() << "tor:" << line;
        });
        QObject::connect(torManager, &TorManager::bootstrapProgressChanged,
                         &application,
                         [](int progress, const QString &summary) {
            qInfo() << "tor-smoke: bootstrap" << progress
                    << "%" << summary;
        });
        QObject::connect(torManager, &TorManager::failed,
                         &application, torFail);
        QObject::connect(torManager, &TorManager::ready,
                         &application,
                         [torManager, echoPort, &application,
                          torFail](const QNetworkProxy &proxy) {
            qInfo() << "tor-smoke: ready — socks"
                    << proxy.hostName() << "port" << proxy.port();
            TorSocks5 *probe = new TorSocks5(&application);
            QObject::connect(probe, &TorSocks5::finished,
                             &application,
                             [torManager, &application, torFail](
                                 bool granted, int replyCode) {
                if (replyCode < 0) {
                    torFail(QLatin1String("socks5 handshake failed"));
                    return;
                }
                qInfo() << "tor-smoke: socks5 CONNECT reply"
                        << replyCode
                        << (granted
                            ? QStringLiteral("(tunnel granted)")
                            : QStringLiteral("(well-formed refusal — "
                                             "expected for a loopback "
                                             "target)"));
                torManager->stop();
                const bool reaped = !torManager->isRunning()
                    && torManager->state() == TorManager::Stopped;
                qInfo() << "tor-smoke:" << (reaped ? "PASS" : "FAIL")
                        << "daemon reaped:" << reaped;
                application.exit(reaped ? 0 : 1);
            });
            probe->connectThrough(QHostAddress::LocalHost,
                                  torManager->socksPort(),
                                  QStringLiteral("127.0.0.1"),
                                  echoPort);
        });
        // Bootstrap over the real network can take a while.
        QTimer::singleShot(240000, &application, [torFail]() {
            torFail(QLatin1String("timeout waiting for bootstrap"));
        });
        torManager->start();
    }

    // Headless verification for TOR02: the tor-window process model —
    // fail-closed proxy until bootstrap, process-global SOCKS5 routing
    // once ready, dedicated off-the-record profile, and real traffic
    // provably exiting a tor relay.  Exits 0 on PASS.  Requires the
    // real daemon (bootstrap) and outbound connectivity
    // (check.torproject.org/api/ip answers IsTor only for exit
    // relays).
    if (args.contains(QLatin1String("--tor-window-smoke"))) {
        auto torWinFail = [&application](const QString &why) {
            qInfo() << "tor-window-smoke: FAIL" << why;
            application.exit(1);
        };

        TorManager *torManager = application.torManager();
        if (!torManager) {
            torWinFail(QLatin1String("no TorManager — tor mode not armed"));
            return application.exec();
        }

        // Fail-closed: before the daemon reports its listener the
        // application proxy must be the dead loopback SOCKS port.
        const QNetworkProxy early = QNetworkProxy::applicationProxy();
        if (early.type() != QNetworkProxy::Socks5Proxy
                || early.hostName() != QLatin1String("127.0.0.1")
                || early.port() != 1) {
            torWinFail(QLatin1String("application proxy not fail-closed at start"));
            return application.exec();
        }
        qInfo() << "tor-window-smoke: fail-closed proxy armed";

        // The browsing profile must be the dedicated OTR tor profile,
        // not the named persistent one.
        if (!BrowserApplication::webEngineProfile()->isOffTheRecord()) {
            torWinFail(QLatin1String("tor profile is not off-the-record"));
            return application.exec();
        }
        if (!BrowserApplication::isPrivate()) {
            torWinFail(QLatin1String("tor mode does not imply private"));
            return application.exec();
        }

        // The daemon was already started in the application
        // constructor — a missing binary or early failure may have
        // fired failed() before these connects.
        if (torManager->state() == TorManager::Failed) {
            torWinFail(torManager->errorString());
            return application.exec();
        }

        const QUrl probeUrl(QStringLiteral("https://check.torproject.org/api/ip"));
        QObject::connect(torManager, &TorManager::failed,
                         &application, torWinFail);
        QObject::connect(torManager, &TorManager::logLine,
                         &application, [](const QString &line) {
            qInfo() << "tor:" << line;
        });
        auto onReady = [torManager, &application, networkAccessManager,
                        view, probeUrl, torWinFail](const QNetworkProxy &) {
            const QNetworkProxy applied = QNetworkProxy::applicationProxy();
            if (applied.type() != QNetworkProxy::Socks5Proxy
                    || applied.port() != torManager->socksPort()) {
                torWinFail(QLatin1String("application proxy not the tor socks listener"));
                return;
            }
            qInfo() << "tor-window-smoke: routed via socks"
                    << applied.hostName() << applied.port();

            // Proof 1 — the app-side fetch manager exits a tor relay.
            QNetworkReply *reply = networkAccessManager->get(
                QNetworkRequest(probeUrl));
            QObject::connect(reply, &QNetworkReply::finished, &application,
                             [&application, reply, view, probeUrl,
                              torWinFail]() {
                const QByteArray body = reply->readAll();
                qInfo() << "tor-window-smoke: api/ip via NAM:"
                        << reply->error() << body.left(120);
                if (reply->error() != QNetworkReply::NoError
                        || !body.contains("\"IsTor\":true")) {
                    torWinFail(QLatin1String(
                        "NAM request did not exit via tor"));
                    return;
                }

                // Proof 2 — the same through the WebEngine stack.
                // api/ip replies application/json, which Chromium
                // renders into a shadow-DOM viewer (empty toPlainText),
                // so navigate the ordinary HTML page and read the API
                // via same-origin fetch — this still exercises the
                // profile's network stack for both document and XHR.
                const QUrl pageUrl(QStringLiteral(
                    "https://check.torproject.org/"));
                QObject::connect(view, &QWebEngineView::loadFinished,
                                 &application,
                                 [&application, view, pageUrl, probeUrl,
                                  torWinFail](bool ok) {
                    if (!ok || view->url() != pageUrl)
                        return;
                    view->page()->runJavaScript(
                        QStringLiteral(
                            "(function(){"
                            "var x=new XMLHttpRequest();"
                            "x.open('GET','/api/ip',false);"
                            "try{x.send(null);return x.responseText;}"
                            "catch(e){return 'XHRERR:'+e+' :: '"
                            "+document.documentElement.innerText"
                            ".slice(0,300);}})()"),
                        [&application, pageUrl, probeUrl, torWinFail](
                            const QVariant &result) {
                        const QString body = result.toString();
                        qInfo() << "tor-window-smoke: api/ip via WebEngine:"
                                << body.left(300);
                        // The api/ip JSON is the proof; the page's own
                        // verdict text is acceptable corroboration.
                        const bool isTor =
                            body.contains(QLatin1String("\"IsTor\":true"))
                            || body.contains(QLatin1String(
                                "configured to use Tor"));
                        const bool historyLeaked =
                            HistoryManager::instance()->historyContains(
                                pageUrl.toString())
                            || HistoryManager::instance()->historyContains(
                                probeUrl.toString());
                        if (!isTor) {
                            torWinFail(QLatin1String(
                                "page load did not exit via tor"));
                            return;
                        }
                        if (historyLeaked) {
                            torWinFail(QLatin1String(
                                "tor page recorded in history"));
                            return;
                        }
                        qInfo() << "tor-window-smoke: PASS"
                                << "(IsTor:true, OTR profile, fail-closed arm)";
                        application.exit(0);
                    });
                });
                view->loadUrl(pageUrl);
            });
        };
        QObject::connect(torManager, &TorManager::ready,
                         &application, onReady);
        if (torManager->isReady())
            onReady(QNetworkProxy());
        // Bootstrap + two tor round-trips can take a while.
        QTimer::singleShot(240000, &application, [torWinFail]() {
            torWinFail(QLatin1String("timeout waiting for bootstrap/probe"));
        });
    }

    // Headless verification for ICONS01: each bundled set must render
    // every registered icon name, a theme switch must reach already
    // created icons, the -dark recolor variants must engage under a
    // dark palette, and the 2009 fallback art must resolve when the
    // active theme misses a name.  Fully synchronous — no event loop.
    if (args.contains(QLatin1String("--icons-smoke"))) {
        int failures = 0;
        auto check = [&failures](const QString &what, bool ok) {
            qInfo() << "icons-smoke:" << what << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++failures;
        };
        auto opaquePixels = [](const QImage &image) {
            int count = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y).alpha() > 40)
                        ++count;
            return count;
        };

        const QStringList names = AroraIcon::names();
        check(QLatin1String("name count"), names.count() >= 30);
        const QStringList sets = { QLatin1String("adwaita"),
                                   QLatin1String("breeze"),
                                   QLatin1String("tabler") };
        for (const QString &set : sets) {
            AroraIcon::setTheme(set);
            int missing = 0;
            for (const QString &name : names) {
                const QImage image = AroraIcon::get(name)
                    .pixmap(QSize(24, 24)).toImage();
                if (image.isNull() || opaquePixels(image) == 0) {
                    qInfo() << "icons-smoke: missing" << set << name;
                    ++missing;
                }
            }
            check(set + QLatin1String(" coverage"), missing == 0);
        }

        // Live apply: an existing QIcon follows setThemeName — that
        // is what makes the Settings combo need no widget rewiring.
        AroraIcon::setTheme(QLatin1String("adwaita"));
        const QIcon live = AroraIcon::get(QLatin1String("go-previous"));
        const QImage before = live.pixmap(QSize(24, 24)).toImage();
        AroraIcon::setTheme(QLatin1String("tabler"));
        check(QLatin1String("live switch"),
              before != live.pixmap(QSize(24, 24)).toImage());

        // A dark palette selects the pre-recolored -dark variant (Qt
        // renders currentColor black, it does not palette-recolor).
        const QPalette savedPalette = qApp->palette();
        qApp->setPalette(BrowserTheme::darkPalette());
        check(QLatin1String("dark variant selection"),
              AroraIcon::effectiveThemeName(QLatin1String("tabler"))
                  == QLatin1String("arora-tabler-dark"));
        AroraIcon::setTheme(QLatin1String("tabler"));
        const QImage darkImage = AroraIcon::get(QLatin1String("go-previous"))
            .pixmap(QSize(24, 24)).toImage();
        qlonglong luminance = 0;
        int opaque = 0;
        for (int y = 0; y < darkImage.height(); ++y) {
            for (int x = 0; x < darkImage.width(); ++x) {
                const QColor color = darkImage.pixelColor(x, y);
                if (color.alpha() > 128) {
                    luminance += qGray(color.rgb());
                    ++opaque;
                }
            }
        }
        check(QLatin1String("dark glyphs light"),
              opaque > 0 && luminance / opaque > 140);
        qApp->setPalette(savedPalette);

        // The original 2009 artwork is the last resort: under a theme
        // that does not exist at all, legacy names still resolve, and
        // names missing there keep falling through to the bundled
        // Adwaita inheritance.
        QIcon::setThemeName(QLatin1String("arora-does-not-exist"));
        check(QLatin1String("legacy fallback"),
              !AroraIcon::get(QLatin1String("tab-new"))
                   .pixmap(QSize(16, 16)).isNull());
        check(QLatin1String("inherited fallback"),
              !AroraIcon::get(QLatin1String("go-previous"))
                   .pixmap(QSize(16, 16)).isNull());

        AroraIcon::setTheme(AroraIcon::theme());
        qInfo() << "icons-smoke:" << (failures == 0 ? "PASS" : "FAIL")
                << failures << "failures";
        return failures == 0 ? 0 : 1;
    }

    // Headless verification for PRIV02 — two runs sharing the
    // test-mode settings store.
    //
    // `--fingerprint-smoke` (parent): the QSettings round-trip, the
    // Accept-Language override path (stored list untouched, wire
    // value normalized on both the profile and the app-side NAM), the
    // TZ env save/restore and the UA-consistency surface are checked
    // in-process; then a child is spawned with the toggles persisted
    // because the page-observable surface needs them active BEFORE
    // the engine initializes.
    //
    // `--fingerprint-child-smoke` (child): boots with the toggles
    // already persisted, loads a local fixture page and asserts what
    // JavaScript sees — Intl timezone, navigator.languages, UA/client
    // hints consistency — plus the Accept-Language header on the
    // wire.  Exits 0 on PASS.
    if (args.contains(QLatin1String("--fingerprint-smoke"))) {
        int failures = 0;
        const auto check = [&failures](bool ok, const char *what) {
            qInfo() << "fingerprint-smoke:" << what
                    << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++failures;
        };

        QSettings settings;
        const QVariant savedUtc =
            settings.value(QLatin1String("privacy/reportUtcTimezone"));
        const QVariant savedLang =
            settings.value(QLatin1String("privacy/normalizeAcceptLanguage"));
        const QVariant savedAccept =
            settings.value(QLatin1String("network/acceptLanguages"));

        // (b) Accept-Language normalization — a distinctive stored
        // list proves the toggle replaces it rather than appending or
        // reading the locale.
        settings.setValue(QLatin1String("network/acceptLanguages"),
                          QStringList{QLatin1String("Klingon [tlh]")});
        settings.setValue(QLatin1String("privacy/normalizeAcceptLanguage"),
                          false);
        check(AcceptLanguageDialog::acceptLanguages()
                  == QStringList{QLatin1String("Klingon [tlh]")},
              "toggle off keeps the configured list");
        settings.setValue(QLatin1String("privacy/normalizeAcceptLanguage"),
                          true);
        check(AcceptLanguageDialog::acceptLanguages()
                  == AcceptLanguageDialog::normalizedAcceptLanguages(),
              "toggle on normalizes the list");

        const QByteArray normalizedHeader = AcceptLanguageDialog::httpString(
            AcceptLanguageDialog::normalizedAcceptLanguages());
        check(normalizedHeader == "en-US, en;q=0.9",
              "normalized header string");
        BrowserProfile::applySettings(profile);
        check(profile->httpAcceptLanguage()
                  == QString::fromUtf8(normalizedHeader),
              "profile sends normalized Accept-Language");
        // The app-side NAM's wire header is verified in the child run
        // (its /nam request is captured by the fixture server).

        // (a) TZ env round-trip — the save/restore path that
        // SettingsDialog::saveToSettings() exercises mid-session.
        const bool tzWasSet = qEnvironmentVariableIsSet("TZ");
        const QByteArray tzBefore = qgetenv("TZ");
        settings.setValue(QLatin1String("privacy/reportUtcTimezone"), true);
        BrowserProfile::applyFingerprintEnvironment();
        check(qgetenv("TZ") == "UTC", "TZ forced to UTC");
        settings.setValue(QLatin1String("privacy/reportUtcTimezone"), false);
        BrowserProfile::applyFingerprintEnvironment();
        check(qgetenv("TZ") == tzBefore && qEnvironmentVariableIsSet("TZ") == tzWasSet,
              "TZ restored on disable");

        // (c) UA consistency — the identity the wire and navigator.*
        // expose must tell the same story: vanilla Chrome UA, no
        // QtWebEngine token, client-hints brands agreeing.
        check(profile->httpUserAgent()
                  == BrowserProfile::defaultHttpUserAgent(),
              "profile UA is the vanilla Chrome UA");
        check(!profile->httpUserAgent().contains(QLatin1String("QtWebEngine")),
              "no QtWebEngine token");
        const QVariantMap brands = profile->clientHints()->fullVersionList();
        check(!brands.value(QLatin1String("Google Chrome")).toString().isEmpty()
              && brands.value(QLatin1String("Google Chrome"))
                     == brands.value(QLatin1String("Chromium")),
              "client hints brands agree with the UA");

        // Seed the toggles for the child run — it must see them in the
        // settings store before its engine initializes.
        settings.setValue(QLatin1String("privacy/reportUtcTimezone"), true);
        settings.setValue(QLatin1String("privacy/normalizeAcceptLanguage"), true);
        settings.sync();

        QProcess child;
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    QStringList{QStringLiteral("--fingerprint-child-smoke")});
        const bool childOk = child.waitForFinished(120000)
            && child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == 0;
        check(childOk, "child run: normalized surface visible to JS");

        const auto restore = [&settings](const QString &key,
                                         const QVariant &saved) {
            if (saved.isValid())
                settings.setValue(key, saved);
            else
                settings.remove(key);
        };
        restore(QLatin1String("privacy/reportUtcTimezone"), savedUtc);
        restore(QLatin1String("privacy/normalizeAcceptLanguage"), savedLang);
        restore(QLatin1String("network/acceptLanguages"), savedAccept);
        BrowserProfile::applyFingerprintEnvironment();

        qInfo() << "fingerprint-smoke:" << (failures == 0 ? "PASS" : "FAIL")
                << failures << "failures";
        return failures == 0 ? 0 : 1;
    }

    if (args.contains(QLatin1String("--fingerprint-child-smoke"))) {
        int failures = 0;
        const auto check = [&failures](bool ok, const QString &what) {
            qInfo() << "fingerprint-smoke(child):" << what
                    << (ok ? "PASS" : "FAIL");
            if (!ok)
                ++failures;
        };

        check(qgetenv("TZ") == "UTC",
              QLatin1String("TZ env forced pre-engine"));
        check(profile->httpAcceptLanguage()
                  .startsWith(QLatin1String("en-US")),
              QLatin1String("profile accept-language normalized"));

        QTcpServer *server = new QTcpServer(&application);
        if (!server->listen(QHostAddress::LocalHost)) {
            qInfo() << "fingerprint-smoke(child): FAIL (listen)"
                    << server->errorString();
            return 1;
        }
        auto capturedLang = std::make_shared<QHash<QString, QByteArray>>();
        QObject::connect(server, &QTcpServer::newConnection, &application,
                         [server, capturedLang]() {
            QTcpSocket *client = server->nextPendingConnection();
            QObject::connect(client, &QTcpSocket::readyRead, client,
                             [client, capturedLang]() {
                const QByteArray request = client->readAll();
                QString path;
                const QList<QByteArray> lines = request.split('\n');
                for (const QByteArray &line : lines) {
                    if (line.startsWith("GET ")) {
                        path = QString::fromLatin1(
                            line.mid(4, line.indexOf(" HTTP/") - 4).trimmed());
                    } else if (line.startsWith("Accept-Language: ")) {
                        capturedLang->insert(path,
                                             line.mid(17).trimmed());
                    }
                }
                const QByteArray body =
                    "<html><head><title>fingerprint-child</title>"
                    "</head><body>probe</body></html>";
                client->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: " + QByteArray::number(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body);
                client->disconnectFromHost();
            });
        });

        auto done = std::make_shared<bool>(false);
        auto finish = [&application, &failures, done](bool ok) {
            if (*done)
                return;
            *done = true;
            qInfo() << "fingerprint-smoke(child):"
                    << (ok && failures == 0 ? "PASS" : "FAIL")
                    << failures << "failures";
            application.exit(ok && failures == 0 ? 0 : 1);
        };

        QObject::connect(view, &QWebEngineView::loadFinished, &application,
                         [&application, view, profile, server, check,
                          capturedLang, finish, networkAccessManager](
                             bool ok) {
            if (!ok || view->url().isEmpty()
                || view->url().scheme() != QLatin1String("http"))
                return;
            view->page()->runJavaScript(QStringLiteral(
                "JSON.stringify({"
                "tz: Intl.DateTimeFormat().resolvedOptions().timeZone,"
                "offset: new Date().getTimezoneOffset(),"
                "langs: navigator.languages,"
                "lang: navigator.language,"
                "ua: navigator.userAgent,"
                "platform: navigator.platform,"
                "brands: (navigator.userAgentData"
                " ? navigator.userAgentData.brands : null),"
                "webdriver: !!navigator.webdriver})"),
                [&application, profile, server, check, capturedLang, finish,
                 networkAccessManager](const QVariant &result) {
                const QJsonObject probe = QJsonDocument::fromJson(
                    result.toString().toUtf8()).object();
                qInfo() << "fingerprint-smoke(child): page reports"
                        << result.toString();

                const QString tz = probe.value(QLatin1String("tz")).toString();
                check(tz == QLatin1String("UTC")
                          || tz == QLatin1String("Etc/UTC")
                          || tz == QLatin1String("GMT")
                          || tz == QLatin1String("Etc/GMT"),
                      QStringLiteral("JS timezone is UTC (got %1)")
                          .arg(tz));
                check(probe.value(QLatin1String("offset")).toInt(-1) == 0,
                      QLatin1String("Date offset is 0"));

                QStringList langs;
                const QJsonArray langArray =
                    probe.value(QLatin1String("langs")).toArray();
                for (const QJsonValue &v : langArray)
                    langs << v.toString();
                check(langs == QStringList{QLatin1String("en-US"),
                                           QLatin1String("en")},
                      QStringLiteral("navigator.languages normalized (got %1)")
                          .arg(langs.join(QLatin1Char(','))));
                check(probe.value(QLatin1String("lang")).toString()
                          == QLatin1String("en-US"),
                      QLatin1String("navigator.language is en-US"));

                const QString ua =
                    probe.value(QLatin1String("ua")).toString();
                check(ua == profile->httpUserAgent()
                          && !ua.contains(QLatin1String("QtWebEngine")),
                      QLatin1String("navigator.userAgent matches the wire UA"));
                check(probe.value(QLatin1String("platform")).toString()
                          .contains(QLatin1String("Linux")),
                      QLatin1String("navigator.platform consistent with UA"));
                bool chromeBrand = false;
                const QString presentedMajor = QString::number(
                    BrowserProfile::presentedChromeMajor());
                const QJsonArray brandArray =
                    probe.value(QLatin1String("brands")).toArray();
                for (const QJsonValue &v : brandArray) {
                    const QJsonObject brand = v.toObject();
                    const QString name =
                        brand.value(QLatin1String("brand")).toString();
                    if (name == QLatin1String("Google Chrome"))
                        chromeBrand = true;
                    // UA03: a version-bearing brand must not
                    // contradict the presented UA milestone — a
                    // UA/brand split is itself a fingerprint.
                    if ((name == QLatin1String("Google Chrome")
                         || name == QLatin1String("Chromium"))) {
                        check(brand.value(QLatin1String("version")).toString()
                                  == presentedMajor,
                              QStringLiteral("%1 brand presents major %2 "
                                             "(got %3)")
                                  .arg(name, presentedMajor,
                                       brand.value(QLatin1String("version"))
                                           .toString()));
                    }
                }
                check(chromeBrand,
                      QLatin1String("userAgentData brands carry Google Chrome"));
                check(!probe.value(QLatin1String("webdriver")).toBool(true),
                      QLatin1String("navigator.webdriver off"));

                const QString wireLang =
                    QString::fromUtf8(capturedLang->value(
                        QLatin1String("/page")))
                        .remove(QLatin1Char(' '));
                check(wireLang == QLatin1String("en-US,en;q=0.9"),
                      QStringLiteral("wire Accept-Language normalized (got %1)")
                          .arg(QString::fromUtf8(
                              capturedLang->value(QLatin1String("/page")))));

                // The app-side fetch manager must tell the same story —
                // a configured list leaking through NAM headers would
                // defeat the normalization on suggestions/downloads.
                QNetworkReply *reply = networkAccessManager->get(
                    QNetworkRequest(QUrl(QStringLiteral(
                        "http://127.0.0.1:%1/nam")
                        .arg(server->serverPort()))));
                QObject::connect(reply, &QNetworkReply::finished,
                                 &application,
                                 [reply, check, capturedLang, finish]() {
                    reply->deleteLater();
                    const QString namLang =
                        QString::fromUtf8(capturedLang->value(
                            QLatin1String("/nam")))
                            .remove(QLatin1Char(' '));
                    check(reply->error() == QNetworkReply::NoError
                              && namLang == QLatin1String("en-US,en;q=0.9"),
                          QStringLiteral("NAM Accept-Language normalized (got %1)")
                              .arg(namLang));
                    finish(true);
                });
            });
        });

        QTimer::singleShot(30000, &application, [finish]() {
            finish(false);
        });
        view->loadUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/page")
                           .arg(server->serverPort())));
        return application.exec();
    }

    // Headless verification for TELEM01: a clean launch must produce
    // zero non-user-initiated connections.  Both network stacks were
    // pointed at a loopback capture proxy — Chromium through the
    // --proxy-server flag set before the engine latched its flags, and
    // the app-side fetch manager through the proxy settings written
    // next to it — and the capture socket has been accepting since
    // before the application constructor, so even ctor-early fetchers
    // arrive as a logged CONNECT/GET.  On Linux a /proc/net poll
    // additionally catches process-owned sockets that bypass proxies
    // (QUIC, DNS).  PASS = nothing observed for the whole watch window.
    if (telemetrySmoke) {
        auto socketHits = std::make_shared<QStringList>();
        if (telemetryProxyPort == 0)
            qWarning() << "telemetry-smoke: capture proxy unavailable"
                          " — only the socket scan is active";

        // The app-side fetch manager picked the capture proxy up from
        // the settings written before the application ctor — reload so
        // a stale persisted proxy value can't leak into the watch
        // window either.
        networkAccessManager->loadSettings();

#if defined(Q_OS_LINUX)
        QTimer *socketPoll = new QTimer(&application);
        socketPoll->setInterval(250);
        QObject::connect(socketPoll, &QTimer::timeout, &application,
                         [socketHits]() {
            const QSet<QString> endpoints = processRemoteEndpoints();
            for (const QString &endpoint : endpoints) {
                if (!socketHits->contains(endpoint)) {
                    socketHits->append(endpoint);
                    qInfo() << "telemetry-smoke: process socket:"
                            << endpoint;
                }
            }
        });
        socketPoll->start();
#endif

        int windowMs = qEnvironmentVariableIntValue("ARORA_TELEMETRY_MS");
        if (windowMs <= 0)
            windowMs = 30000;
        qInfo() << "telemetry-smoke: watching" << windowMs
                << "ms for unsolicited outbound connections";
        QTimer::singleShot(windowMs, &application,
                           [&application, socketHits]() {
            QStringList observed;
            {
                const QMutexLocker lock(&s_telemetryMutex);
                observed = s_telemetryHits;
            }
            const bool pass = observed.isEmpty() && socketHits->isEmpty();
            qInfo() << "telemetry-smoke:" << (pass ? "PASS" : "FAIL")
                    << "proxy attempts:" << observed
                    << "socket endpoints:" << *socketHits;
            // Undo the capture-proxy settings so a dead port does not
            // leak into later smoke runs sharing this test profile.
            {
                QSettings settings;
                settings.beginGroup(QLatin1String("proxy"));
                settings.setValue(QLatin1String("enabled"), false);
                settings.endGroup();
            }
            application.exit(pass ? 0 : 1);
        });
        return application.exec();
    }

    return application.exec();
}
