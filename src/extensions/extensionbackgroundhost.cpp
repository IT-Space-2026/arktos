/*
 * Copyright 2026 The Arora Authors
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

#include "extensionbackgroundhost.h"

#include "browserapplication.h"
#include "browserprofile.h"
#include "extensioneventhub.h"
#include "extensionmanager.h"
#include "extensionshimbackend.h"

#include <qapplication.h>
#include <qcoreapplication.h>
#include <qdebug.h>
#include <qdir.h>
#include <qevent.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qregularexpression.h>
#include <qtimer.h>
#include <qwidget.h>

#include <qwebchannel.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
#include <qwebengineview.h>

#include <QtWebEngineCore/qtwebenginecore-config.h>
#if QT_CONFIG(webengine_extensions)
#include <qwebengineextensioninfo.h>
#include <qwebengineextensionmanager.h>

#include <utility>
#endif

// ------------------------------------------------------------------
// ChromeBridge — the 'chromeBridge' object registered on each hosted
// page's QWebChannel (MainWorld — the shim and the extension's real
// background scripts share that world; AroraChannelWorld isolation is
// for untrusted web content, not an extension talking to its own
// host).  EXT14 ships the handshake + error reporting only; EXT15's
// gapfill API surface dispatches through invoke().
// ------------------------------------------------------------------

ChromeBridge::ChromeBridge(const QString &extensionId,
                           const QString &extensionName,
                           const QString &extensionPath,
                           ExtensionShimBackend *backend,
                           QObject *parent)
    : QObject(parent)
    , m_extensionId(extensionId)
    , m_extensionName(extensionName)
{
    // EXT15 backend — storage store, host-timer alarms, the live
    // tab snapshot, action state, manifest-derived answers.  Its
    // pushEvent feeds the page through the hostEvent signal below.
    // EXT17: sibling pages (popup/options) SHARE the hosted
    // background's backend — storage writes, alarms and action state
    // are per-extension, not per-context.
    m_backend = backend
        ? backend
        : new ExtensionShimBackend(extensionId, extensionName,
                                   extensionPath, this);
    connect(m_backend, &ExtensionShimBackend::pushEvent, this,
            &ChromeBridge::hostEvent);
    connect(m_backend, &ExtensionShimBackend::gapReported, this,
            &ChromeBridge::gapReported);
}

QJsonObject ChromeBridge::handshake(const QJsonObject &hello)
{
    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("host"), QStringLiteral("arora"));
    reply.insert(QStringLiteral("shim"), 2);
    reply.insert(QStringLiteral("id"), m_extensionId);
    reply.insert(QStringLiteral("name"), m_extensionName);
    // The raw manifest rides the handshake so the shim can answer
    // synchronous lookups (permissions/commands/getManifest
    // fallback/default_locale) without a blocking bridge call.
    reply.insert(QStringLiteral("manifest"),
                 m_backend->manifestJson());
    // hello carries the shim's own protocol version — kept so a
    // future shim bump can negotiate; nothing negotiated yet means
    // "any".
    reply.insert(QStringLiteral("shimVersion"),
                 hello.value(QStringLiteral("shimVersion")));
    return reply;
}

ExtensionShimBackend *ChromeBridge::backend() const
{
    return m_backend;
}

void ChromeBridge::setContextPage(QWebEnginePage *page)
{
    m_contextPage = page;
}

QWebEnginePage *ChromeBridge::contextPage() const
{
    return m_contextPage;
}

void ChromeBridge::reportError(const QString &message)
{
    emit errorReported(message.left(4096));
}

QJsonObject ChromeBridge::invoke(const QString &ns,
                                 const QString &method,
                                 const QJsonObject &args)
{
    if (qEnvironmentVariableIsSet("ARORA_EXTHOST_DEBUG"))
        qInfo() << "chromeBridge.invoke" << m_extensionId
                << ns << method << args;
    if (ns == QLatin1String("relay"))
        return invokeRelay(method, args);
    // EXT17: getCurrent is context-local — the bridge knows which
    // page asked (a popup hosted inside a real tab resolves to that
    // tab; the hidden background page is never a tab).
    if (ns == QLatin1String("tabs")
            && method == QLatin1String("getCurrent")) {
        return ExtensionShimBackend::tabForPageReply(m_contextPage);
    }
    if (!m_backend) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), false);
        o.insert(QStringLiteral("error"),
                 QStringLiteral("backend-gone"));
        return o;
    }
    return m_backend->invoke(ns, method, args);
}

// EXT15 messaging relay — the spike measured that runtime.
// sendMessage from a hosted page routes only to the real service
// worker context, never to sibling hosted/extension pages, so
// shimmed contexts fan out through the runtime's registry instead.
QJsonObject ChromeBridge::invokeRelay(const QString &method,
                                      const QJsonObject &args)
{
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    auto ok = [](const QJsonValue &v = QJsonValue()) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        if (!v.isUndefined())
            o.insert(QStringLiteral("result"), v);
        return o;
    };
    auto err = [](const QString &code) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), false);
        o.insert(QStringLiteral("error"), code);
        return o;
    };
    if (method == QLatin1String("register")) {
        const QString kind =
            args.value(QStringLiteral("kind")).toString();
        if (kind.isEmpty())
            return err(QStringLiteral("invalid-argument"));
        const QString contextId =
            runtime->registerRelayContext(
                this, kind,
                args.value(QStringLiteral("url")).toString());
        return ok(contextId);
    }
    if (method == QLatin1String("send")) {
        const QString mid =
            args.value(QStringLiteral("mid")).toString();
        const QJsonValue msg = args.value(QStringLiteral("msg"));
        if (mid.isEmpty() || msg.isUndefined())
            return err(QStringLiteral("invalid-argument"));
        const int peers = runtime->relaySend(this, mid,
                                             msg.toObject());
        return ok(QJsonObject{{QStringLiteral("peers"), peers}});
    }
    if (method == QLatin1String("respond")) {
        const QString mid =
            args.value(QStringLiteral("mid")).toString();
        const bool routed = runtime->relayRespond(
            this, mid, args.value(QStringLiteral("response")));
        return ok(routed);
    }
    if (method == QLatin1String("portConnect")) {
        const QString chan =
            args.value(QStringLiteral("chan")).toString();
        const QString portId =
            args.value(QStringLiteral("portId")).toString();
        if (chan.isEmpty() || portId.isEmpty())
            return err(QStringLiteral("invalid-argument"));
        const int peers = runtime->relayPortConnect(
            this, chan, portId,
            args.value(QStringLiteral("name")).toString(),
            args.value(QStringLiteral("senderUrl")).toString());
        return ok(QJsonObject{{QStringLiteral("peers"), peers}});
    }
    if (method == QLatin1String("portAccept")) {
        const QString chan =
            args.value(QStringLiteral("chan")).toString();
        const QString portId =
            args.value(QStringLiteral("portId")).toString();
        if (chan.isEmpty() || portId.isEmpty())
            return err(QStringLiteral("invalid-argument"));
        runtime->relayPortAccept(this, chan, portId);
        return ok();
    }
    if (method == QLatin1String("portMessage")) {
        const QString chan =
            args.value(QStringLiteral("chan")).toString();
        const QString portId =
            args.value(QStringLiteral("portId")).toString();
        if (chan.isEmpty() || portId.isEmpty())
            return err(QStringLiteral("invalid-argument"));
        const int delivered = runtime->relayPortMessage(
            this, chan, portId, args.value(QStringLiteral("msg")));
        return ok(delivered);
    }
    if (method == QLatin1String("portDisconnect")) {
        runtime->relayPortDisconnect(
            this, args.value(QStringLiteral("chan")).toString(),
            args.value(QStringLiteral("portId")).toString());
        return ok();
    }
    return err(QStringLiteral("unimplemented"));
}

// ------------------------------------------------------------------
// ExtensionBackgroundHost
// ------------------------------------------------------------------

namespace {

// QUAL03: setWebChannel binds 'chromeBridge' to the PAGE, not the
// document — every frame of every later commit exposes the same
// objects to that world.  The hosted view is therefore navigation-
// locked: the main frame may only ever be the wrapper url, and
// subframes must stay inside the extension's own origin.  Without
// this, a hosted page navigated (or iframed) to foreign content
// would hand that content the bridge — the extension's storage,
// identity, and relay channels included.
class HostedBackgroundPage : public QWebEnginePage
{
public:
    HostedBackgroundPage(QWebEngineProfile *profile,
                         const QString &extensionId,
                         QObject *parent = nullptr)
        : QWebEnginePage(profile, parent)
        , m_extensionId(extensionId)
    {
    }

protected:
    bool acceptNavigationRequest(const QUrl &url,
                                 NavigationType /*type*/,
                                 bool isMainFrame) override
    {
        if (isMainFrame)
            return url == ExtensionBackgroundHost::hostUrl(
                       m_extensionId)
                || url == QUrl(QStringLiteral("about:blank"));
        return url.scheme() == QLatin1String("chrome-extension")
            && url.host() == m_extensionId;
    }

