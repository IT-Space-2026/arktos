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
#include "extensionmanager.h"
#include "extensionshimbackend.h"

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qtimer.h>

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
                           QObject *parent)
    : QObject(parent)
    , m_extensionId(extensionId)
    , m_extensionName(extensionName)
{
    // EXT15 backend — storage store, host-timer alarms, the live
    // tab snapshot, action state, manifest-derived answers.  Its
    // pushEvent feeds the page through the hostEvent signal below.
    m_backend = new ExtensionShimBackend(extensionId, extensionName,
                                         extensionPath, this);
    connect(m_backend, &ExtensionShimBackend::pushEvent, this,
            &ChromeBridge::hostEvent);
    connect(m_backend, &ExtensionShimBackend::gapReported, this,
            &ChromeBridge::gapReported);
}

QJsonObject ChromeBridge::handshake(const QJsonObject &hello)
{
    m_handshakeDone = true;
    emit handshook();
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
            runtime->registerRelayContext(this, kind);
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
            args.value(QStringLiteral("name")).toString());
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
                        const QStringList &scripts,
                        bool inlineCode)
{
    QJsonObject config;
    config.insert(QStringLiteral("id"), extensionId);
    config.insert(QStringLiteral("name"), extensionName);
    config.insert(QStringLiteral("inlineCode"), inlineCode);
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
    m_page = new QWebEnginePage(profile, m_view);
    m_view->setPage(m_page);

    m_channel = new QWebChannel(m_page);
    m_bridge = new ChromeBridge(extensionId, extensionName,
                                extensionPath, m_page);
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
            injectionSource(extensionId, extensionName, m_scripts,
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
                || script.name() == name + QLatin1String("-code"))
                m_profile->scripts()->remove(script);
        }
    }
    delete m_view;
}

QStringList ExtensionBackgroundHost::shimNotes() const
{
    return m_bridge && m_bridge->backend()
        ? m_bridge->backend()->notes() : QStringList();
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
#endif // QT_CONFIG(webengine_extensions)
}

ExtensionBackgroundHost *ExtensionBackgroundRuntime::host(
        const QString &extensionId) const
{
    return m_hosts.value(extensionId);
}

QStringList ExtensionBackgroundRuntime::hostedIds() const
{
    return m_hosts.keys();
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
        ChromeBridge *bridge, const QString &kind)
{
    if (!bridge)
        return QString();
    for (const RelayContext &ctx : m_relayContexts) {
        if (ctx.bridge == bridge)
            return ctx.contextId;
    }
    RelayContext ctx;
    ctx.bridge = bridge;
    ctx.kind = kind;
    ctx.contextId = bridge->extensionId() + QLatin1Char('|') + kind
        + QLatin1Char('|')
        + QString::number(m_relayContexts.size());
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
    if (!from || mid.isEmpty())
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
    Q_UNUSED(responder);
    auto it = m_relayPending.find(mid);
    if (it == m_relayPending.end() || !it.value().origin)
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
        const QString &portId, const QString &name)
{
    if (!from || chan.isEmpty() || portId.isEmpty())
        return 0;
    m_portEnds[chan].append(PortEnd{from, portId});
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
                        {QStringLiteral("sender"), from->extensionId()}});
        ++peers;
    }
    return peers;
}

void ExtensionBackgroundRuntime::relayPortAccept(
        ChromeBridge *accepter, const QString &chan,
        const QString &portId)
{
    if (!accepter || chan.isEmpty() || portId.isEmpty())
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
    if (!from)
        return 0;
    int delivered = 0;
    const QList<PortEnd> ends = m_portEnds.value(chan);
    for (const PortEnd &end : ends) {
        if (!end.bridge)
            continue;
        if (end.bridge == from && end.localPortId == portId)
            continue;   // the sending end itself
        emit end.bridge->hostEvent(
            QStringLiteral("__arora.portMessage"),
            QJsonObject{{QStringLiteral("portId"), end.localPortId},
                        {QStringLiteral("msg"), msg}});
        ++delivered;
    }
    return delivered;
}

void ExtensionBackgroundRuntime::relayPortDisconnect(
        ChromeBridge *from, const QString &chan,
        const QString &portId)
{
    if (!from)
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
