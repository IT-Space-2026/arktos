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

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qfile.h>
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
                           QObject *parent)
    : QObject(parent)
    , m_extensionId(extensionId)
    , m_extensionName(extensionName)
{
}

QJsonObject ChromeBridge::handshake(const QJsonObject &hello)
{
    m_handshakeDone = true;
    emit handshook();
    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), true);
    reply.insert(QStringLiteral("host"), QStringLiteral("arora"));
    reply.insert(QStringLiteral("shim"), 1);
    reply.insert(QStringLiteral("id"), m_extensionId);
    reply.insert(QStringLiteral("name"), m_extensionName);
    // hello carries the shim's own protocol version — kept so EXT15
    // can negotiate; nothing negotiated yet means "any".
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
    Q_UNUSED(args);
    // EXT15 fills this in — until then every op answers a TYPED
    // error so a shim relying on it surfaces the gap through the
    // health row instead of silently no-op'ing.
    QJsonObject reply;
    reply.insert(QStringLiteral("ok"), false);
    reply.insert(QStringLiteral("error"),
                 QStringLiteral("unimplemented"));
    reply.insert(QStringLiteral("api"),
                 ns + QLatin1Char('.') + method);
    return reply;
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
// then the config record, the qwebchannel client, and the preamble
// (src/data/exthost.js).  Everything lives in one DocumentCreation
// MainWorld script so the shim lands before any extension code —
// extension pages forbid eval() under MV3 CSP, which is exactly why
// the preamble cannot be delivered by runJavaScript.
QString injectionSource(const QString &extensionId,
                        const QString &extensionName,
                        const QStringList &scripts)
{
    QJsonObject config;
    config.insert(QStringLiteral("id"), extensionId);
    config.insert(QStringLiteral("name"), extensionName);
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
    source += QLatin1String("})();");
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
    m_bridge = new ChromeBridge(extensionId, extensionName, m_page);
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
        QWebEngineScript injection;
        injection.setName(injectionName(extensionId));
        injection.setInjectionPoint(QWebEngineScript::DocumentCreation);
        injection.setWorldId(QWebEngineScript::MainWorld);
        injection.setRunsOnSubFrames(false);
        injection.setSourceCode(
            injectionSource(extensionId, extensionName, m_scripts));
        profile->scripts()->insert(injection);
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
            if (script.name() == name)
                m_profile->scripts()->remove(script);
        }
    }
    delete m_view;
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
            want.profile, id, want.name, manifest.backgroundScripts,
            this);
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