private:
    QString m_extensionId;
};

// QUAL03: relay keys arrive inside caller-supplied arguments, so the
// "<extId>#..." prefix the shim mints is re-verified host-side rather
// than trusted — a bridge may only touch mids/chans its own
// extension owns.
bool ownsRelayKey(const ChromeBridge *bridge, const QString &key)
{
    return bridge
        && key.startsWith(bridge->extensionId() + QLatin1Char('#'));
}

// The injection runs on every page of the profile — the gate has to
// be a cheap exact match so normal browsing pays ~nothing.
constexpr int kBootWatchdogMs = 45000;
constexpr int kPollMs = 250;
constexpr int kNudgeMs = 15000;

QString resourceText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "ExtensionBackgroundHost: cannot open" << path;
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

// A JS string literal produced through the JSON encoder — quoting and
// escaping are exactly what a string needs.
QString jsLiteral(const QString &text)
{
    QString json = QString::fromUtf8(
        QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact));
    return json.mid(1, json.size() - 2);
}

// The generated per-extension injection: a cheap URL gate first,
// then the config record, the qwebchannel client, the preamble
// (src/data/exthost.js) and the EXT15 chrome.* gapfill shim
// (src/data/extshim.js).  Everything lives in one DocumentCreation
// MainWorld script so the shim lands before any extension code —
// extension pages forbid eval() under MV3 CSP, which is exactly why
// the preamble cannot be delivered by runJavaScript.
QString injectionSource(const QString &extensionId,
                        const QString &extensionName,
                        const QString &extensionPath,
                        const QStringList &scripts,
                        bool inlineCode)
{
    QJsonObject config;
    config.insert(QStringLiteral("id"), extensionId);
    config.insert(QStringLiteral("name"), extensionName);
    config.insert(QStringLiteral("inlineCode"), inlineCode);
    // EXT17: the extension's _locales map rides the config so the
    // shim's getMessage resolves synchronously — the native binding
    // answers '' for every key on this engine (blank popup chrome),
    // and page scripts call getMessage before the bridge is up.
    config.insert(QStringLiteral("i18n"),
                  ExtensionShimBackend::i18nMessages(extensionPath));
    QJsonArray list;
    for (const QString &script : scripts)
        list.append(script);
    config.insert(QStringLiteral("scripts"), list);

    QString source = QStringLiteral("(function(){")
        + QLatin1String("if(window.top!==window||location.href!==")
        + jsLiteral(ExtensionBackgroundHost::hostUrl(extensionId)
                        .toString())
        + QLatin1String(")return;window.__ARORA_HOST_CONFIG=")
        + QString::fromUtf8(
            QJsonDocument(config).toJson(QJsonDocument::Compact))
        + QLatin1Char(';');
    source += resourceText(
        QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    source += resourceText(QStringLiteral(":exthost.js"));
    source += resourceText(QStringLiteral(":extshim.js"));
    source += QLatin1String("})();");
    return source;
}

// EXT17 — sibling-page injection: the same preamble+shim payload on
// every other top-level document at the extension's own origin (the
// action popup, options_ui pages, offscreen documents).  No
// background code — cfg.kind 'page' tells exthost.js to skip the
// script loader, and the shim registers the context with the relay
// so runtime.sendMessage reaches the hosted background.
QString pageInjectionSource(const QString &extensionId,
                            const QString &extensionName,
                            const QString &extensionPath)
{
    QJsonObject config;
    config.insert(QStringLiteral("id"), extensionId);
    config.insert(QStringLiteral("name"), extensionName);
    config.insert(QStringLiteral("kind"), QStringLiteral("page"));
    config.insert(QStringLiteral("scripts"), QJsonArray());
    config.insert(QStringLiteral("i18n"),
                  ExtensionShimBackend::i18nMessages(extensionPath));

    const QString origin =
        QStringLiteral("chrome-extension://") + extensionId;
    // The EXT12 health probe measures the REAL engine worker from a
    // bare extension-origin page — it must keep pristine native
    // chrome.* bindings, so its marker fragment skips this shim.
    QString source = QStringLiteral("(function(){")
        + QLatin1String("if(window.top!==window)return;"
                        "var l=location;if(l.origin!==")
        + jsLiteral(origin)
        + QLatin1String("||l.href===")
        + jsLiteral(ExtensionBackgroundHost::hostUrl(extensionId)
                        .toString())
        + QLatin1String("||l.hash==='#arora-healthprobe'"
                        ")return;window.__ARORA_HOST_CONFIG=")
        + QString::fromUtf8(
            QJsonDocument(config).toJson(QJsonDocument::Compact))
        + QLatin1Char(';');
    source += resourceText(
        QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    source += resourceText(QStringLiteral(":exthost.js"));
    source += resourceText(QStringLiteral(":extshim.js"));
    source += QLatin1String("})();");
    return source;
}

// The extension's own background code, merged into a single injected
// script: manifest entries first, then any webpack-style lazy chunks
// ("719.background.js" beside "background.js").  Webpack lazy-loads
// those chunks through a synchronous importScripts() that a DOM page
// cannot satisfy under the MV3 script-src 'self' CSP — no eval, no
// inline.  Running the chunk source in the same synchronous pass, and
// after the entry installed its patched webpackChunk.push(), lets the
// modules register before the deferred o.e().then(o.bind()) require.
QString extensionCodeSource(const QString &extensionPath,
                            const QStringList &scripts)
{
    if (extensionPath.isEmpty())
        return QString();
    const QDir dir(extensionPath);
    QString source;
    QStringList included;
    const auto appendFile = [&](const QString &rel) {
        QFile file(dir.absoluteFilePath(rel));
        if (!file.open(QIODevice::ReadOnly))
            return;
        source += QString::fromUtf8(file.readAll());
        source += QLatin1Char('\n');
        included.append(rel);
    };
    for (const QString &script : scripts)
        appendFile(script);
    for (const QString &script : scripts) {
        const QString base = QFileInfo(script).fileName();
        const QString suffix = QLatin1Char('.') + base;
        const QString scriptDir = QFileInfo(script).path();
        const QString prefix =
            (scriptDir == QLatin1String("."))
                ? QString() : scriptDir + QLatin1Char('/');
        const QStringList entries = dir.entryList(
            QStringList{QStringLiteral("*") + suffix}, QDir::Files);
        for (const QString &entry : entries) {
            const QString head =
                entry.left(entry.size() - suffix.size());
            bool digits = !head.isEmpty();
            for (const QChar c : head)
                digits = digits && c.isDigit();
            const QString rel = prefix + entry;
            if (digits && !included.contains(rel))
                appendFile(rel);
        }
    }
    return source;
}
}

QUrl ExtensionBackgroundHost::hostUrl(const QString &extensionId)
{
    return QUrl(QStringLiteral(
        "chrome-extension://%1/manifest.json#arora-hosted")
        .arg(extensionId));
}

QString ExtensionBackgroundHost::injectionName(const QString &extensionId)
{
    return QStringLiteral("arora-exthost-") + extensionId;
}

QString ExtensionBackgroundHost::pageInjectionName(
    const QString &extensionId)
{
    return QStringLiteral("arora-extpage-") + extensionId;
}

ExtensionBackgroundHost::ExtensionBackgroundHost(
        QWebEngineProfile *profile,
        const QString &extensionId,
        const QString &extensionName,
        const QString &extensionPath,
        const QStringList &backgroundScripts,
        QObject *parent)
    : QObject(parent)
    , m_profile(profile)
    , m_extensionId(extensionId)
    , m_extensionName(extensionName)
    , m_extensionPath(extensionPath)
{
    // Manifest-order list, minus anything that is not a plain
    // relative path — a scheme/absolute/traversal entry never becomes
    // a <script src> on a page we own.
    for (const QString &script : backgroundScripts) {
        if (script.isEmpty() || script.startsWith(QLatin1Char('/'))
                || script.contains(QLatin1Char(':'))
                || script.split(QLatin1Char('/')).contains(
                    QStringLiteral("..")))
            continue;
        m_scripts.append(script);
    }

    // Hidden host view — never shown (the spike measured the page
    // stays answerable under Chromium's hidden-page throttling; the
    // Active lifecycle request + nudge timer are the bounded wake
    // strategy on top).
    m_view = new QWebEngineView;
    // Navigation-locked page — see HostedBackgroundPage above: the
    // registered channel objects outlive any document change.
    m_page = new HostedBackgroundPage(profile, extensionId, m_view);
    m_view->setPage(m_page);

    m_channel = new QWebChannel(m_page);
    m_bridge = new ChromeBridge(extensionId, extensionName,
                                extensionPath, nullptr, m_page);
    m_bridge->setContextPage(m_page);
    m_channel->registerObject(QStringLiteral("chromeBridge"), m_bridge);
    // MainWorld — the shim runs there beside the extension's real
    // scripts; the page is the extension's own context, so the bridge
    // object it reaches is scoped to its own id by construction.
    m_page->setWebChannel(m_channel, QWebEngineScript::MainWorld);

    connect(m_bridge, &ChromeBridge::errorReported, this,
            [this](const QString &message) {
        if (m_errors.size() < 200)
            m_errors.append(message);
        m_error = message;
        emit stateChanged();
    });
    // EXT15: a shimmed API answering a typed error is health-relevant
    // (the extension relies on something that cannot work) but not a
    // boot failure — record it on the diagnostics list, not m_error.
    connect(m_bridge, &ChromeBridge::gapReported, this,
            [this](const QString &api, const QString &error) {
        const QString entry = api + QLatin1String(": ") + error;
        if (m_errors.size() < 200 && !m_errors.contains(entry))
            m_errors.append(entry);
        emit stateChanged();
    });
    connect(m_page, &QWebEnginePage::renderProcessTerminated, this,
            [this](QWebEnginePage::RenderProcessTerminationStatus status,
                   int exitCode) {
        fail(tr("render process terminated (status %1, exit %2)")
                 .arg(int(status)).arg(exitCode));
    });
    connect(m_page, &QWebEnginePage::loadFinished, this,
            [this](bool ok) {
        if (!ok && m_state == State::Starting)
            fail(tr("host page failed to load"));
        else if (m_state == State::Starting)
            m_page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
    });

    // Arm the injection BEFORE navigating — DocumentCreation only
    // runs once per document.
    if (profile) {
        const QString extensionCode =
            extensionCodeSource(extensionPath, m_scripts);
        QWebEngineScript injection;
        injection.setName(injectionName(extensionId));
        injection.setInjectionPoint(QWebEngineScript::DocumentCreation);
        injection.setWorldId(QWebEngineScript::MainWorld);
        injection.setRunsOnSubFrames(false);
        injection.setSourceCode(
            injectionSource(extensionId, extensionName,
                            extensionPath, m_scripts,
                            !extensionCode.isEmpty()));
        profile->scripts()->insert(injection);

        // The extension's own code is delivered as injected source —
        // exempt from the page CSP that blocks every synchronous
        // execution channel importScripts() could use.  Entry first,
        // then lazy chunks, inside one synchronous pass.
        if (!extensionCode.isEmpty()) {
            QWebEngineScript code;
            code.setName(injectionName(extensionId)
                         + QStringLiteral("-code"));
            code.setInjectionPoint(QWebEngineScript::DocumentReady);
            code.setWorldId(QWebEngineScript::MainWorld);
            code.setRunsOnSubFrames(false);
            code.setSourceCode(
                QStringLiteral("(function(){")
                + QLatin1String("if(window.top!==window||location.href!==")
                + jsLiteral(hostUrl(extensionId).toString())
                + QLatin1String(")return;")
                + extensionCode
                + QLatin1String(
                    "if(window.__aroraHost)"
                        "window.__aroraHost.scriptsLoaded=true;"
                    "})();"));
            profile->scripts()->insert(code);
        }

        // EXT17 — the sibling-page shim: armed with the host (the
        // relay peer it talks to), gated inside the payload to
        // top-level chrome-extension://<id>/ documents that are not
        // the host wrapper.  No package bytes are touched (SEC25).
        QWebEngineScript pages;
        pages.setName(pageInjectionName(extensionId));
        pages.setInjectionPoint(QWebEngineScript::DocumentCreation);
        pages.setWorldId(QWebEngineScript::MainWorld);
        pages.setRunsOnSubFrames(false);
        pages.setSourceCode(
            pageInjectionSource(extensionId, extensionName,
                                extensionPath));
        profile->scripts()->insert(pages);
    }

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this,
            &ExtensionBackgroundHost::poll);
    m_poll->start();

    m_nudge = new QTimer(this);
    m_nudge->setInterval(kNudgeMs);
    connect(m_nudge, &QTimer::timeout, this,
            &ExtensionBackgroundHost::nudge);
    m_nudge->start();

    QTimer::singleShot(kBootWatchdogMs, this, [this]() {
        if (m_state == State::Starting)
            fail(tr("background boot timed out"));
    });

    m_page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
    m_page->load(hostUrl(extensionId));
}

ExtensionBackgroundHost::~ExtensionBackgroundHost()
{
    if (m_profile) {
        const QList<QWebEngineScript> installed =
            m_profile->scripts()->toList();
        const QString name = injectionName(m_extensionId);
        for (const QWebEngineScript &script : installed) {
            if (script.name() == name
                || script.name() == name + QLatin1String("-code")
                || script.name() == pageInjectionName(m_extensionId))
                m_profile->scripts()->remove(script);
        }
    }
    delete m_view;
}

QStringList ExtensionBackgroundHost::shimGaps() const
{
    return m_bridge && m_bridge->backend()
        ? m_bridge->backend()->gaps() : QStringList();
}

void ExtensionBackgroundHost::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void ExtensionBackgroundHost::fail(const QString &why)
{
    m_error = why;
    if (m_errors.size() < 200)
        m_errors.append(why);
    setState(State::Failed);
}

void ExtensionBackgroundHost::poll()
{
    if (m_state != State::Starting || !m_page)
        return;
    m_page->runJavaScript(
        QStringLiteral(
            "window.__aroraHost?JSON.stringify({s:__aroraHost.state,"
            "b:__aroraHost.bridgeUp,l:__aroraHost.scriptsLoaded,"
            "e:__aroraHost.errors.slice(-1)}):null"),
        [this](const QVariant &value) {
            if (m_state != State::Starting || !value.isValid()
                    || value.isNull())
                return;
            const QJsonObject report = QJsonDocument::fromJson(
                value.toString().toUtf8()).object();
            const QString state =
                report.value(QLatin1String("s")).toString();
            if (state == QLatin1String("ready")) {
                setState(State::Ready);
                m_poll->stop();
            } else if (state.startsWith(QLatin1String("failed"))) {
                const QJsonArray errs =
                    report.value(QLatin1String("e")).toArray();
                fail(errs.isEmpty()
                    ? state
                    : errs.last().toString());
            }
        });
}

void ExtensionBackgroundHost::nudge()
{
    if (!m_page)
        return;
    // Bounded wake — the hidden page's timers are throttled but the
    // context stays answerable; a trivial eval per interval keeps
    // deferred work draining instead of starving to a full freeze.
    m_page->runJavaScript(
        QStringLiteral("window.__aroraWake&&__aroraWake()"));
}

// ------------------------------------------------------------------
// ExtensionBackgroundRuntime
// ------------------------------------------------------------------

ExtensionBackgroundRuntime::ExtensionBackgroundRuntime(QObject *parent)
    : QObject(parent)
    , m_heartbeat(new QTimer(this))
{
    // The reconcile heartbeat — changed() covers the signal-driving
    // transitions; the heartbeat catches the ones with no signal
    // (the bundled-install path toggles the Qt manager directly).
    m_heartbeat->setInterval(2500);
    connect(m_heartbeat, &QTimer::timeout, this,
            &ExtensionBackgroundRuntime::syncNow);
}

ExtensionBackgroundRuntime *ExtensionBackgroundRuntime::instance()
{
    static ExtensionBackgroundRuntime *runtime = nullptr;
    if (!runtime)
        runtime = new ExtensionBackgroundRuntime(qApp);
    return runtime;
}

void ExtensionBackgroundRuntime::attachProfile(QWebEngineProfile *profile)
{
    if (!profile || m_profiles.contains(profile))
        return;
    // Same gate as ExtensionManager::installOnProfile — extensions
    // are a deanonymization surface on the tor profile and the engine
    // refuses them off-the-record anyway.
    if (BrowserApplication::isTorMode()
            || profile->isOffTheRecord()
            || profile == BrowserProfile::torProfileIfCreated())
        return;
    m_profiles.append(profile);
    if (!m_heartbeat->isActive())
        m_heartbeat->start();
    // EXT19: extension pages wire themselves — start watching views.
    watchExtensionViews();
    // A profile can arrive with extensions already enabled (installed
    // earlier on this session's profile) — reconcile promptly.
    QTimer::singleShot(2000, this,
                       &ExtensionBackgroundRuntime::syncNow);
}

void ExtensionBackgroundRuntime::teardown(const QString &id)
{
    ExtensionBackgroundHost *host = m_hosts.take(id);
    if (host) {
        unregisterRelayContext(host->bridge());
        host->disconnect(this);
        host->deleteLater();
        emit hostChanged(id);
    }
    // EXT17: sibling pages keep no state across a host teardown —
    // their shared backend just died, so their bridges are dead
    // contexts.  Drop them (the shim answers typed errors); Chrome
    // closes an extension's pages on unload anyway.
    for (auto it = m_pageBridges.begin(); it != m_pageBridges.end();) {
        ChromeBridge *bridge = it.value();
        if (bridge && bridge->extensionId() == id) {
            unregisterRelayContext(bridge);
            it = m_pageBridges.erase(it);
            bridge->deleteLater();
        } else {
            ++it;
        }
    }
}

void ExtensionBackgroundRuntime::syncNow()
{
#if QT_CONFIG(webengine_extensions)
    // Desired set: enabled+loaded extensions on prepared profiles
    // that we can read a manifest for (built-ins carry no path and
    // never enter the map).
    struct Want {
        QPointer<QWebEngineProfile> profile;
        QString name;
        QString path;
    };
    QHash<QString, Want> desired;
    for (const QPointer<QWebEngineProfile> &profile : m_profiles) {
        if (!profile || profile->isOffTheRecord())
            continue;
        QWebEngineExtensionManager *extensions =
            profile->extensionManager();
        if (!extensions)
            continue;
        const QList<QWebEngineExtensionInfo> loaded =
            extensions->extensions();
        for (const QWebEngineExtensionInfo &info : loaded) {
            if (!info.id().isEmpty() && info.isLoaded()
                    && info.isEnabled() && !info.path().isEmpty()
                    && !desired.contains(info.id())) {
                Want want;
                want.profile = profile;
                want.name = info.name();
                want.path = info.path();
                desired.insert(info.id(), want);
            }
        }
    }

    // Drop hosts whose extension went away or got disabled; drop the
    // no-background memo for anything no longer desired so a future
    // enable (or a manifest-changing update) re-evaluates.
    for (const QString &id : m_hosts.keys()) {
        if (!desired.contains(id))
            teardown(id);
    }
    const QSet<QString> stale = m_noBackground;
    for (const QString &id : stale) {
        if (!desired.contains(id))
            m_noBackground.remove(id);
    }
    const QStringList gated = m_extPermissions.keys();
    for (const QString &id : gated) {
        if (!desired.contains(id)) {
            m_extPermissions.remove(id);
            m_extHostPermissions.remove(id);
        }
    }

    // EXT16 — the browser-wide event hub tracks tabs/windows only
    // while an extension exists to consume events.
    ExtensionEventHub::instance()->setTracking(!desired.isEmpty());

    for (auto it = desired.constBegin(); it != desired.constEnd(); ++it) {
        const QString &id = it.key();
        if (m_hosts.contains(id) || m_noBackground.contains(id))
            continue;
        const Want &want = it.value();
        // The script list comes through the same classified manifest
        // read the review path uses (rc_ext_manifest_check under
        // rustcore) — never a raw JSON parse here.
        const ExtensionManager::Manifest manifest =
            ExtensionManager::inspectManifest(want.path);
        if (!manifest.valid || manifest.backgroundScripts.isEmpty()) {
            m_noBackground.insert(id);
            continue;
        }
        // EXT16 gate snapshot — the fan-out filters events by the
        // manifest's declared permissions.
        m_extPermissions.insert(id, manifest.permissions);
        m_extHostPermissions.insert(id, manifest.hostPermissions);
        if (!want.profile)
            continue;
        auto *host = new ExtensionBackgroundHost(
            want.profile, id, want.name, want.path,
            manifest.backgroundScripts, this);
        connect(host, &ExtensionBackgroundHost::stateChanged, this,
                [this, id]() { emit hostChanged(id); });
        m_hosts.insert(id, host);
        emit hostChanged(id);
    }

    // EXT19: re-sweep — a popup whose document committed while no
    // host existed yet retries its attach on this pass, and views an
    // event could never reach (parentless, never shown) are picked up
    // each heartbeat.
    watchExtensionViews();
#endif // QT_CONFIG(webengine_extensions)
}

// -------------------------------------------------------------------
// EXT17 sibling pages — an extension's own documents (action popup,
// options tab, offscreen document) get a chromeBridge channel of
// their own so the injected preamble/shim handshake lands and their
// chrome.* calls share the hosted background's backend.  The page
// injection (armed by the host) already ran at DocumentCreation; the
// bridge is what makes the shim live.
// -------------------------------------------------------------------

bool ExtensionBackgroundRuntime::attachExtensionPage(
        QWebEnginePage *page, const QString &extensionId)
{
    if (!page)
        return false;
    QString id = extensionId;
    if (id.isEmpty()) {
        const QUrl url = page->url();
        if (url.scheme() == QLatin1String("chrome-extension"))
            id = url.host();
    }
    if (id.isEmpty())
        return false;
    if (m_pageBridges.contains(page)) {
        ChromeBridge *existing = m_pageBridges.value(page);
        if (existing && existing->extensionId() == id)
            return true;    // already wired for this extension
        // A navigation to ANOTHER extension's document — the old
        // bridge is scoped to the previous id and must not answer
        // for the new context (relay would cross boundaries).
        detachExtensionPage(page);
    }
    ExtensionBackgroundHost *host = m_hosts.value(id);
    if (!host || !host->bridge())
        return false;
    // EXT19: the host's own document already carries the channel its
    // preamble connected to — installing a page bridge on it would
    // replace qt.webChannelTransport mid-boot and the register call
    // would land on the page bridge instead of host->bridge() (the
    // view watch sweeps hidden host views too — they are widgets).
    if (page == host->page())
        return false;
    // Channels only ever bind pages on the host's own profile — a
    // foreign-profile or OTR/tor page never reaches the bridge.
    if (!page->profile() || page->profile()->isOffTheRecord()
            || !host->page() || !host->page()->profile()
            || page->profile() != host->page()->profile())
        return false;

    auto *channel = new QWebChannel(page);
    auto *bridge = new ChromeBridge(id, host->extensionName(),
                                    host->extensionPath(),
                                    host->bridge()->backend(), page);
    bridge->setContextPage(page);
    channel->registerObject(QStringLiteral("chromeBridge"), bridge);
    page->setWebChannel(channel, QWebEngineScript::MainWorld);
    m_pageBridges.insert(page, bridge);
    connect(page, &QObject::destroyed, this, [this, page]() {
        detachExtensionPage(page);
    });
    return true;
}

void ExtensionBackgroundRuntime::detachExtensionPage(
        QWebEnginePage *page)
{
    if (!page)
        return;
    ChromeBridge *bridge = m_pageBridges.take(page);
    if (bridge) {
        unregisterRelayContext(bridge);
        bridge->deleteLater();
    }
}

bool ExtensionBackgroundRuntime::hasExtensionPage(
        QWebEnginePage *page) const
{
    return m_pageBridges.contains(page);
}

// -------------------------------------------------------------------
// EXT19 — sibling pages self-wire.  The live action popup (EXT08's
// toolbar strip) creates a plain QWebEngineView + page and loads
// chrome-extension://<id>/... — nothing on that path remembered to
// call attachExtensionPage, so the injected preamble's channel poll
// expired into a blank white popup.  Watch every QWebEngineView the
// application raises: a committed chrome-extension:// navigation on
// a hosted profile attaches its own bridge; navigating away detaches
// it again.  attachExtensionPage remains the explicit path for
// callers that can wire before the first commit.
// -------------------------------------------------------------------

bool ExtensionBackgroundRuntime::eventFilter(QObject *watched,
                                             QEvent *event)
{
    if (event->type() == QEvent::ChildAdded) {
        QWebEngineView *view = qobject_cast<QWebEngineView *>(
            static_cast<QChildEvent *>(event)->child());
        if (view)
            watchView(view);
    } else if (event->type() == QEvent::Show) {
        if (QWebEngineView *view =
                qobject_cast<QWebEngineView *>(watched))
            watchView(view);
    }
    return QObject::eventFilter(watched, event);
}

void ExtensionBackgroundRuntime::watchExtensionViews()
{
    if (!m_viewFilterInstalled) {
        m_viewFilterInstalled = true;
        qApp->installEventFilter(this);
    }
    const QList<QWidget *> widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        if (QWebEngineView *view =
                qobject_cast<QWebEngineView *>(widget))
            watchView(view);
    }
}

void ExtensionBackgroundRuntime::watchView(QWebEngineView *view)
{
    if (!view)
        return;
    if (!view->property("aroraExtPageWatch").toBool()) {
        view->setProperty("aroraExtPageWatch", true);
        // Context = the view: the hook dies with it.  Both guards are
        // QPointer — a navigation landing during application teardown
        // must not touch a gone runtime or view.
        const QPointer<ExtensionBackgroundRuntime> self(this);
        const QPointer<QWebEngineView> guard(view);
        connect(view, &QWebEngineView::urlChanged, view,
                [self, guard]() {
            if (self && guard)
                self->syncPageBridge(guard);
        });
    }
    syncPageBridge(view);
}

void ExtensionBackgroundRuntime::syncPageBridge(QWebEngineView *view)
{
    QWebEnginePage *page = view->page();
    if (!page)
        return;
    const QUrl url = page->url();
    if (url.scheme() == QLatin1String("chrome-extension"))
        attachExtensionPage(page, url.host());
    else
        detachExtensionPage(page);
}

QJsonArray ExtensionBackgroundRuntime::contextsJson(
        const QString &extensionId) const
{
    QJsonArray out;
    for (const RelayContext &ctx : m_relayContexts) {
        if (!ctx.bridge || ctx.bridge->extensionId() != extensionId)
            continue;
        QJsonObject entry;
        QString type = QStringLiteral("TAB");
        if (ctx.kind == QLatin1String("background"))
            type = QStringLiteral("BACKGROUND");
        else if (ctx.kind == QLatin1String("popup"))
            type = QStringLiteral("POPUP");
        else if (ctx.kind == QLatin1String("offscreen"))
            type = QStringLiteral("OFFSCREEN_DOCUMENT");
        entry.insert(QStringLiteral("contextType"), type);
        entry.insert(QStringLiteral("contextId"), ctx.contextId);
        entry.insert(QStringLiteral("documentOrigin"),
                     QStringLiteral("chrome-extension://")
                         + extensionId);
        entry.insert(QStringLiteral("documentUrl"),
                     ctx.url.isEmpty()
                         ? ExtensionBackgroundHost::hostUrl(extensionId)
                             .toString()
                         : ctx.url);
        entry.insert(QStringLiteral("incognito"), false);
        entry.insert(QStringLiteral("tabId"), -1);
        entry.insert(QStringLiteral("windowId"), -1);
        out.append(entry);
    }
    return out;
}

ExtensionBackgroundHost *ExtensionBackgroundRuntime::host(
        const QString &extensionId) const
{
    return m_hosts.value(extensionId);
}

// -------------------------------------------------------------------
// EXT15 messaging relay.  Native runtime.sendMessage only reaches the
// real service-worker context — messages between shimmed contexts
// (hosted<->hosted now, hosted->popup once EXT17 registers popup
// channels) fan out here.  Delivery is the sender's wrapped
// envelope; sendResponse routes back over relay.respond.  In-flight
// mids expire after 30s so a dead peer cannot pin the slot.
// -------------------------------------------------------------------

QString ExtensionBackgroundRuntime::registerRelayContext(
        ChromeBridge *bridge, const QString &kind, const QString &url)
{
    if (!bridge)
        return QString();
    for (int i = 0; i < m_relayContexts.size(); ++i) {
        if (m_relayContexts.at(i).bridge == bridge) {
            // A re-register refreshes kind/url (the page may learn
            // its role after attach).
            m_relayContexts[i].kind = kind;
            if (!url.isEmpty())
                m_relayContexts[i].url = url;
            return m_relayContexts.at(i).contextId;
        }
    }
    RelayContext ctx;
    ctx.bridge = bridge;
    ctx.kind = kind;
    ctx.url = url;
    ctx.contextId = bridge->extensionId() + QLatin1Char('|') + kind
        + QLatin1Char('|')
        + QString::number(m_nextContextId++);
    m_relayContexts.append(ctx);
    return ctx.contextId;
}

void ExtensionBackgroundRuntime::unregisterRelayContext(
        ChromeBridge *bridge)
{
    for (int i = m_relayContexts.size() - 1; i >= 0; --i) {
        if (!m_relayContexts.at(i).bridge
                || m_relayContexts.at(i).bridge == bridge)
            m_relayContexts.removeAt(i);
    }
    // Any in-flight messages this bridge sent are unanswerable —
    // drop them (the page-side entry has its own timeout).
    const QStringList mids = m_relayPending.keys();
    for (const QString &mid : mids) {
        if (!m_relayPending.value(mid).origin
                || m_relayPending.value(mid).origin == bridge) {
            if (QTimer *t = m_relayPendingTimers.take(mid))
                t->deleteLater();
            m_relayPending.remove(mid);
        }
    }
    // Port ends owned by this bridge die with it; remaining ends get
    // the disconnect push so their onDisconnect fires honestly.
    const QStringList chans = m_portEnds.keys();
    for (const QString &chan : chans) {
        QList<PortEnd> &ends = m_portEnds[chan];
        bool removed = false;
        for (int i = ends.size() - 1; i >= 0; --i) {
            if (!ends.at(i).bridge || ends.at(i).bridge == bridge) {
                ends.removeAt(i);
                removed = true;
            }
        }
        if (!removed)
            continue;
        if (ends.isEmpty()) {
            m_portEnds.remove(chan);
            continue;
        }
        for (const PortEnd &end : std::as_const(ends)) {
            if (end.bridge) {
                emit end.bridge->hostEvent(
                    QStringLiteral("__arora.portDisconnect"),
                    QJsonObject{{QStringLiteral("portId"),
                                 end.localPortId}});
            }
        }
    }
}

int ExtensionBackgroundRuntime::relaySend(ChromeBridge *from,
                                          const QString &mid,
                                          const QJsonObject &envelope)
{
    if (!from || !ownsRelayKey(from, mid))
        return 0;
    int peers = 0;
    for (const RelayContext &ctx : m_relayContexts) {
        if (!ctx.bridge || ctx.bridge == from)
            continue;
        if (ctx.bridge->extensionId() != from->extensionId())
            continue;   // never cross extension boundaries
        emit ctx.bridge->hostEvent(QStringLiteral("__arora.relay"),
                                   QJsonObject{
                                       {QStringLiteral("mid"), mid},
                                       {QStringLiteral("msg"), envelope}});
        ++peers;
        // A popup->background send lands on a throttled hidden page —
        // nudge it awake like the EXT16 event fan-out does.
        if (ExtensionBackgroundHost *host =
                m_hosts.value(ctx.bridge->extensionId())) {
            if (ctx.bridge == host->bridge())
                host->nudge();
        }
    }
    if (peers > 0) {
        PendingRelay pending;
        pending.origin = from;
        pending.peers = peers;
        m_relayPending.insert(mid, pending);
        QTimer *expiry = new QTimer(this);
        expiry->setSingleShot(true);
        expiry->setInterval(30000);
        connect(expiry, &QTimer::timeout, this, [this, mid]() {
            m_relayPending.remove(mid);
            if (QTimer *t = m_relayPendingTimers.take(mid))
                t->deleteLater();
        });
        m_relayPendingTimers.insert(mid, expiry);
        expiry->start();
    }
    return peers;
}

bool ExtensionBackgroundRuntime::relayRespond(ChromeBridge *responder,
                                              const QString &mid,
                                              const QJsonValue &response)
{
    auto it = m_relayPending.find(mid);
    if (it == m_relayPending.end() || !it.value().origin)
        return false;
    // QUAL03: only a context of the SENDING extension may resolve its
    // message — a foreign bridge that learned (or guessed) the mid
    // would otherwise inject a forged sendMessage response.
    if (!responder || !ownsRelayKey(responder, mid)
            || responder->extensionId()
               != it.value().origin->extensionId())
        return false;
    PendingRelay &pending = it.value();
    ++pending.replies;
    const bool defined =
        !response.isUndefined() && !response.isNull();
    // First real response wins (Chrome parity); empty peer replies
    // only resolve once every peer has answered.
    if (!defined && pending.replies < pending.peers)
        return true;
    QPointer<ChromeBridge> origin = pending.origin;
    m_relayPending.erase(it);
    if (QTimer *t = m_relayPendingTimers.take(mid))
        t->deleteLater();
    emit origin->hostEvent(
        QStringLiteral("__arora.relayResponse"),
        QJsonObject{{QStringLiteral("mid"), mid},
                    {QStringLiteral("response"),
                     defined ? response : QJsonValue(QJsonValue::Null)}});
    return true;
}

// -------------------------------------------------------------------
// EXT15 port routing — runtime.connect() between shimmed contexts.
// A chan identifies the logical port; each context registers its own
// local port id as an end.  portConnect offers the channel to every
// same-extension peer (the accepter fires runtime.onConnect), message
// delivery re-addresses to the remote ends' local ids, and disconnect
// pushes onDisconnect at the survivors.  Extension boundaries are
// never crossed — peers are filtered by extension id throughout.
// -------------------------------------------------------------------

int ExtensionBackgroundRuntime::relayPortConnect(
        ChromeBridge *from, const QString &chan,
        const QString &portId, const QString &name,
        const QString &senderUrl)
{
    if (!from || chan.isEmpty() || portId.isEmpty()
            || !ownsRelayKey(from, chan))
        return 0;
    m_portEnds[chan].append(PortEnd{from, portId});
    // A complete MessageSender — extension handlers gate on
    // sender.origin == chrome.runtime.getURL('') (Bitwarden's
    // senderIsInternal drops the session port otherwise).  The id is
    // always trusted from the bridge; url/origin describe the
    // connecting document.
    const QJsonObject sender{
        {QStringLiteral("id"), from->extensionId()},
        {QStringLiteral("url"), senderUrl},
        {QStringLiteral("origin"), QStringLiteral("chrome-extension://")
                                     + from->extensionId()}};
    int peers = 0;
    for (const RelayContext &ctx : m_relayContexts) {
        if (!ctx.bridge || ctx.bridge == from)
            continue;
        if (ctx.bridge->extensionId() != from->extensionId())
            continue;
        emit ctx.bridge->hostEvent(
            QStringLiteral("__arora.portConnect"),
            QJsonObject{{QStringLiteral("chan"), chan},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("sender"), sender}});
        ++peers;
        if (ExtensionBackgroundHost *host =
                m_hosts.value(ctx.bridge->extensionId())) {
            if (ctx.bridge == host->bridge())
                host->nudge();
        }
    }
    return peers;
}

void ExtensionBackgroundRuntime::relayPortAccept(
        ChromeBridge *accepter, const QString &chan,
        const QString &portId)
{
    if (!accepter || chan.isEmpty() || portId.isEmpty()
            || !ownsRelayKey(accepter, chan))
        return;
    for (const PortEnd &end : m_portEnds.value(chan)) {
        if (end.bridge == accepter && end.localPortId == portId)
            return;   // already registered
    }
    m_portEnds[chan].append(PortEnd{accepter, portId});
}

int ExtensionBackgroundRuntime::relayPortMessage(
        ChromeBridge *from, const QString &chan,
        const QString &portId, const QJsonValue &msg)
{
    if (!from || !ownsRelayKey(from, chan))
        return 0;
    int delivered = 0;
    const QList<PortEnd> ends = m_portEnds.value(chan);
    for (const PortEnd &end : ends) {
        if (!end.bridge)
            continue;
        if (end.bridge->extensionId() != from->extensionId())
            continue;   // never cross extension boundaries
        if (end.bridge == from && end.localPortId == portId)
            continue;   // the sending end itself
        emit end.bridge->hostEvent(
            QStringLiteral("__arora.portMessage"),
            QJsonObject{{QStringLiteral("portId"), end.localPortId},
                        {QStringLiteral("msg"), msg}});
        ++delivered;
        if (ExtensionBackgroundHost *host =
                m_hosts.value(end.bridge->extensionId())) {
            if (end.bridge == host->bridge())
                host->nudge();
        }
    }
    return delivered;
}

void ExtensionBackgroundRuntime::relayPortDisconnect(
        ChromeBridge *from, const QString &chan,
        const QString &portId)
{
    if (!from || !ownsRelayKey(from, chan))
        return;
    auto it = m_portEnds.find(chan);
    if (it == m_portEnds.end())
        return;
    QList<PortEnd> &ends = it.value();
    for (int i = ends.size() - 1; i >= 0; --i) {
        if (ends.at(i).bridge == from
                && ends.at(i).localPortId == portId)
            ends.removeAt(i);
    }
    if (ends.isEmpty()) {
        m_portEnds.erase(it);
        return;
    }
    for (const PortEnd &end : std::as_const(ends)) {
        if (end.bridge) {
            emit end.bridge->hostEvent(
                QStringLiteral("__arora.portDisconnect"),
                QJsonObject{{QStringLiteral("portId"),
                             end.localPortId}});
        }
    }
}

// -------------------------------------------------------------------
// EXT16 event pushes — browser-originated events produced by
// ExtensionEventHub.  Chrome permission parity:
//   webNavigation.* / contextMenus.* — the whole namespace needs the
//     manifest permission; without it Chrome exposes no API surface
//     at all, so the event never reaches the context.
//   tabs.* / windows.* — the events themselves are ungated, but the
//     url-bearing fields (tab.url/title/favIconUrl, changeInfo.url…)
//     are scrubbed unless the extension holds 'tabs' or a host
//     permission covering the tab's URL.
// Host-internal pushes (__arora.*, alarms) keep their ungated path.
// -------------------------------------------------------------------

namespace {

// The manifest permission a host event family is gated on; empty for
// ungated events.
QString eventGate(const QString &name)
{
    const QString ns = name.section(QLatin1Char('.'), 0, 0);
    if (ns == QLatin1String("webNavigation"))
        return QStringLiteral("webNavigation");
    if (ns == QLatin1String("contextMenus"))
        return QStringLiteral("contextMenus");
    return QString();
}

// Chrome match-pattern check for the tabs/url gate — a host
// permission covering the tab's URL unhides url/title the same way
// the 'tabs' permission does.
bool hostCovers(const QString &pattern, const QUrl &url)
{
    const QString scheme = url.scheme();
    if (pattern == QLatin1String("<all_urls>")) {
        return scheme == QLatin1String("http")
            || scheme == QLatin1String("https")
            || scheme == QLatin1String("file")
            || scheme == QLatin1String("ftp");
    }
    const int sep = pattern.indexOf(QLatin1String("://"));
    if (sep < 0)
        return false;
    const QString pscheme = pattern.left(sep);
    const QString rest = pattern.mid(sep + 3);
    const int slash = rest.indexOf(QLatin1Char('/'));
    const QString phost = slash < 0 ? rest : rest.left(slash);
    const QString ppath =
        slash < 0 ? QStringLiteral("/*") : rest.mid(slash);
    if (pscheme == QLatin1String("*")) {
        // '*://' covers http + https only (Chrome semantics).
        if (scheme != QLatin1String("http")
                && scheme != QLatin1String("https"))
            return false;
    } else if (pscheme != scheme) {
        return false;
    }
    const QString host = url.host();
    if (phost == QLatin1String("*")) {
        // any host
    } else if (phost.startsWith(QLatin1String("*."))) {
        const QString base = phost.mid(2);
        if (host != base && !host.endsWith(QLatin1Char('.') + base))
            return false;
    } else if (phost != host) {
        return false;
    }
    QString path = url.path(QUrl::FullyEncoded);
    if (url.hasQuery())
        path += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    const QRegularExpression re(
        QRegularExpression::wildcardToRegularExpression(ppath));
    return re.match(path).hasMatch();
}

// url-bearing fields hidden without 'tabs'/host access — Chrome
// omits them entirely rather than emptying them.  Applied to tab
// objects, changeInfo, and window.tab lists alike — every shape the
// hub's payloads carry them in.
void scrubObject(QJsonObject &object)
{
    object.remove(QStringLiteral("url"));
    object.remove(QStringLiteral("title"));
    object.remove(QStringLiteral("favIconUrl"));
    object.remove(QStringLiteral("pendingUrl"));
    if (object.contains(QLatin1String("tabs"))) {
        QJsonArray tabs =
            object.value(QLatin1String("tabs")).toArray();
        for (int i = 0; i < tabs.size(); ++i) {
            QJsonObject tab = tabs.at(i).toObject();
            scrubObject(tab);
            tabs[i] = tab;
        }
        object.insert(QStringLiteral("tabs"), tabs);
    }
}

} // namespace

bool ExtensionBackgroundRuntime::seesUrls(const QString &extensionId,
                                          const QString &url) const
{
    if (m_extPermissions.value(extensionId)
            .contains(QLatin1String("tabs")))
        return true;
    const QUrl qurl(QString::fromUtf8(url.toUtf8()));
    for (const QString &pattern : m_extHostPermissions.value(extensionId)) {
        if (hostCovers(pattern, qurl))
            return true;
    }
    return false;
}

QJsonObject ExtensionBackgroundRuntime::shapedPayload(
        const QString &extensionId, const QString &name,
        const QJsonObject &payload) const
{
    const QString ns = name.section(QLatin1Char('.'), 0, 0);
    if (ns != QLatin1String("tabs")
            && ns != QLatin1String("windows")
            && ns != QLatin1String("contextMenus"))
        return payload;
    // The URL the host-permission fallback matches against — the
    // first url field the payload carries: a named 'tab'/'info'
    // field, else the first url-bearing object inside the __args
    // multi-arg envelope.
    QString url = payload.value(QLatin1String("tab")).toObject()
        .value(QLatin1String("url")).toString();
    if (url.isEmpty()) {
        url = payload.value(QLatin1String("info")).toObject()
            .value(QLatin1String("pageUrl")).toString();
    }
    const QJsonArray argList =
        payload.value(QLatin1String("__args")).toArray();
    for (const QJsonValue &v : argList) {
        if (!url.isEmpty())
            break;
        url = v.toObject().value(QLatin1String("url")).toString();
    }
    if (seesUrls(extensionId, url))
        return payload;
    QJsonObject shaped = payload;
    for (const QString &key : {QStringLiteral("tab"),
                               QStringLiteral("changeInfo"),
                               QStringLiteral("window"),
                               QStringLiteral("info")}) {
        if (!shaped.contains(key))
            continue;
        QJsonObject o = shaped.value(key).toObject();
        scrubObject(o);
        shaped.insert(key, o);
    }
    if (!argList.isEmpty()) {
        // Tab objects travel inside the __args array — scrub each
        // object argument's url-bearing fields.
        QJsonArray scrubbed = argList;
        for (int i = 0; i < scrubbed.size(); ++i) {
            if (!scrubbed.at(i).isObject())
                continue;
            QJsonObject o = scrubbed.at(i).toObject();
            scrubObject(o);
            scrubbed[i] = o;
        }
        shaped.insert(QStringLiteral("__args"), scrubbed);
    }
    return shaped;
}

int ExtensionBackgroundRuntime::fanOut(
        const QString &name, const QJsonObject &payload,
        const QString &onlyExtensionId)
{
    const QString gate = eventGate(name);
    int delivered = 0;
    QSet<QString> nudged;
    for (const RelayContext &ctx : m_relayContexts) {
        ChromeBridge *bridge = ctx.bridge;
        if (!bridge)
            continue;
        const QString extId = bridge->extensionId();
        if (!onlyExtensionId.isEmpty() && extId != onlyExtensionId)
            continue;
        if (!gate.isEmpty()
                && !m_extPermissions.value(extId).contains(gate))
            continue;
        emit bridge->hostEvent(name, shapedPayload(extId, name, payload));
        ++delivered;
        // Push-then-wake — a throttled hidden page drains the
        // delivery promptly instead of waiting for its own nudge
        // timer's next tick.
        if (!nudged.contains(extId)) {
            nudged.insert(extId);
            if (ExtensionBackgroundHost *host = m_hosts.value(extId))
                host->nudge();
        }
    }
    return delivered;
}

int ExtensionBackgroundRuntime::broadcastEvent(
        const QString &name, const QJsonObject &payload)
{
    return fanOut(name, payload, QString());
}

int ExtensionBackgroundRuntime::deliverEvent(
        const QString &extensionId, const QString &name,
        const QJsonObject &payload)
{
    return fanOut(name, payload, extensionId);
}
