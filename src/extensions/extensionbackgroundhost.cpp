/*
 * Copyright 2026 The Arktos Authors
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
#include "browsermainwindow.h"
#include "browserprofile.h"
#include "extensioncommands.h"
#include "extensioneventhub.h"
#include "extensionmanager.h"
#include "extensionnotificationcenter.h"
#include "extensionpagebridge.h"
#include "extensionrequestwatcher.h"
#include "extensionshimbackend.h"
#include "profilesettings.h"
#include "tabwidget.h"
#include "webview.h"

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
#include <qwebengineframe.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
#include <qwebenginesettings.h>
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
// background scripts share that world; ArktosChannelWorld isolation is
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
    , m_extensionPath(extensionPath)
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
    reply.insert(QStringLiteral("host"), QStringLiteral("arktos"));
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
    if (qEnvironmentVariableIsSet("ARKTOS_EXTHOST_DEBUG"))
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
    // ER02: tabs.sendMessage — context-scoped like getCurrent: the
    // calling bridge is the waiter the page agent's response
    // resolves onto, so the dispatch lives at this level.
    if (ns == QLatin1String("tabs")
            && method == QLatin1String("sendMessage")) {
        return invokeTabMessage(args);
    }
    // ER07: chrome.scripting.executeScript — async the same way:
    // the per-frame results resolve onto this bridge's hostEvent as
    // '__arktos.execResult'.
    if (ns == QLatin1String("scripting"))
        return invokeScripting(method, args);
    // ER09: chrome.contentScripts — the Firefox-shape registration
    // surface (the bundled webextension-polyfill resolves
    // browser.contentScripts.* to chrome.contentScripts.*).
    if (ns == QLatin1String("contentScripts"))
        return invokeContentScripts(method, args);
    // ER10: chrome.offscreen — context-scoped like tabs.sendMessage:
    // the async createDocument resolves onto THIS bridge's hostEvent.
    if (ns == QLatin1String("offscreen"))
        return invokeOffscreen(method, args);
    // ER11: observe-only chrome.webRequest — listener registration
    // lives at this level because the registering bridge is the
    // lifetime context Chrome binds the listener to.
    if (ns == QLatin1String("webRequest"))
        return invokeWebRequest(method, args);
    // ER17: storage 'changed' — the posting context fired its own
    // onChanged locally, so the fan-out excludes this bridge.
    if (ns == QLatin1String("storage")
            && method == QLatin1String("changed")) {
        return invokeStorageChanged(args);
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

// ER17: a shim context reports the change record it computed for a
// storage mutation (the page keeps the REAL engine area so the
// store stays shared with real extension pages — the engine's own
// storage delegate never emits onChanged, so this post is the only
// onChanged the extension's contexts see).  The poster's shim
// already fired the event synchronously when its mutation
// resolved; the fan-out therefore skips THIS bridge's context.
// Same-domain trust: a covered context can already message its own
// extension's other contexts, so the report needs no re-proof.
QJsonObject ChromeBridge::invokeStorageChanged(const QJsonObject &args)
{
    QJsonObject reply;
    if (!m_backend) {
        reply.insert(QStringLiteral("ok"), false);
        reply.insert(QStringLiteral("error"),
                     QStringLiteral("backend-gone"));
        return reply;
    }
    // ER16 parity: the 'storage' manifest permission gates the
    // namespace — same check invokeStorage applies.
    if (!m_backend->manifestPermissions()
            .contains(QLatin1String("storage"))) {
        reply.insert(QStringLiteral("ok"), false);
        reply.insert(QStringLiteral("error"),
                     QStringLiteral("no-permission"));
        return reply;
    }
    const QString area = args.value(QStringLiteral("area")).toString();
    if (area != QLatin1String("local")
            && area != QLatin1String("session")
            && area != QLatin1String("sync")) {
        reply.insert(QStringLiteral("ok"), false);
        reply.insert(QStringLiteral("error"),
                     QStringLiteral("invalid-area"));
        return reply;
    }
    ExtensionBackgroundRuntime::instance()->fanOutStorageChanged(
        m_extensionId, area,
        args.value(QStringLiteral("changes")).toObject(), this);
    reply.insert(QStringLiteral("ok"), true);
    return reply;
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
// locked: the main frame may only ever be its declared url (the
// wrapper for the background host, the createDocument url for an
// ER10 offscreen document), and subframes must stay inside the
// extension's own origin.  Without this, a hosted page navigated (or
// iframed) to foreign content would hand that content the bridge —
// the extension's storage, identity, and relay channels included.
class HostedBackgroundPage : public QWebEnginePage
{
public:
    HostedBackgroundPage(QWebEngineProfile *profile,
                         const QString &extensionId,
                         const QUrl &mainUrl,
                         QObject *parent = nullptr)
        : QWebEnginePage(profile, parent)
        , m_extensionId(extensionId)
        , m_mainUrl(mainUrl)
    {
        // ER20: extension chrome is not web content — the profile's
        // JavascriptEnabled preference governs pages, never extension
        // contexts (Chrome parity; devtoolswindow.cpp does the same
        // for the inspector).  Without the lift a JS-off session
        // leaves every hosted context dead at "starting".
        settings()->setAttribute(QWebEngineSettings::JavascriptEnabled,
                                 true);
    }

protected:
    bool acceptNavigationRequest(const QUrl &url,
                                 NavigationType /*type*/,
                                 bool isMainFrame) override
    {
        if (isMainFrame)
            return url == m_mainUrl
                || url == QUrl(QStringLiteral("about:blank"));
        return url.scheme() == QLatin1String("chrome-extension")
            && url.host() == m_extensionId;
    }

private:
    QString m_extensionId;
    QUrl m_mainUrl;
};

// QUAL03: relay keys arrive inside caller-supplied arguments, so the
// "<extId>#..." prefix the shim mints is re-verified host-side rather
// than trusted — a bridge may only touch mids/chans its own
// extension owns.
bool ownsRelayKey(const QString &extensionId, const QString &key)
{
    return !extensionId.isEmpty()
        && key.startsWith(extensionId + QLatin1Char('#'));
}

bool ownsRelayKey(const ChromeBridge *bridge, const QString &key)
{
    return bridge && ownsRelayKey(bridge->extensionId(), key);
}

// ER01: a port end / pending origin can be a shim context
// (ChromeBridge — the event leaves on hostEvent) or a page agent
// (ExtensionPageBridge — extEvent).  The chan's "<extId>#" prefix
// carries the same-extension scope for page ends, which have no
// extensionId() of their own (one bridge serves every extension's
// agent on that page).
void emitToEnd(QObject *target, const QString &name,
               const QJsonObject &payload)
{
    if (auto *bridge = qobject_cast<ChromeBridge *>(target)) {
        emit bridge->hostEvent(name, payload);
    } else if (auto *page = qobject_cast<ExtensionPageBridge *>(target)) {
        emit page->extEvent(name, payload);
    }
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
        + QLatin1String(")return;window.__ARKTOS_HOST_CONFIG=")
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
        + QLatin1String("||l.hash==='#arktos-healthprobe'"
                        ")return;window.__ARKTOS_HOST_CONFIG=")
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

// ------------------------------------------------------------------
// ER01 — page agent + declared content scripts.  Both ride the
// profile's script collection in the shared AgentWorldId realm (the
// page's own world never sees chrome.*); the agent installs
// __arktosExtAgents[<id>] with a scoped chrome object and each
// content script is wrapped in a closure receiving THAT extension's
// binding — the shared realm is what lets Qt's 256-world cap scale
// to many extensions.
// ------------------------------------------------------------------

// One manifest content_scripts[] entry, sanitized for injection.
struct ContentScriptEntry {
    QStringList matches;
    QStringList excludeMatches;
    QStringList jsFiles;
    QStringList cssFiles;
    QString runAt;              // document_start | document_end |
                                // document_idle
    bool allFrames = false;
    QString world;              // 'ISOLATED' (default) | 'MAIN'
};

// Read + sanitize manifest "content_scripts".  Paths get the same
// treatment as background scripts: plain relative paths only —
// scheme/absolute/traversal entries never become injected source.
QList<ContentScriptEntry> parseContentScripts(
        const QJsonObject &manifest, QStringList *matchUnion)
{
    QList<ContentScriptEntry> out;
    const QJsonArray entries =
        manifest.value(QStringLiteral("content_scripts")).toArray();
    auto cleanPaths = [](const QJsonArray &files) {
        QStringList cleaned;
        for (const QJsonValue &v : files) {
            const QString path = v.toString();
            if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
                    || path.contains(QLatin1Char(':'))
                    || path.split(QLatin1Char('/')).contains(
                        QStringLiteral("..")))
                continue;
            cleaned.append(path);
        }
        return cleaned;
    };
    for (const QJsonValue &v : entries) {
        const QJsonObject o = v.toObject();
        ContentScriptEntry entry;
        for (const QJsonValue &m :
                 o.value(QStringLiteral("matches")).toArray()) {
            const QString pattern = m.toString();
            if (!pattern.isEmpty()) {
                entry.matches.append(pattern);
                if (matchUnion
                        && !matchUnion->contains(pattern))
                    matchUnion->append(pattern);
            }
        }
        for (const QJsonValue &m :
                 o.value(QStringLiteral("exclude_matches")).toArray())
            entry.excludeMatches.append(m.toString());
        entry.jsFiles = cleanPaths(
            o.value(QStringLiteral("js")).toArray());
        entry.cssFiles = cleanPaths(
            o.value(QStringLiteral("css")).toArray());
        entry.runAt = o.value(QStringLiteral("run_at"))
            .toString(QStringLiteral("document_idle"));
        entry.allFrames =
            o.value(QStringLiteral("all_frames")).toBool(false);
        entry.world = o.value(QStringLiteral("world"))
            .toString(QStringLiteral("ISOLATED"));
        if (!entry.matches.isEmpty()
                && (!entry.jsFiles.isEmpty()
                    || !entry.cssFiles.isEmpty()))
            out.append(entry);
    }
    return out;
}

// The page-agent payload: __ARKTOS_EXT_CONFIG + src/data/extagent.js.
// hostPerms + grants (content-script match union) seed the js-side
// coverage gate; the C++ bridge re-verifies on every call.
QString agentInjectionSource(const QString &extensionId,
                             const QString &extensionName,
                             const QString &extensionPath,
                             const QJsonObject &manifest,
                             const QStringList &contentMatches)
{
    QJsonObject config;
    config.insert(QStringLiteral("id"), extensionId);
    config.insert(QStringLiteral("name"), extensionName);
    config.insert(QStringLiteral("manifest"), manifest);
    config.insert(QStringLiteral("i18n"),
                  ExtensionShimBackend::i18nMessages(extensionPath));
    QJsonArray grants;
    for (const QString &m : contentMatches)
        grants.append(m);
    config.insert(QStringLiteral("grants"), grants);
    config.insert(QStringLiteral("hostPerms"),
                  manifest.value(QStringLiteral("host_permissions")));

    QString source = QStringLiteral("(function(){")
        + QLatin1String("window.__ARKTOS_EXT_CONFIG=")
        + QString::fromUtf8(
            QJsonDocument(config).toJson(QJsonDocument::Compact))
        + QLatin1Char(';');
    source += resourceText(QStringLiteral(":extagent.js"));
    source += QLatin1String("})();");
    return source;
}

// The shared content-script wrapper: declared content scripts
// (index-numbered guard) and ER09's dynamically-registered ones
// (registration-id guard) produce the same payload — wait
// (boundedly) for this extension's agent, re-check the spec's
// matches/excludeMatches against the live url (the agent's own
// grant check already passed), then run the js inside a closure
// that binds `chrome` to THIS extension's object; css entries land
// as <style> elements deferred until a documentElement exists
// (DocumentCreation lands before <head>).
// run_at document_idle is approximated by the DocumentReady
// injection point (Qt has no separate idle point; the document is
// already interactive when it lands).
QString wrapContentScript(const QString &extensionId,
                          const QString &guardSuffix,
                          const QString &matchesJson,
                          const QString &excludesJson,
                          const QString &jsBody,
                          const QStringList &cssUnits)
{
    QString body;
    body += QLatin1String("(function(){var __key=")
        + jsLiteral(extensionId)
        + QLatin1String(";var __ent={matches:")
        + matchesJson
        + QLatin1String(",excludeMatches:")
        + excludesJson
        + QLatin1String("};")
        // Same-document double-fire guard (pushState re-commits on
        // some sites re-run DocumentCreation scripts in Chromium).
        + QLatin1String("var __guard='__arktosCsRan_'+__key+'_'")
        + QLatin1String("+")
        + jsLiteral(guardSuffix)
        + QLatin1String(";if(window[__guard])return;")
        + QLatin1String(
            "var __tries=0;"
            "function __agent(){var a=window.__arktosExtAgents;"
            "return a&&a[__key]||null;}"
            "function __gate(){var a=__agent();"
            "if(!a)return 'wait';"
            "if(!a.active)return 'stop';"
            "try{var u=String(location.href);"
            "if(a.matchAny(__ent.matches,u)"
            "&&!a.matchAny(__ent.excludeMatches,u))return 'go';"
            "}catch(e){}"
            "return 'stop';}"
            "(function __when(){"
            "var g=__gate();"
            "if(g==='go'){window[__guard]=true;__run();return;}"
            "if(g==='wait'&&++__tries<400){"
            "setTimeout(__when,10);return;}"
            "})();"
            "function __run(){"
            "var __a=__agent();if(!__a)return;"
            "try{__css();}catch(e){"
            "try{__a.errors.push('cs-css: '+String(e&&e.message||e));"
            "}catch(_){}}"
            "try{(function(chrome,browser){"
            );

    body += jsBody;
    body += QLatin1String(
        "})(__a.chrome,__a.chrome);}catch(e){"
        "try{__a.errors.push('cs: '+String(e&&e.message||e)"
        "+(e&&e.stack?' @'+String(e.stack).split('\\n')"
        ".slice(1,4).join(' <-'):''));}catch(_){}}"
        "}"
        // css — the spec's css[] becomes <style> elements,
        // deferred until a documentElement exists.
        "function __css(){"
        "var __styles=[");
    bool first = true;
    for (const QString &unit : cssUnits) {
        if (!first)
            body += QLatin1Char(',');
        first = false;
        body += jsLiteral(unit);
    }
    body += QLatin1String(
        "];"
        "if(!__styles.length)return;"
        "function __ins(){var h=document.head"
        "||document.documentElement;"
        "if(!h){setTimeout(__ins,10);return;}"
        "for(var i=0;i<__styles.length;++i){"
        "var s=document.createElement('style');"
        "s.textContent=__styles[i];h.appendChild(s);}}"
        "__ins();}"
        "})();");
    return body;
}

QString jsonArrayOfStrings(const QStringList &list)
{
    QJsonArray arr;
    for (const QString &s : list)
        arr.append(s);
    return QString::fromUtf8(
        QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// A declared manifest content_scripts[] entry wrapped for the
// shared agent world.
QString contentScriptSource(const QString &extensionId,
                            int index,
                            const QString &extensionPath,
                            const ContentScriptEntry &entry)
{
    // The extension's own files, inlined in manifest order — the
    // same read+merge the background loader uses (lazy webpack
    // chunks ride along in the synchronous pass).
    const QString code =
        extensionCodeSource(extensionPath, entry.jsFiles);
    QStringList cssUnits;
    const QDir dir(extensionPath);
    for (const QString &rel : entry.cssFiles) {
        QFile file(dir.absoluteFilePath(rel));
        if (file.open(QIODevice::ReadOnly))
            cssUnits.append(QString::fromUtf8(file.readAll()));
    }
    return wrapContentScript(extensionId, QString::number(index),
                             jsonArrayOfStrings(entry.matches),
                             jsonArrayOfStrings(entry.excludeMatches),
                             code, cssUnits);
}
}

QUrl ExtensionBackgroundHost::hostUrl(const QString &extensionId)
{
    return QUrl(QStringLiteral(
        "chrome-extension://%1/manifest.json#arktos-hosted")
        .arg(extensionId));
}

QString ExtensionBackgroundHost::injectionName(const QString &extensionId)
{
    return QStringLiteral("arktos-exthost-") + extensionId;
}

QString ExtensionBackgroundHost::pageInjectionName(
    const QString &extensionId)
{
    return QStringLiteral("arktos-extpage-") + extensionId;
}

QString ExtensionBackgroundHost::pageAgentInjectionName(
    const QString &extensionId)
{
    return QStringLiteral("arktos-extagent-") + extensionId;
}

QString ExtensionBackgroundHost::contentScriptInjectionName(
    const QString &extensionId, int index)
{
    return QStringLiteral("arktos-extcs-") + extensionId
        + QLatin1Char('-') + QString::number(index);
}

QString ExtensionBackgroundHost::dynamicContentScriptInjectionName(
        const QString &extensionId, const QString &registrationId)
{
    // Registration ids are caller-chosen strings — the script name
    // needs a tame charset for prefix sweeps.
    QString safe;
    for (const QChar c : registrationId) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_')
                || c == QLatin1Char('-'))
            safe.append(c);
        else
            safe.append(QLatin1Char('_'));
        if (safe.size() >= 96)
            break;
    }
    if (safe.isEmpty())
        safe = QStringLiteral("_");
    return QStringLiteral("arktos-extdcs-") + extensionId
        + QLatin1Char('-') + safe;
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
    m_page = new HostedBackgroundPage(profile, extensionId,
                                      hostUrl(extensionId), m_view);
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
                    "if(window.__arktosHost)"
                        "window.__arktosHost.scriptsLoaded=true;"
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

        // ER01 — the page agent + this extension's declared content
        // scripts, profile-wide in the shared AgentWorldId isolated
        // realm.  The agent installs a scoped chrome.* only where
        // the extension's host_permissions/content-script matches
        // cover the document; the ExtensionPageBridge on each
        // WebPage re-verifies the coverage per call and routes
        // chrome.runtime.sendMessage/connect traffic here.
        const QJsonObject manifest =
            m_bridge->backend()
                ? m_bridge->backend()->manifestJson()
                : QJsonObject();
        const QList<ContentScriptEntry> contentScripts =
            parseContentScripts(manifest, &m_csMatches);
        QWebEngineScript agent;
        agent.setName(pageAgentInjectionName(extensionId));
        agent.setInjectionPoint(QWebEngineScript::DocumentCreation);
        agent.setWorldId(
            ExtensionBackgroundRuntime::AgentWorldId);
        agent.setRunsOnSubFrames(true);
        agent.setSourceCode(
            agentInjectionSource(extensionId, extensionName,
                                 extensionPath, manifest,
                                 m_csMatches));
        profile->scripts()->insert(agent);

        for (int i = 0; i < contentScripts.size(); ++i) {
            const ContentScriptEntry &entry = contentScripts.at(i);
            QWebEngineScript cs;
            cs.setName(contentScriptInjectionName(extensionId, i));
            cs.setInjectionPoint(
                entry.runAt == QLatin1String("document_start")
                    ? QWebEngineScript::DocumentCreation
                    : QWebEngineScript::DocumentReady);
            // world:'MAIN' is approximated by the shared isolated
            // realm (more isolation, same chrome binding) — the
            // distinction is recorded, not silently dropped.
            cs.setWorldId(ExtensionBackgroundRuntime::AgentWorldId);
            cs.setRunsOnSubFrames(entry.allFrames);
            cs.setSourceCode(
                contentScriptSource(extensionId, i, extensionPath,
                                    entry));
            profile->scripts()->insert(cs);
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
        const QString csPrefix =
            QStringLiteral("arktos-extcs-") + m_extensionId
            + QLatin1Char('-');
        // ER09: the dynamically-registered set dies with the host
        // too — the specs themselves outlive it in the registry
        // (and ProfileSettings for persisted ones) so a respawn
        // re-arms them.
        const QString dcsPrefix =
            QStringLiteral("arktos-extdcs-") + m_extensionId
            + QLatin1Char('-');
        for (const QWebEngineScript &script : installed) {
            if (script.name() == name
                || script.name() == name + QLatin1String("-code")
                || script.name() == pageInjectionName(m_extensionId)
                || script.name() == pageAgentInjectionName(m_extensionId)
                || script.name().startsWith(csPrefix)
                || script.name().startsWith(dcsPrefix))
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
            "window.__arktosHost?JSON.stringify({s:__arktosHost.state,"
            "b:__arktosHost.bridgeUp,l:__arktosHost.scriptsLoaded,"
            "e:__arktosHost.errors.slice(-1)}):null"),
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
        QStringLiteral("window.__arktosWake&&__arktosWake()"));
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
    // ER10: the offscreen document dies with the extension — Chrome
    // closes an extension's pages on unload, and its bridge's backend
    // just died anyway.
    dropOffscreenDocument(id);
    // ER11: unload sweeps every webRequest listener the extension
    // registered — Chrome binds listeners to the extension, not to
    // a surviving context.
    ExtensionRequestWatcher::instance()->dropExtension(id);
    // ER12: Chrome dismisses an extension's notifications when it
    // unloads — records drop (no onClosed: no context could hear it)
    // and presented banners are closed.
    ExtensionNotificationCenter::instance()->dropExtension(id);
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

    // ER14 — chrome.commands bindings: the WHOLE desired set feeds
    // the shortcut registry, hosted or not — _execute_action opens
    // the action popup without needing any background context.
    // Departed ids drop their bindings inside the sync.
    {
        QHash<QString, QString> commandPaths;
        for (auto it = desired.constBegin(); it != desired.constEnd();
                ++it)
            commandPaths.insert(it.key(), it.value().path);
        ExtensionCommandRegistry::instance()->syncExtensions(
            commandPaths);
    }
    // ER09: session-only registrations (persistAcrossSessions:false)
    // die with the extension's loaded lifetime; persisted ones keep
    // the ProfileSettings record for the next session.
    const QStringList regIds = m_regScripts.keys();
    for (const QString &id : regIds) {
        if (desired.contains(id))
            continue;
        QHash<QString, QJsonObject> &specs = m_regScripts[id];
        for (auto it = specs.begin(); it != specs.end();) {
            if (!it.value().value(QStringLiteral(
                        "persistAcrossSessions")).toBool(true))
                it = specs.erase(it);
            else
                ++it;
        }
        if (specs.isEmpty())
            m_regScripts.remove(id);
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
        // manifest's declared permissions.  ER16: REQUIRED only —
        // manifest.permissions merges optional_permissions in, and an
        // optional grant is inert until chrome.permissions.request
        // consents it (typed unsupported in this runtime), so it must
        // never unlock a gated call or event family.
        m_extPermissions.insert(id, manifest.requiredPermissions);
        m_extHostPermissions.insert(id, manifest.hostPermissions);
        if (!want.profile)
            continue;
        auto *host = new ExtensionBackgroundHost(
            want.profile, id, want.name, want.path,
            manifest.backgroundScripts, this);
        connect(host, &ExtensionBackgroundHost::stateChanged, this,
                [this, id]() {
            emit hostChanged(id);
            // ER05 — a host reaching Ready is the extension's
            // "loaded" moment: the lifecycle dispatch reads the
            // version record and fires onInstalled/onStartup.
            ExtensionBackgroundHost *h = m_hosts.value(id);
            if (h && h->state() == ExtensionBackgroundHost::State::Ready) {
                dispatchRuntimeLifecycle(id);
                // ER14: command declarations parsed before the host
                // existed land their fidelity notes now.
                ExtensionCommandRegistry::instance()->flushNotes(id);
            }
        });
        m_hosts.insert(id, host);
        emit hostChanged(id);
        // ER09: re-arm the extension's dynamic content-script
        // registrations (persisted + session) onto the new host's
        // profile.
        restoreRegisteredScripts(id);
    }

    // EXT19: re-sweep — a popup whose document committed while no
    // host existed yet retries its attach on this pass, and views an
    // event could never reach (parentless, never shown) are picked up
    // each heartbeat.
    watchExtensionViews();
#endif // QT_CONFIG(webengine_extensions)
}

// -------------------------------------------------------------------
// ER05 — runtime.onInstalled / runtime.onStartup.  Chrome fires
// onInstalled when an extension first installs, when its version
// changes, and when the browser itself updates; onStartup when a
// profile carrying the extension starts up — never for a fresh
// install's own session and never for a mid-session respawn.  The
// version record in ProfileSettings — extensions/runtime/<id> =
// {extension version, browser version}, rewritten whenever a boot
// observes a difference — is the comparison base; uninstall clears
// it (ExtensionManager) so a reinstall is a fresh install again.
//
// Emission rides the shared backend's pushEvent — every shim
// context of this extension (the hosted background plus any sibling
// pages, matching Chrome's per-context dispatch) receives it
// through its bridge's hostEvent.  There is no relay-registration
// wait: Ready itself implies the page-side hostEvent connect is
// already wired, and runtime.* events are ungated like the alarms
// pushes.
// -------------------------------------------------------------------

void ExtensionBackgroundRuntime::dispatchRuntimeLifecycle(
        const QString &extensionId)
{
    ExtensionBackgroundHost *host = m_hosts.value(extensionId);
    if (!host || host->state() != ExtensionBackgroundHost::State::Ready
            || !host->bridge() || !host->bridge()->backend())
        return;
    ExtensionShimBackend *backend = host->bridge()->backend();

    const QString profileId = BrowserProfile::profileIdFor(
        host->page() ? host->page()->profile() : nullptr);
    ProfileSettings settings(profileId);
    const QString key = QStringLiteral("extensions/runtime/")
        + extensionId;
    const QStringList record = settings.value(key).toStringList();
    const QString prevVersion = record.value(0);
    const QString prevBrowser = record.value(1);
    const QString extVersion = backend->manifestJson()
        .value(QStringLiteral("version")).toString();
    const QString appVersion =
        QCoreApplication::applicationVersion();

    QString reason;
    if (record.isEmpty())
        reason = QStringLiteral("install");
    else if (prevVersion != extVersion)
        reason = QStringLiteral("update");
    else if (prevBrowser != appVersion)
        reason = QStringLiteral("browser_update");

    if (!reason.isEmpty()) {
        QJsonObject details{
            {QStringLiteral("reason"), reason}};
        if (reason == QLatin1String("update"))
            details.insert(QStringLiteral("previousVersion"),
                           prevVersion);
        emit backend->pushEvent(
            QStringLiteral("runtime.onInstalled"),
            QJsonObject{{QStringLiteral("__args"),
                         QJsonArray{details}}});
        settings.setValue(key,
                          QStringList{extVersion, appVersion});
    }
    // onStartup answers a profile startup with the extension already
    // installed — the record predating this dispatch is what marks
    // it (a fresh install wrote none before Ready and never sees the
    // event on its own session).  The per-session set keeps respawns
    // and mid-session re-enables from re-firing; an enable of an
    // extension that was enabled last session does report startup
    // here — a recorded approximation of Chrome's strict profile-
    // startup-only rule, harmless in practice since the listener
    // re-runs the same session init a real startup would.
    if (!record.isEmpty() && !m_lifecycleSeen.contains(extensionId)) {
        emit backend->pushEvent(
            QStringLiteral("runtime.onStartup"),
            QJsonObject{{QStringLiteral("__args"), QJsonArray()}});
    }
    m_lifecycleSeen.insert(extensionId);
    host->nudge();
}

void ExtensionBackgroundRuntime::restartLifecycleSession()
{
    m_lifecycleSeen.clear();
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
    if (!view->property("arktosExtPageWatch").toBool()) {
        view->setProperty("arktosExtPageWatch", true);
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
        if (!ctx.bridge || ctx.extensionId != extensionId)
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
        // ER15: a context whose page lives in a real browser tab
        // answers that tab's ids like Chrome does; hidden/detached
        // contexts keep -1.
        int tabId = -1, windowId = -1;
        QWebEnginePage *ctxPage = ctx.bridge->contextPage();
        if (ctxPage) {
            for (BrowserMainWindow *window
                 : ExtensionEventHub::browsingWindows()) {
                if (!window || !window->tabWidget())
                    continue;
                TabWidget *tw = window->tabWidget();
                for (int i = 0; i < tw->count(); ++i) {
                    WebView *view = tw->webView(i);
                    if (ExtensionEventHub::trackable(view)
                            && view->page() == ctxPage) {
                        tabId = ExtensionShimBackend::tabIdFor(view);
                        windowId =
                            ExtensionShimBackend::windowIdFor(window);
                        break;
                    }
                }
                if (tabId >= 0)
                    break;
            }
        }
        entry.insert(QStringLiteral("tabId"), tabId);
        entry.insert(QStringLiteral("windowId"), windowId);
        out.append(entry);
    }
    return out;
}

void ExtensionBackgroundRuntime::pushContexts(
        const QString &extensionId)
{
    if (extensionId.isEmpty())
        return;
    QJsonObject payload;
    payload.insert(QStringLiteral("contexts"),
                   contextsJson(extensionId));
    for (const RelayContext &ctx : m_relayContexts) {
        if (ctx.extensionId == extensionId && ctx.bridge) {
            emit ctx.bridge->hostEvent(
                QStringLiteral("__arktos.contexts"), payload);
            nudgeBridge(ctx.bridge);
        }
    }
}

QJsonObject ExtensionBackgroundRuntime::reloadContextPage(
        const QString &extensionId, const QString &contextId)
{
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
    for (const RelayContext &ctx : m_relayContexts) {
        if (ctx.extensionId != extensionId
                || ctx.contextId != contextId)
            continue;
        QWebEnginePage *page = ctx.bridge
            ? ctx.bridge->contextPage() : nullptr;
        if (!page)
            return err(QStringLiteral("no-page"));
        page->triggerAction(QWebEnginePage::Reload);
        return ok();
    }
    return err(QStringLiteral("not-found"));
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
    // ER10: the runtime — not the page — knows a context is the
    // offscreen document; pin its kind so getContexts reports
    // OFFSCREEN_DOCUMENT and a pending createDocument can resolve.
    QString contextKind = kind;
    const bool isOffscreen = isOffscreenBridge(bridge);
    if (isOffscreen)
        contextKind = QStringLiteral("offscreen");
    for (int i = 0; i < m_relayContexts.size(); ++i) {
        if (m_relayContexts.at(i).bridge == bridge) {
            // A re-register refreshes kind/url (the page may learn
            // its role after attach).
            m_relayContexts[i].kind = contextKind;
            if (!url.isEmpty())
                m_relayContexts[i].url = url;
            if (isOffscreen)
                resolveOffscreenOps(bridge);
            pushContexts(bridge->extensionId());
            return m_relayContexts.at(i).contextId;
        }
    }
    RelayContext ctx;
    ctx.bridge = bridge;
    ctx.extensionId = bridge->extensionId();
    ctx.kind = contextKind;
    ctx.url = url;
    ctx.contextId = bridge->extensionId() + QLatin1Char('|')
        + contextKind + QLatin1Char('|')
        + QString::number(m_nextContextId++);
    m_relayContexts.append(ctx);
    if (isOffscreen)
        resolveOffscreenOps(bridge);
    // ER15: every sibling's synchronous getViews cache learns the
    // set changed — including the just-registered context itself.
    pushContexts(bridge->extensionId());
    return ctx.contextId;
}

void ExtensionBackgroundRuntime::unregisterRelayContext(
        ChromeBridge *bridge)
{
    // ER15: removed contexts' extensions get a fresh '__arktos.
    // contexts' push below — the stored id survives a dead bridge.
    QStringList changedExts;
    for (int i = m_relayContexts.size() - 1; i >= 0; --i) {
        if (!m_relayContexts.at(i).bridge
                || m_relayContexts.at(i).bridge == bridge) {
            const QString extId = m_relayContexts.at(i).extensionId;
            if (!extId.isEmpty() && !changedExts.contains(extId))
                changedExts.append(extId);
            m_relayContexts.removeAt(i);
        }
    }
    for (const QString &extId : changedExts)
        pushContexts(extId);
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
    // Host->page deliveries this bridge was waiting on die with it —
    // a late page response finds no slot.
    const QStringList dmids = m_pageDeliveries.keys();
    for (const QString &mid : dmids) {
        if (m_pageDeliveries.value(mid).waiter == bridge) {
            m_pageDeliveries.remove(mid);
            if (QTimer *t = m_pageDeliveryTimers.take(mid))
                t->deleteLater();
        }
    }
    // Port ends owned by this bridge die with it; remaining ends get
    // the disconnect push so their onDisconnect fires honestly.
    const QStringList chans = m_portEnds.keys();
    for (const QString &chan : chans) {
        QList<PortEnd> &ends = m_portEnds[chan];
        bool removed = false;
        for (int i = ends.size() - 1; i >= 0; --i) {
            if (!ends.at(i).target || ends.at(i).target == bridge) {
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
            if (end.target) {
                emitToEnd(end.target,
                          QStringLiteral("__arktos.portDisconnect"),
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
        emit ctx.bridge->hostEvent(QStringLiteral("__arktos.relay"),
                                   QJsonObject{
                                       {QStringLiteral("mid"), mid},
                                       {QStringLiteral("msg"), envelope}});
        ++peers;
        // A popup->background send lands on a throttled hidden page —
        // nudge it awake like the EXT16 event fan-out does.
        nudgeBridge(ctx.bridge);
    }
    if (peers > 0) {
        PendingRelay pending;
        pending.origin = from;
        pending.extensionId = from->extensionId();
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
               != it.value().extensionId)
        return false;
    PendingRelay &pending = it.value();
    ++pending.replies;
    const bool defined =
        !response.isUndefined() && !response.isNull();
    // First real response wins (Chrome parity); empty peer replies
    // only resolve once every peer has answered.
    if (!defined && pending.replies < pending.peers)
        return true;
    QPointer<QObject> origin = pending.origin;
    m_relayPending.erase(it);
    if (QTimer *t = m_relayPendingTimers.take(mid))
        t->deleteLater();
    const QJsonObject reply{
        {QStringLiteral("mid"), mid},
        {QStringLiteral("response"),
         defined ? response : QJsonValue(QJsonValue::Null)}};
    // ER01: a page-originated send resolves on the agent's extEvent
    // channel; a shim context's on hostEvent like before.
    emitToEnd(origin,
              qobject_cast<ExtensionPageBridge *>(origin.data())
                  ? QStringLiteral("__arktos.extResponse")
                  : QStringLiteral("__arktos.relayResponse"),
              reply);
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
            QStringLiteral("__arktos.portConnect"),
            QJsonObject{{QStringLiteral("chan"), chan},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("sender"), sender}});
        ++peers;
        nudgeBridge(ctx.bridge);
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
        if (end.target == accepter && end.localPortId == portId)
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
        if (!end.target)
            continue;
        // Ends landed on this chan through prefix-checked registers
        // only — the chan's "<extId>#" scope is the extension
        // boundary for shim contexts and page ends alike.
        if (end.target == from && end.localPortId == portId)
            continue;   // the sending end itself
        emitToEnd(end.target,
                  QStringLiteral("__arktos.portMessage"),
                  QJsonObject{{QStringLiteral("portId"),
                               end.localPortId},
                              {QStringLiteral("msg"), msg}});
        ++delivered;
        if (auto *cb = qobject_cast<ChromeBridge *>(end.target))
            nudgeBridge(cb);
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
        if (ends.at(i).target == from
                && ends.at(i).localPortId == portId)
            ends.removeAt(i);
    }
    if (ends.isEmpty()) {
        m_portEnds.erase(it);
        return;
    }
    for (const PortEnd &end : std::as_const(ends)) {
        if (end.target) {
            emitToEnd(end.target,
                      QStringLiteral("__arktos.portDisconnect"),
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
// Host-internal pushes (__arktos.*, alarms) keep their ungated path.
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
    // ER11: same Chrome parity — no 'webRequest' permission, no
    // event surface.
    if (ns == QLatin1String("webRequest"))
        return QStringLiteral("webRequest");
    // ER12: notification events ride the same gate create() applies.
    if (ns == QLatin1String("notifications"))
        return QStringLiteral("notifications");
    // ER13: same Chrome parity — 'idle' gates the whole API,
    // onStateChanged included.
    if (ns == QLatin1String("idle"))
        return QStringLiteral("idle");
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
    QString phost = slash < 0 ? rest : rest.left(slash);
    const QString ppath =
        slash < 0 ? QStringLiteral("/*") : rest.mid(slash);
    // Chrome lets a pattern pin a port ('127.0.0.1:*') — split it
    // off before the host compare.
    QString pport;
    const int colon = phost.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) {
        pport = phost.mid(colon + 1);
        phost = phost.left(colon);
    }
    if (pscheme == QLatin1String("*")) {
        // '*://' covers http + https only (Chrome semantics).
        if (scheme != QLatin1String("http")
                && scheme != QLatin1String("https"))
            return false;
    } else if (pscheme != scheme) {
        return false;
    }
    if (!pport.isEmpty() && pport != QLatin1String("*")) {
        const int defPort = scheme == QLatin1String("https") ? 443
            : scheme == QLatin1String("http") ? 80 : -1;
        if (url.port(defPort) != pport.toInt())
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
    // ER16: contextMenus' OnClickData names the same page location
    // 'pageUrl'/'frameUrl' — scrubbed under the identical rule.  The
    // click-target fields (linkUrl/srcUrl/selectionText) stay: the
    // user's own gesture disclosed them, matching Chrome's fill.
    object.remove(QStringLiteral("pageUrl"));
    object.remove(QStringLiteral("frameUrl"));
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

bool ExtensionBackgroundRuntime::hasExtensionPermission(
        const QString &extensionId, const QString &permission) const
{
    return m_extPermissions.value(extensionId)
        .contains(permission);
}

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

// ER11: Chrome scopes webRequest events by host_permissions — a
// listener's url filter alone grants nothing.  The 'tabs' shortcut
// deliberately does NOT apply: it covers tab metadata, not request
// traffic.
bool ExtensionBackgroundRuntime::requestVisibleTo(
        const QString &extensionId, const QUrl &url) const
{
    for (const QString &pattern :
             m_extHostPermissions.value(extensionId)) {
        if (hostCovers(pattern, url))
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
            && ns != QLatin1String("contextMenus")
            // ER14: commands.onCommand and action.onClicked both
            // carry a tab object — same url-field rule.
            && ns != QLatin1String("commands")
            && ns != QLatin1String("action"))
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
        const QString &onlyExtensionId, ChromeBridge *exclude)
{
    const QString gate = eventGate(name);
    int delivered = 0;
    for (const RelayContext &ctx : m_relayContexts) {
        ChromeBridge *bridge = ctx.bridge;
        if (!bridge || bridge == exclude)
            continue;
        const QString extId = bridge->extensionId();
        if (!onlyExtensionId.isEmpty() && extId != onlyExtensionId)
            continue;
        if (!gate.isEmpty()
                && !m_extPermissions.value(extId).contains(gate))
            continue;
        emit bridge->hostEvent(name, shapedPayload(extId, name, payload));
        ++delivered;
        // Push-then-wake — a throttled hidden page (host or
        // offscreen document) drains the delivery promptly instead
        // of waiting for its own nudge timer's next tick.
        nudgeBridge(bridge);
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
        const QJsonObject &payload, ChromeBridge *exclude)
{
    return fanOut(name, payload, extensionId, exclude);
}

void ExtensionBackgroundRuntime::fanOutStorageChanged(
        const QString &extensionId, const QString &area,
        const QJsonObject &changes, ChromeBridge *exclude)
{
    const QJsonObject payload{
        {QStringLiteral("__args"), QJsonArray{changes, area}}};
    deliverEvent(extensionId, QStringLiteral("storage.onChanged"),
                 payload, exclude);
    deliverEvent(extensionId,
                 QStringLiteral("storage.") + area
                     + QStringLiteral(".onChanged"),
                 payload, exclude);
}

// -------------------------------------------------------------------
// ER01 — page agent plumbing.  Each WebPage carries an 'arktosExt'
// ExtensionPageBridge on its ArktosChannelWorld channel; the
// per-extension agent running in AgentWorldId reaches it through
// the SEC24 whitelisted postMessage forwarder.  agentCovers() is the
// security boundary — the forwarder is reachable by ANY world on
// the page, so every op re-proves the named extension's grants
// against the live page url.  A denied page gets the typed
// 'no-host-access' absent-API reply, matching what Chrome surfaces
// as "extension not present".
// -------------------------------------------------------------------

bool ExtensionBackgroundRuntime::isHosted(
        const QString &extensionId) const
{
    return m_hosts.value(extensionId) != nullptr;
}

bool ExtensionBackgroundRuntime::agentCovers(
        const QString &extensionId, const QUrl &pageUrl) const
{
    const QString scheme = pageUrl.scheme();
    if (scheme.isEmpty())
        return false;
    QStringList patterns = m_extHostPermissions.value(extensionId);
    if (ExtensionBackgroundHost *host =
            m_hosts.value(extensionId))
        patterns += host->contentScriptMatches();
    if (patterns.isEmpty())
        return false;
    // Originless documents (data:, about:) get no coverage — Chrome
    // semantics, and here also a measured transport fact: WebPage's
    // page-level channel bootstrap does not inject there (verified
    // with a world probe), so no forwarder could ever carry the call.
    for (const QString &pattern : patterns) {
        if (hostCovers(pattern, pageUrl))
            return true;
    }
    return false;
}

namespace {

QJsonObject pageOk(const QJsonValue &v = QJsonValue())
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    if (!v.isUndefined())
        o.insert(QStringLiteral("result"), v);
    return o;
}

QJsonObject pageErr(const QString &code)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    return o;
}

} // namespace

// ER02 — chrome.tabs.sendMessage: the host->page direction of the
// hosted runtime's messaging.  The tab id resolves through the shim
// backend's live-tab registry; the push rides the page's arktosExt
// extEvent channel to the page agent, which fires runtime.onMessage
// in the extension's isolated content-script realm and answers over
// the 'respond' op — the reply reaches this bridge as
// '__arktos.deliverResponse'.
//
// agentCovers is the security boundary and runs BEFORE any push:
// extEvent reposts to window.postMessage('*') — every world on the
// page, hostile main-world script included, sees whatever we emit —
// so an extension without coverage on the committed url gets the
// typed denial and the page never sees its traffic.  (Defined down
// here with the page plumbing: it needs ownsRelayKey + emitToEnd's
// sibling helpers from the anonymous namespace above.)
QJsonObject ChromeBridge::invokeTabMessage(const QJsonObject &args)
{
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
    const QString mid = args.value(QStringLiteral("mid")).toString();
    const QJsonValue msg = args.value(QStringLiteral("msg"));
    if (mid.isEmpty() || msg.isUndefined()
            || !ownsRelayKey(m_extensionId, mid))
        return err(QStringLiteral("invalid-argument"));
    // options.frameId: the page's channel forwarder lives in the
    // main frame only — subframe addressing is a documented gap,
    // answered typed rather than silently mis-delivered to frame 0.
    const QJsonObject opts =
        args.value(QStringLiteral("opts")).toObject();
    if (opts.contains(QStringLiteral("frameId"))
            && int(opts.value(QStringLiteral("frameId"))
                       .toDouble(-1)) != 0)
        return err(QStringLiteral("unsupported"));
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    WebView *view = ExtensionShimBackend::viewForTabId(
        int(args.value(QStringLiteral("id")).toDouble(-1)));
    if (!view || !view->page())
        return err(QStringLiteral("not-found"));
    if (!runtime->agentCovers(m_extensionId, view->page()->url()))
        return err(QStringLiteral("no-host-access"));
    ExtensionPageBridge *page =
        view->page()->findChild<ExtensionPageBridge *>();
    if (!page)
        return err(QStringLiteral("no-receiver"));
    // A Chrome-shaped MessageSender for an extension context: the
    // id is trusted from the bridge; url is the calling document's
    // real url minus our internal marker fragment; no .tab field.
    QString senderUrl;
    if (m_contextPage) {
        QUrl u = m_contextPage->url();
        u.setFragment(QString());
        senderUrl = u.toString();
    }
    const QJsonObject sender{
        {QStringLiteral("id"), m_extensionId},
        {QStringLiteral("url"), senderUrl},
        {QStringLiteral("origin"),
         QStringLiteral("chrome-extension://") + m_extensionId}};
    runtime->pageDeliver(page, m_extensionId, mid, msg, sender, this);
    return ok(QJsonObject{{QStringLiteral("delivered"), true}});
}

// -------------------------------------------------------------------
// ER07/ER08 — chrome.scripting.executeScript + insertCSS/removeCSS:
// the product API.  The call resolves a tab through the same registry
// chrome.tabs uses, gates on the extension's declared 'scripting'
// permission and its host_permissions covering the committed url —
// BEFORE any source is generated into the page — then fans out over
// the frame tree via QWebEngineFrame::runJavaScript.
//
// World emulation: 'ISOLATED' (default) runs in AgentWorldId — the
// shared agent realm — inside a closure that binds `chrome`/`browser`
// to THIS frame's scoped agent object, the exact surface declared
// content scripts get.  'MAIN' runs in the page's own world with no
// extension surface, matching Chrome where a main-world injection is
// unprivileged.  Frame ids are traversal positions over the CURRENT
// frame tree (0 = main frame, children depth-first) — unlike Chrome's
// per-document ids they renumber if the tree mutates; callers only
// ever learn them from our own results and event payloads.
// insertCSS/removeCSS share every gate above and the same fan-out;
// their payload is a ledger-keyed <style> upsert/removal in the agent
// world (see cssSource) and they resolve void like Chrome — a
// frame-level failure rejects the call, unlike executeScript whose
// per-frame error field is part of the resolved value.
//
// runJavaScript does not await a returned Promise (measured — see the
// mv3-health smoke comment in main.cpp), so a thenable result parks
// on the frame's __arktosExecResults registry under the exec key and
// this side polls for a bounded window.  Chrome resolves the call
// once injection has started even when the script throws, so a
// throwing func lands as a per-frame {frameId, error} entry rather
// than rejecting the whole call — the documented InjectionResult
// shape; only injection-level failures (bad tab, no coverage, bad
// args) reject typed.
// -------------------------------------------------------------------

namespace {

// The frame tree as a depth-first list — index 0 is the main frame
// and a frame's position IS the frame id this call answers for.
// coverUrls rides alongside: each frame's EFFECTIVE url for the
// per-frame host-coverage check (ER16) — an about: or url-less frame
// inherits its embedder's origin (Chrome's match check runs on that,
// never the literal spelling), and a blob: url names its owning
// origin inside itself.  data:/javascript: keep their own url —
// originless, never covered.
void collectFrames(const QWebEngineFrame &frame,
                   const QUrl &inherited,
                   QList<QWebEngineFrame> *out,
                   QList<QUrl> *coverUrls)
{
    QUrl effective = frame.url();
    const QString scheme = effective.scheme();
    if (scheme.isEmpty() || scheme == QLatin1String("about"))
        effective = inherited;
    else if (scheme == QLatin1String("blob"))
        effective = QUrl(effective.toString().mid(5));
    out->append(frame);
    coverUrls->append(effective);
    const QList<QWebEngineFrame> children = frame.children();
    for (const QWebEngineFrame &child : children)
        collectFrames(child, effective, out, coverUrls);
}

// The injected source.  `func` evaluates INSIDE a closure whose
// chrome/browser params carry the frame's scoped agent binding —
// identifier resolution is lexical, so the payload sees the emulated
// surface exactly like a declared content script does (and in the
// MAIN world, where no agent registry exists, the page's own chrome
// stub answers — Chrome parity).  A thenable result parks on
// window.__arktosExecResults for the poll below; a JSON-incompatible
// value drops to the honest result-less entry.
QString execSource(const QString &extensionId,
                   const QString &funcSource,
                   const QJsonArray &execArgs,
                   const QStringList &fileSources,
                   const QString &key)
{
    QString argsJson = QString::fromUtf8(
        QJsonDocument(execArgs).toJson(QJsonDocument::Compact));
    // JSON permits raw U+2028/2029 — JS source does not.
    argsJson.replace(QChar(0x2028), QStringLiteral("\\u2028"))
        .replace(QChar(0x2029), QStringLiteral("\\u2029"));
    QString source = QStringLiteral(
        "(function(){var __key=%1;var __a=null;"
        "try{__a=(window.__arktosExtAgents||{})[%2]||null;}catch(e){}"
        "var __c=__a&&__a.chrome?__a.chrome"
            ":(typeof chrome!=='undefined'?chrome:undefined);"
        "try{var __r=(function(chrome,browser){")
        .arg(jsLiteral(key), jsLiteral(extensionId));
    if (!funcSource.isEmpty()) {
        source += QStringLiteral("var __f=(") + funcSource
            + QStringLiteral(");return __f.apply(null,") + argsJson
            + QLatin1String(");");
    } else {
        // files: the merged source runs once inside the closure —
        // a function body has no completion value, so the result is
        // honestly absent where Chrome reports the last file's.
        for (const QString &file : fileSources) {
            source += file;
            source += QLatin1Char('\n');
        }
    }
    source += QLatin1String(
        "}).call(window,__c,__c);"
        "if(__r&&typeof __r.then==='function'){"
        "var __reg=window.__arktosExecResults="
            "window.__arktosExecResults||{};"
        "__reg[__key]=null;"
        "__r.then(function(v){var o={ok:true};"
        "if(v!==undefined){try{JSON.stringify(v);o.v=v;}catch(e){}}"
        "__reg[__key]=o;},"
        "function(e){__reg[__key]={ok:false,"
            "e:String(e&&e.message||e)};});"
        "return {__exec:1,pending:true};}"
        "var o={__exec:1,ok:true};"
        "if(__r!==undefined){try{JSON.stringify(__r);o.v=__r;}"
            "catch(e){}}"
        "return o;}catch(e){"
        "return {__exec:1,ok:false,e:String(e&&e.message||e)};}})()");
    return source;
}

QJsonObject execResultEntry(int frameId, const QJsonObject &envelope)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("frameId"), frameId);
    if (envelope.value(QLatin1String("ok")).toBool()) {
        if (envelope.contains(QLatin1String("v")))
            entry.insert(QStringLiteral("result"),
                         envelope.value(QLatin1String("v")));
    } else {
        entry.insert(QStringLiteral("error"),
                     envelope.value(QLatin1String("e")).toString());
    }
    return entry;
}

// Shared files[] loader — bundle-relative paths only, the same
// hygiene the background loader applies: no absolute paths, schemes,
// or traversal; size caps keep a hostile bundle from parking the
// browser on a multi-GB read.  Answers the typed error code or empty.
QString loadBundleFiles(const QString &extensionPath,
                        const QJsonArray &filesArg, QStringList *out)
{
    qint64 total = 0;
    const QDir dir(extensionPath);
    for (const QJsonValue &v : filesArg) {
        const QString rel = v.toString();
        if (rel.isEmpty() || rel.startsWith(QLatin1Char('/'))
                || rel.contains(QLatin1Char(':'))
                || rel.split(QLatin1Char('/'))
                       .contains(QStringLiteral("..")))
            return QStringLiteral("bad-path");
        QFile file(dir.absoluteFilePath(rel));
        total += file.size();
        if (file.size() > 4 * 1024 * 1024 || total > 8 * 1024 * 1024
                || !file.open(QIODevice::ReadOnly))
            return QStringLiteral("no-such-file");
        out->append(QString::fromUtf8(file.readAll()));
    }
    return QString();
}

// ER08 — the insertCSS/removeCSS payload.  Runs in the agent world so
// the per-frame ledger (window.__arktosCss) stays out of page reach;
// each css text unit keys one <style> element.  insert upserts its
// elements — Chrome dedupes a repeated key rather than stacking
// duplicate sheets — and remove drops exactly the matching keys, a
// never-inserted key being Chrome's silent no-op.  The ledger dies
// with the document, matching Chrome where injected sheets live on
// the committed document and vanish on navigation.
//
// 'USER' origin: a DOM <style> can only sit at author level — the
// real user cascade level is unreachable from script — so origin
// scopes the removal key (Chrome's observable contract) while the
// sheet lands at author level; the approximation is recorded here,
// not hidden.
QString cssSource(bool insert, const QString &origin,
                  const QStringList &units)
{
    QJsonArray arr;
    for (const QString &u : units)
        arr.append(u);
    QString unitsJson = QString::fromUtf8(
        QJsonDocument(arr).toJson(QJsonDocument::Compact));
    unitsJson.replace(QChar(0x2028), QStringLiteral("\\u2028"))
        .replace(QChar(0x2029), QStringLiteral("\\u2029"));
    QString source = QStringLiteral(
        "(function(){var __u=%1,__o=%2;"
        "var __reg=window.__arktosCss=window.__arktosCss||{};"
        "try{")
        .arg(unitsJson, jsLiteral(origin));
    if (insert) {
        source += QStringLiteral(
            "var __host=document.head||document.documentElement;"
            "if(!__host)throw new Error('no-document');"
            "for(var i=0;i<__u.length;++i){"
            "var __k=__o+'\\n'+__u[i],__el=__reg[__k];"
            "if(!__el||!__el.isConnected){"
            "__el=document.createElement('style');"
            "__el.setAttribute('data-arktos-css','');"
            "__host.appendChild(__el);__reg[__k]=__el;}"
            "__el.textContent=__u[i];}");
    } else {
        source += QStringLiteral(
            "for(var i=0;i<__u.length;++i){"
            "var __k=__o+'\\n'+__u[i],__el=__reg[__k];"
            "if(__el){delete __reg[__k];__el.remove();}}");
    }
    source += QLatin1String(
        "return {__exec:1,ok:true};}catch(e){"
        "return {__exec:1,ok:false,e:String(e&&e.message||e)};}})()");
    return source;
}

} // namespace

QJsonObject ChromeBridge::invokeScripting(const QString &method,
                                          const QJsonObject &args)
{
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
    if (!m_backend)
        return err(QStringLiteral("backend-gone"));
    // ER09: the registration ops share the namespace but none of the
    // execId machinery — synchronous registry answers.
    if (method == QLatin1String("registerContentScripts")
            || method == QLatin1String("unregisterContentScripts")
            || method == QLatin1String("getRegisteredContentScripts")
            || method == QLatin1String("updateContentScripts"))
        return invokeScriptRegistration(method, args);
    const bool insertCss = method == QLatin1String("insertCSS");
    const bool cssOp =
        insertCss || method == QLatin1String("removeCSS");
    if (method != QLatin1String("executeScript") && !cssOp)
        return err(QStringLiteral("unimplemented"));

    const QString execId =
        args.value(QStringLiteral("execId")).toString();
    if (execId.isEmpty() || !ownsRelayKey(m_extensionId, execId))
        return err(QStringLiteral("invalid-argument"));

    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    // Chrome exposes chrome.scripting only to extensions that declare
    // the 'scripting' permission — this call-level gate stands in for
    // the absent namespace.
    if (!runtime->hasExtensionPermission(
            m_extensionId, QStringLiteral("scripting")))
        return err(QStringLiteral("no-permission"));

    const QJsonObject target =
        args.value(QStringLiteral("target")).toObject();
    if (!target.value(QStringLiteral("documentIds")).toArray()
             .isEmpty())
        // Document ids are a Chrome-internal handle we never mint —
        // typed, never a silent main-frame substitution.
        return err(QStringLiteral("unsupported"));
    if (!target.contains(QStringLiteral("tabId")))
        return err(QStringLiteral("invalid-argument"));
    WebView *view = ExtensionShimBackend::viewForTabId(
        int(target.value(QStringLiteral("tabId")).toDouble(-1)));
    if (!view || !view->page())
        return err(QStringLiteral("not-found"));
    QWebEnginePage *page = view->page();
    // The security boundary, BEFORE any source is generated into the
    // page — the same check the page agent's per-call gate runs
    // against the committed url.
    if (!runtime->agentCovers(m_extensionId, page->url()))
        return err(QStringLiteral("no-host-access"));

    QList<QWebEngineFrame> frames;
    QList<QUrl> coverUrls;
    const QWebEngineFrame mainFrame = page->mainFrame();
    if (mainFrame.isValid())
        collectFrames(mainFrame, page->url(), &frames, &coverUrls);
    if (frames.isEmpty())
        return err(QStringLiteral("no-frame"));

    const bool allFrames =
        target.value(QStringLiteral("allFrames")).toBool();
    const QJsonArray frameIds =
        target.value(QStringLiteral("frameIds")).toArray();
    if (allFrames && !frameIds.isEmpty())
        return err(QStringLiteral("invalid-argument"));
    QList<int> selected;
    if (allFrames) {
        for (int i = 0; i < frames.size(); ++i)
            selected.append(i);
    } else if (!frameIds.isEmpty()) {
        for (const QJsonValue &v : frameIds) {
            const int fid = int(v.toDouble(-1));
            if (fid < 0 || fid >= frames.size())
                return err(QStringLiteral("not-found"));
            if (!selected.contains(fid))
                selected.append(fid);
        }
    } else {
        selected.append(0);
    }

    // ER16 — per-frame coverage.  The page-level check above proves
    // the TOP document only; a covered page may frame an origin the
    // extension holds no grant for, and the fan-out below must never
    // runJavaScript into it.  coverUrls carries each frame's
    // effective url (about:/empty inherit the embedder, blob: names
    // its origin).  An explicitly-named uncovered frame rejects the
    // whole call like Chrome's frameIds contract; under allFrames
    // the uncovered frames get per-frame error entries instead.
    if (!frameIds.isEmpty()) {
        for (const int frameId : selected) {
            if (!runtime->agentCovers(
                    m_extensionId, coverUrls.at(frameId)))
                return err(QStringLiteral("no-host-access"));
        }
    }

    const QString funcSource =
        args.value(QStringLiteral("func")).toString();
    const QJsonArray execArgs =
        args.value(QStringLiteral("args")).toArray();
    const QJsonArray filesArg =
        args.value(QStringLiteral("files")).toArray();

    // The per-frame payload.  executeScript honours world (the
    // default ISOLATED is the agent world); insertCSS/removeCSS are
    // Chrome-internal injections with no world parameter and always
    // run in the agent world so the css ledger stays out of page
    // reach.
    quint32 worldId = ExtensionBackgroundRuntime::AgentWorldId;
    QStringList fileSources;
    QStringList cssUnits;
    QString cssOrigin;
    if (cssOp) {
        // Chrome takes exactly one of css/files; origin scopes the
        // removal key ('AUTHOR' default — 'USER' is the documented
        // cascade approximation recorded in cssSource).
        cssOrigin = args.value(QStringLiteral("origin"))
                        .toString(QStringLiteral("AUTHOR"));
        if (cssOrigin != QLatin1String("AUTHOR")
                && cssOrigin != QLatin1String("USER"))
            return err(QStringLiteral("invalid-argument"));
        const QJsonValue cssValue =
            args.value(QStringLiteral("css"));
        if (!cssValue.isUndefined() && !cssValue.isString())
            return err(QStringLiteral("invalid-argument"));
        if (cssValue.isString() && !filesArg.isEmpty())
            return err(QStringLiteral("invalid-argument"));
        if (cssValue.isString()) {
            cssUnits.append(cssValue.toString());
        } else {
            if (filesArg.isEmpty())
                return err(QStringLiteral("invalid-argument"));
            const QString error = loadBundleFiles(
                m_extensionPath, filesArg, &cssUnits);
            if (!error.isEmpty())
                return err(error);
        }
    } else {
        const QString world =
            args.value(QStringLiteral("world"))
                .toString(QStringLiteral("ISOLATED"));
        if (world != QLatin1String("ISOLATED")
                && world != QLatin1String("MAIN"))
            return err(QStringLiteral("invalid-argument"));
        if (world == QLatin1String("MAIN"))
            worldId = quint32(QWebEngineScript::MainWorld);
        if (!filesArg.isEmpty()) {
            if (!funcSource.isEmpty())
                return err(QStringLiteral("invalid-argument"));
            // Bundle files only — the same path hygiene the
            // background loader applies.
            const QString error = loadBundleFiles(
                m_extensionPath, filesArg, &fileSources);
            if (!error.isEmpty())
                return err(error);
        } else if (funcSource.isEmpty()) {
            return err(QStringLiteral("invalid-argument"));
        }
    }

    PendingExec &pending = m_pendingExecs[execId];
    pending.remaining = selected.size();
    pending.frameIds = selected;
    for (int i = 0; i < selected.size(); ++i)
        pending.results.append(QJsonValue(QJsonValue::Null));
    pending.expiry = new QTimer(this);
    pending.expiry->setSingleShot(true);
    pending.expiry->setInterval(20000);
    connect(pending.expiry, &QTimer::timeout, this, [this, execId]() {
        const auto it = m_pendingExecs.find(execId);
        if (it == m_pendingExecs.end())
            return;
        QJsonArray results = it->results;
        // Slots a dead or wedged frame never answered get the honest
        // per-frame error rather than a fabricated result.
        for (int i = 0; i < results.size(); ++i) {
            if (results.at(i).isNull())
                results.replace(i, QJsonObject{
                    {QStringLiteral("frameId"),
                     it->frameIds.at(i)},
                    {QStringLiteral("error"),
                     QStringLiteral("exec-timeout")}});
        }
        QTimer *timer = it->expiry;
        m_pendingExecs.erase(it);
        if (timer)
            timer->deleteLater();
        emit hostEvent(QStringLiteral("__arktos.execResult"),
                       QJsonObject{{QStringLiteral("execId"), execId},
                                   {QStringLiteral("ok"), true},
                                   {QStringLiteral("results"),
                                    results}});
    });
    pending.expiry->start();

    const QPointer<ChromeBridge> self(this);
    for (int slot = 0; slot < selected.size(); ++slot) {
        const int frameId = selected.at(slot);
        if (!runtime->agentCovers(
                m_extensionId, coverUrls.at(frameId))) {
            // allFrames swept in an uncovered subframe — the
            // InjectionResult entry carries the denial per frame
            // (explicit frameIds already rejected the call above);
            // nothing is generated into that document.
            finishExecSlot(execId, slot, QJsonObject{
                {QStringLiteral("frameId"), frameId},
                {QStringLiteral("error"),
                 QStringLiteral("no-host-access")}});
            continue;
        }
        QWebEngineFrame frame = frames.at(frameId);
        const QString key = execId + QLatin1Char('/')
            + QString::number(frameId);
        const QString source =
            cssOp ? cssSource(insertCss, cssOrigin, cssUnits)
                  : execSource(m_extensionId, funcSource, execArgs,
                               fileSources, key);
        frame.runJavaScript(
            source, worldId,
            [self, this, execId, slot, frameId, frame, worldId,
             key](const QVariant &value) {
                if (self)
                    finishExecFrame(execId, slot, frameId, value,
                                    frame, worldId, key);
            });
    }
    return ok();
}

void ChromeBridge::finishExecFrame(const QString &execId, int slot,
                                   int frameId, const QVariant &value,
                                   QWebEngineFrame frame,
                                   quint32 worldId, const QString &key)
{
    if (!m_pendingExecs.contains(execId))
        return;   // expired already
    const QJsonObject envelope = value.toJsonObject();
    if (envelope.value(QLatin1String("__exec")).toInt() != 1) {
        // The wrapper itself produced no answer — a func source that
        // fails to parse dies at compile time, before the inner
        // try/catch exists.  Chrome resolves with a bare entry and
        // logs to the console; the error field carries it honestly.
        finishExecSlot(execId, slot,
                       QJsonObject{
                           {QStringLiteral("frameId"), frameId},
                           {QStringLiteral("error"),
                            QStringLiteral("exec-failed")}});
        return;
    }
    if (envelope.value(QLatin1String("pending")).toBool()) {
        pollExecResult(execId, slot, frameId, frame, worldId, key, 60);
        return;
    }
    finishExecSlot(execId, slot, execResultEntry(frameId, envelope));
}

void ChromeBridge::finishExecSlot(const QString &execId, int slot,
                                  const QJsonObject &entry)
{
    const auto it = m_pendingExecs.find(execId);
    if (it == m_pendingExecs.end())
        return;
    it->results.replace(slot, entry);
    if (--it->remaining > 0)
        return;
    if (it->expiry) {
        it->expiry->stop();
        it->expiry->deleteLater();
    }
    const QJsonArray results = it->results;
    m_pendingExecs.erase(it);
    emit hostEvent(QStringLiteral("__arktos.execResult"),
                   QJsonObject{{QStringLiteral("execId"), execId},
                               {QStringLiteral("ok"), true},
                               {QStringLiteral("results"), results}});
}

void ChromeBridge::pollExecResult(const QString &execId, int slot,
                                  int frameId, QWebEngineFrame frame,
                                  quint32 worldId, const QString &key,
                                  int triesLeft)
{
    if (!m_pendingExecs.contains(execId))
        return;
    if (!frame.isValid() || triesLeft <= 0) {
        finishExecSlot(execId, slot,
                       QJsonObject{
                           {QStringLiteral("frameId"), frameId},
                           {QStringLiteral("error"),
                            frame.isValid()
                                ? QStringLiteral("exec-timeout")
                                : QStringLiteral("frame-gone")}});
        return;
    }
    const QPointer<ChromeBridge> self(this);
    frame.runJavaScript(
        QStringLiteral("(window.__arktosExecResults||{})[%1]||null")
            .arg(jsLiteral(key)),
        worldId,
        [self, this, execId, slot, frameId, frame, worldId, key,
         triesLeft](const QVariant &value) mutable {
            if (!self)
                return;
            const QJsonObject envelope = value.toJsonObject();
            if (!envelope.isEmpty()) {
                frame.runJavaScript(
                    QStringLiteral(
                        "delete (window.__arktosExecResults||{})[%1]")
                        .arg(jsLiteral(key)),
                    worldId);
                finishExecSlot(execId, slot,
                               execResultEntry(frameId, envelope));
                return;
            }
            QTimer::singleShot(150, this, [self, this, execId, slot,
                                           frameId, frame, worldId,
                                           key, triesLeft]() {
                if (self)
                    pollExecResult(execId, slot, frameId, frame,
                                   worldId, key, triesLeft - 1);
            });
        });
}

// -------------------------------------------------------------------
// ER09 — dynamic content-script registration.  Two front doors share
// one runtime registry:
//
//   chrome.scripting.registerContentScripts / unregisterContentScripts
//   / getRegisteredContentScripts / updateContentScripts  — the MV3
//   product API Bitwarden's updateMv3ContentScriptsRegistration /
//   ipc-content-script flows call (requires the 'scripting'
//   permission, like every chrome.scripting member).
//   chrome.contentScripts.register / unregister / getScripts — the
//   Firefox-shape surface the bundled webextension-polyfill resolves
//   browser.contentScripts.* to in a Chromium-shaped context (no API
//   permission exists for it; the per-match host-coverage check is
//   the gate, exactly like the MV2 model).
//
// A normalized spec arms as a profile-level QWebEngineScript in the
// shared AgentWorldId realm — same world, same wrapper, same
// match gate as a declared content_script — so it fires on the next
// matching navigation.  Chrome's rule applies BEFORE anything is
// armed: every matches[] pattern must be implied by the extension's
// host coverage (host_permissions or declared content-script
// matches — the same union agentCovers consults); a miss is a typed
// 'no-host-access', never a silent grant.  Specs carrying
// persistAcrossSessions (the default) are written to ProfileSettings
// 'extensions/regscripts/<id>' so they survive restarts exactly like
// Chrome's; session-only specs die when the extension unloads.
// -------------------------------------------------------------------

namespace {

struct RegMatchParts {
    QString scheme;
    QString host;
    QString path;
    bool allUrls = false;
    bool valid = false;
};

// Chrome match-pattern parse — '<scheme>://<host><path>' or
// '<all_urls>'.  file: patterns carry an empty host ('file:///*');
// every other scheme requires one.
RegMatchParts parseRegPattern(const QString &pattern)
{
    RegMatchParts p;
    if (pattern == QLatin1String("<all_urls>")) {
        p.allUrls = true;
        p.valid = true;
        return p;
    }
    const int sep = pattern.indexOf(QLatin1String("://"));
    if (sep <= 0)
        return p;
    p.scheme = pattern.left(sep);
    for (const QChar c : p.scheme) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('*')
                && c != QLatin1Char('+') && c != QLatin1Char('-')
                && c != QLatin1Char('.'))
            return p;
    }
    const QString rest = pattern.mid(sep + 3);
    const int slash = rest.indexOf(QLatin1Char('/'));
    p.host = slash < 0 ? rest : rest.left(slash);
    p.path = slash < 0 ? QStringLiteral("/*") : rest.mid(slash);
    if (p.host.isEmpty() && p.scheme != QLatin1String("file"))
        return p;
    p.valid = true;
    return p;
}

// Pattern-level host-coverage implication: is reg ⊆ hp?  The
// conservative version Chrome's "must have host permission for the
// match" check needs — hp '*://*/*' or '<all_urls>' covers the
// ordinary http/https registrations; a wildcard host covers its own
// domain + deeper subdomains; a path grant covers only itself (a
// broader registered path under a narrow grant is still access the
// extension never declared, so it fails closed).
bool hostPermCoversPattern(const QString &hp, const QString &reg)
{
    const RegMatchParts h = parseRegPattern(hp);
    const RegMatchParts r = parseRegPattern(reg);
    if (!h.valid || !r.valid)
        return false;
    if (r.allUrls)
        return h.allUrls;
    if (h.allUrls) {
        return r.scheme == QLatin1String("*")
            || r.scheme == QLatin1String("http")
            || r.scheme == QLatin1String("https")
            || r.scheme == QLatin1String("file")
            || r.scheme == QLatin1String("ftp");
    }
    if (h.scheme == QLatin1String("*")) {
        if (r.scheme != QLatin1String("http")
                && r.scheme != QLatin1String("https"))
            return false;
    } else if (h.scheme != r.scheme) {
        return false;
    }
    if (h.host == QLatin1String("*")) {
        // any host
    } else if (h.host.startsWith(QLatin1String("*."))) {
        const QString base = h.host.mid(2);
        const QString rhost = r.host.startsWith(QLatin1String("*."))
            ? r.host.mid(2) : r.host;
        if (rhost != base && !rhost.endsWith(QLatin1Char('.') + base))
            return false;
    } else if (h.host != r.host) {
        return false;
    }
    if (h.path == QLatin1String("/*") || h.path == QLatin1String("/"))
        return true;
    return h.path == r.path;
}

// One js/css spec entry: MV3 gives bare path strings; the Firefox
// shape gives {file:"x"} or {code:"..."} — normalize to {file}|
// {code} objects; file entries are re-proven against the bundle
// (path hygiene + existence — Chrome rejects a missing file at
// registration time too).  {code} is Firefox-only: MV3's
// RegisteredContentScript knows paths only, so inline code there is
// a typed invalid-argument, never a silent accept.  ({file} objects
// still pass on MV3 — the registry's own normalized spec shape
// round-trips through updateContentScripts.)
QJsonArray normalizeRegEntries(const QJsonArray &entries,
                               const QString &extensionPath,
                               bool allowCode, QString *error)
{
    QJsonArray out;
    const QDir dir(extensionPath);
    qint64 total = 0;
    for (const QJsonValue &v : entries) {
        QString file;
        QString code;
        if (v.isString()) {
            file = v.toString();
        } else if (v.isObject()) {
            const QJsonObject o = v.toObject();
            if (o.contains(QStringLiteral("code"))) {
                if (!allowCode) {
                    *error = QStringLiteral("invalid-argument");
                    return QJsonArray();
                }
                code = o.value(QStringLiteral("code")).toString();
            } else {
                file = o.value(QStringLiteral("file")).toString();
                if (file.isEmpty()) {
                    *error = QStringLiteral("invalid-argument");
                    return QJsonArray();
                }
            }
        } else {
            *error = QStringLiteral("invalid-argument");
            return QJsonArray();
        }
        if (!file.isEmpty() || (!v.isObject() && code.isEmpty())) {
            if (file.isEmpty()
                    || file.startsWith(QLatin1Char('/'))
                    || file.contains(QLatin1Char(':'))
                    || file.split(QLatin1Char('/'))
                           .contains(QStringLiteral(".."))) {
                *error = QStringLiteral("bad-path");
                return QJsonArray();
            }
            QFile f(dir.absoluteFilePath(file));
            total += f.size();
            if (f.size() > 4 * 1024 * 1024 || total > 8 * 1024 * 1024
                    || !f.exists()) {
                *error = QStringLiteral("no-such-file");
                return QJsonArray();
            }
            out.append(QJsonObject{{QStringLiteral("file"), file}});
        } else {
            out.append(QJsonObject{{QStringLiteral("code"), code}});
        }
    }
    return out;
}

// Normalize one caller spec (MV3 RegisteredContentScript or the
// Firefox contentScriptOptions) into the registry's canonical
// record.  coveragePatterns is hostPerms + declared cs matches —
// the agentCovers union.  Answers {} with *error set on failure;
// *notes collects the recorded approximations the reply carries.
// fallbackId is the caller-minted unique id the Firefox shape gets
// (it carries no caller id).
QJsonObject normalizeRegSpec(const QString &api,
                             const QJsonObject &raw,
                             const QStringList &coveragePatterns,
                             const QString &extensionPath,
                             const QString &fallbackId,
                             QString *error, QStringList *notes)
{
    const bool mv3 = api == QLatin1String("scripting");
    QString id = raw.value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        if (mv3) {   // Chrome requires a caller id.
            *error = QStringLiteral("invalid-argument");
            return QJsonObject();
        }
        id = fallbackId;
    }

    // Firefox extras we cannot serve: glob lists (a different
    // matcher, not translateable into match patterns) and container
    // scoping.
    if (!mv3) {
        if (!raw.value(QStringLiteral("includeGlobs")).toArray()
                 .isEmpty()
                || !raw.value(QStringLiteral("excludeGlobs"))
                        .toArray().isEmpty()) {
            *error = QStringLiteral("unsupported");
            return QJsonObject();
        }
        if (!raw.value(QStringLiteral("cookieStoreId"))
                 .isUndefined()) {
            *error = QStringLiteral("unsupported");
            return QJsonObject();
        }
    }

    QJsonArray matches;
    for (const QJsonValue &v :
             raw.value(QStringLiteral("matches")).toArray()) {
        const QString pattern = v.toString();
        if (!parseRegPattern(pattern).valid) {
            *error = QStringLiteral("invalid-argument");
            return QJsonObject();
        }
        matches.append(pattern);
    }
    if (matches.isEmpty()) {
        *error = QStringLiteral("invalid-argument");
        return QJsonObject();
    }
    // The security gate — BEFORE the spec is armed: every match must
    // be implied by the extension's declared host coverage.  A miss
    // is a typed refusal, never a silent grant.
    for (const QJsonValue &v : matches) {
        const QString pattern = v.toString();
        bool covered = false;
        for (const QString &hp : coveragePatterns) {
            if (hostPermCoversPattern(hp, pattern)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            *error = QStringLiteral("no-host-access");
            return QJsonObject();
        }
    }

    QJsonArray excludes;
    for (const QJsonValue &v :
             raw.value(QStringLiteral("excludeMatches")).toArray()) {
        const QString pattern = v.toString();
        if (!parseRegPattern(pattern).valid) {
            *error = QStringLiteral("invalid-argument");
            return QJsonObject();
        }
        excludes.append(pattern);
    }

    QString entryError;
    const QJsonArray js = normalizeRegEntries(
        raw.value(QStringLiteral("js")).toArray(), extensionPath,
        !mv3, &entryError);
    if (!entryError.isEmpty()) {
        *error = entryError;
        return QJsonObject();
    }
    const QJsonArray css = normalizeRegEntries(
        raw.value(QStringLiteral("css")).toArray(), extensionPath,
        !mv3, &entryError);
    if (!entryError.isEmpty()) {
        *error = entryError;
        return QJsonObject();
    }

    const QString runAt = raw.value(QStringLiteral("runAt"))
        .toString(QStringLiteral("document_idle"));
    if (runAt != QLatin1String("document_start")
            && runAt != QLatin1String("document_end")
            && runAt != QLatin1String("document_idle")) {
        *error = QStringLiteral("invalid-argument");
        return QJsonObject();
    }
    const QString world = raw.value(QStringLiteral("world"))
        .toString(QStringLiteral("ISOLATED"));
    if (world != QLatin1String("ISOLATED")
            && world != QLatin1String("MAIN")) {
        *error = QStringLiteral("invalid-argument");
        return QJsonObject();
    }
    if (world == QLatin1String("MAIN"))
        notes->append(QStringLiteral(
            "chrome.%1 register: world:'MAIN' approximated by the "
            "shared isolated realm — more isolation, same chrome "
            "binding").arg(api));
    if (raw.value(QStringLiteral("matchAboutBlank")).toBool()
            || raw.value(QStringLiteral("matchOriginAsFallback"))
                   .toBool())
        notes->append(QStringLiteral(
            "chrome.%1 register: matchAboutBlank/"
            "matchOriginAsFallback accepted but originless frames "
            "are never covered — the flag is inert here").arg(api));

    QJsonObject spec;
    spec.insert(QStringLiteral("id"), id);
    spec.insert(QStringLiteral("api"), api);
    spec.insert(QStringLiteral("matches"), matches);
    spec.insert(QStringLiteral("excludeMatches"), excludes);
    spec.insert(QStringLiteral("js"), js);
    spec.insert(QStringLiteral("css"), css);
    spec.insert(QStringLiteral("allFrames"),
                raw.value(QStringLiteral("allFrames")).toBool());
    spec.insert(QStringLiteral("matchAboutBlank"),
                raw.value(QStringLiteral("matchAboutBlank"))
                    .toBool());
    spec.insert(QStringLiteral("runAt"), runAt);
    spec.insert(QStringLiteral("world"), world);
    spec.insert(QStringLiteral("persistAcrossSessions"),
                raw.value(QStringLiteral("persistAcrossSessions"))
                    .toBool(true));
    return spec;
}

// The get* echo.  MV3 answers Chrome's RegisteredContentScript
// shape (js/css back to plain path strings — {code} can only exist
// on the MV2 api); MV2 answers the spec fields plus the id the
// handle needs.
QJsonObject regEcho(const QString &api, const QJsonObject &spec)
{
    if (api == QLatin1String("contentScripts")) {
        QJsonObject echo = spec;
        echo.remove(QStringLiteral("api"));
        echo.remove(QStringLiteral("world"));
        echo.remove(QStringLiteral("persistAcrossSessions"));
        return echo;
    }
    QJsonObject echo;
    echo.insert(QStringLiteral("id"),
                spec.value(QStringLiteral("id")));
    echo.insert(QStringLiteral("matches"),
                spec.value(QStringLiteral("matches")));
    echo.insert(QStringLiteral("excludeMatches"),
                spec.value(QStringLiteral("excludeMatches")));
    QJsonArray jsFiles;
    for (const QJsonValue &v :
             spec.value(QStringLiteral("js")).toArray())
        jsFiles.append(v.toObject()
                           .value(QStringLiteral("file")));
    echo.insert(QStringLiteral("js"), jsFiles);
    QJsonArray cssFiles;
    for (const QJsonValue &v :
             spec.value(QStringLiteral("css")).toArray())
        cssFiles.append(v.toObject()
                            .value(QStringLiteral("file")));
    echo.insert(QStringLiteral("css"), cssFiles);
    echo.insert(QStringLiteral("allFrames"),
                spec.value(QStringLiteral("allFrames")));
    echo.insert(QStringLiteral("matchAboutBlank"),
                spec.value(QStringLiteral("matchAboutBlank")));
    echo.insert(QStringLiteral("runAt"),
                spec.value(QStringLiteral("runAt")));
    echo.insert(QStringLiteral("world"),
                spec.value(QStringLiteral("world")));
    echo.insert(QStringLiteral("persistAcrossSessions"),
                spec.value(QStringLiteral("persistAcrossSessions")));
    return echo;
}

// A registered spec wrapped for the shared agent world — the same
// closure/gate shape declared content scripts get, keyed by the
// registration id.  {file} entries get the background loader's
// read+lazy-chunk merge; {code} entries land inline (Firefox shape
// only — MV3 specs never carry them).
QString registeredScriptSource(const QString &extensionId,
                               const QString &registrationId,
                               const QString &extensionPath,
                               const QJsonObject &spec)
{
    QStringList matches;
    for (const QJsonValue &v :
             spec.value(QStringLiteral("matches")).toArray())
        matches.append(v.toString());
    QStringList excludes;
    for (const QJsonValue &v :
             spec.value(QStringLiteral("excludeMatches")).toArray())
        excludes.append(v.toString());

    QString jsBody;
    const QDir dir(extensionPath);
    for (const QJsonValue &v :
             spec.value(QStringLiteral("js")).toArray()) {
        const QJsonObject entry = v.toObject();
        if (entry.contains(QStringLiteral("code"))) {
            jsBody += entry.value(QStringLiteral("code")).toString();
            jsBody += QLatin1Char('\n');
        } else {
            jsBody += extensionCodeSource(
                extensionPath,
                QStringList{
                    entry.value(QStringLiteral("file")).toString()});
        }
    }
    QStringList cssUnits;
    for (const QJsonValue &v :
             spec.value(QStringLiteral("css")).toArray()) {
        const QJsonObject entry = v.toObject();
        if (entry.contains(QStringLiteral("code"))) {
            cssUnits.append(
                entry.value(QStringLiteral("code")).toString());
        } else {
            QFile file(dir.absoluteFilePath(
                entry.value(QStringLiteral("file")).toString()));
            if (file.open(QIODevice::ReadOnly))
                cssUnits.append(QString::fromUtf8(file.readAll()));
        }
    }
    return wrapContentScript(extensionId,
                             QStringLiteral("dcs_") + registrationId,
                             jsonArrayOfStrings(matches),
                             jsonArrayOfStrings(excludes),
                             jsBody, cssUnits);
}

QJsonObject regOk(const QJsonValue &result = QJsonValue())
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    if (!result.isUndefined())
        o.insert(QStringLiteral("result"), result);
    return o;
}

QJsonObject regErr(const QString &code)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    return o;
}

} // namespace

QString ExtensionBackgroundRuntime::regProfileId(
        const QString &extensionId) const
{
    if (ExtensionBackgroundHost *host = m_hosts.value(extensionId)) {
        if (host->page() && host->page()->profile())
            return BrowserProfile::profileIdFor(
                host->page()->profile());
    }
    return ProfileSettings::defaultId();
}

void ExtensionBackgroundRuntime::persistRegisteredScripts(
        const QString &extensionId)
{
    ProfileSettings settings(regProfileId(extensionId));
    const QString key = QStringLiteral("extensions/regscripts/")
        + extensionId;
    QJsonArray persisted;
    const QHash<QString, QJsonObject> specs =
        m_regScripts.value(extensionId);
    for (const QJsonObject &spec : specs) {
        if (spec.value(QStringLiteral("persistAcrossSessions"))
                .toBool(true))
            persisted.append(spec);
    }
    if (persisted.isEmpty())
        settings.remove(key);
    else
        settings.setValue(key, QString::fromUtf8(
            QJsonDocument(persisted)
                .toJson(QJsonDocument::Compact)));
}

void ExtensionBackgroundRuntime::armRegisteredScript(
        const QString &extensionId, const QJsonObject &spec)
{
    ExtensionBackgroundHost *host = m_hosts.value(extensionId);
    if (!host || !host->page() || !host->page()->profile())
        return;   // arms on the next restoreRegisteredScripts()
    QWebEngineProfile *profile = host->page()->profile();
    const QString id = spec.value(QStringLiteral("id")).toString();
    QWebEngineScript script;
    script.setName(
        ExtensionBackgroundHost::dynamicContentScriptInjectionName(
            extensionId, id));
    // document_start -> DocumentCreation; end/idle -> DocumentReady,
    // the same run_at approximation the declared set carries.
    script.setInjectionPoint(
        spec.value(QStringLiteral("runAt")).toString()
                == QLatin1String("document_start")
            ? QWebEngineScript::DocumentCreation
            : QWebEngineScript::DocumentReady);
    script.setWorldId(ExtensionBackgroundRuntime::AgentWorldId);
    script.setRunsOnSubFrames(
        spec.value(QStringLiteral("allFrames")).toBool());
    script.setSourceCode(registeredScriptSource(
        extensionId, id, host->extensionPath(), spec));
    profile->scripts()->insert(script);
}

void ExtensionBackgroundRuntime::disarmRegisteredScripts(
        const QString &extensionId, const QString &registrationId)
{
    const QString name =
        ExtensionBackgroundHost::dynamicContentScriptInjectionName(
            extensionId, registrationId);
    for (const QPointer<QWebEngineProfile> &profile : m_profiles) {
        if (!profile)
            continue;
        const QList<QWebEngineScript> installed =
            profile->scripts()->toList();
        for (const QWebEngineScript &script : installed) {
            if (script.name() == name)
                profile->scripts()->remove(script);
        }
    }
}

void ExtensionBackgroundRuntime::restoreRegisteredScripts(
        const QString &extensionId)
{
    if (!m_hosts.value(extensionId))
        return;
    // Persisted specs merge back over the in-memory registry —
    // session entries from before a disable/enable cycle keep their
    // slot; a persisted record is the source of truth for its id.
    ProfileSettings settings(regProfileId(extensionId));
    const QJsonArray persisted = QJsonDocument::fromJson(
        settings.value(QStringLiteral("extensions/regscripts/")
                       + extensionId)
            .toString().toUtf8()).array();
    for (const QJsonValue &v : persisted) {
        const QJsonObject spec = v.toObject();
        const QString id = spec.value(QStringLiteral("id"))
            .toString();
        if (!id.isEmpty())
            m_regScripts[extensionId][id] = spec;
    }
    const QHash<QString, QJsonObject> specs =
        m_regScripts.value(extensionId);
    for (const QJsonObject &spec : specs)
        armRegisteredScript(extensionId, spec);
}

void ExtensionBackgroundRuntime::dropRegisteredScripts(
        const QString &extensionId)
{
    const QHash<QString, QJsonObject> specs =
        m_regScripts.take(extensionId);
    for (const QString &id : specs.keys())
        disarmRegisteredScripts(extensionId, id);
}

QJsonObject ExtensionBackgroundRuntime::registeredScriptSpecs(
        const QString &extensionId) const
{
    QJsonObject out;
    const QHash<QString, QJsonObject> specs =
        m_regScripts.value(extensionId);
    for (auto it = specs.constBegin(); it != specs.constEnd(); ++it)
        out.insert(it.key(), it.value());
    return out;
}

QJsonObject ExtensionBackgroundRuntime::registerContentScripts(
        const QString &extensionId, const QString &extensionPath,
        const QJsonArray &scripts, const QString &api)
{
    if (scripts.isEmpty())
        return regOk();

    // The coverage union agentCovers consults — declared
    // content-script matches count as host access for registration
    // exactly like Chrome treats them.
    QStringList coverage = m_extHostPermissions.value(extensionId);
    if (ExtensionBackgroundHost *host = m_hosts.value(extensionId))
        coverage += host->contentScriptMatches();

    // Validate the WHOLE batch before touching the registry — a
    // failed register registers nothing, matching Chrome's atomic
    // contract.
    const QHash<QString, QJsonObject> existing =
        m_regScripts.value(extensionId);
    QSet<QString> ids;
    QSet<QString> names;
    for (const QString &have : existing.keys()) {
        ids.insert(have);
        names.insert(
            ExtensionBackgroundHost
                ::dynamicContentScriptInjectionName(extensionId,
                                                    have));
    }
    QList<QJsonObject> normalized;
    QStringList notes;
    int mintSeq = 0;
    auto mintId = [&ids, &mintSeq]() {
        QString candidate;
        do {
            candidate = QStringLiteral("mv2cs-%1").arg(mintSeq++);
        } while (ids.contains(candidate));
        return candidate;
    };
    for (const QJsonValue &v : scripts) {
        const QJsonObject raw = v.toObject();
        QString error;
        QJsonObject spec = normalizeRegSpec(
            api, raw, coverage, extensionPath, mintId(),
            &error, &notes);
        if (!error.isEmpty())
            return regErr(error);
        const QString id = spec.value(QStringLiteral("id"))
            .toString();
        const QString name =
            ExtensionBackgroundHost
                ::dynamicContentScriptInjectionName(extensionId, id);
        if (ids.contains(id) || names.contains(name))
            return regErr(QStringLiteral("invalid-argument"));
        ids.insert(id);
        names.insert(name);
        normalized.append(spec);
    }

    for (const QJsonObject &spec : normalized) {
        m_regScripts[extensionId].insert(
            spec.value(QStringLiteral("id")).toString(), spec);
        armRegisteredScript(extensionId, spec);
    }
    persistRegisteredScripts(extensionId);

    QJsonObject result;
    if (api != QLatin1String("scripting"))
        result.insert(QStringLiteral("id"),
                      normalized.first()
                          .value(QStringLiteral("id")));
    if (!notes.isEmpty()) {
        QJsonArray arr;
        for (const QString &n : notes)
            arr.append(n);
        result.insert(QStringLiteral("notes"), arr);
    }
    return regOk(result);
}

QJsonObject ExtensionBackgroundRuntime::unregisterContentScripts(
        const QString &extensionId, const QStringList &ids)
{
    auto it = m_regScripts.find(extensionId);
    if (it == m_regScripts.end() || it.value().isEmpty()) {
        // Nothing registered: an unfiltered unregister is a no-op,
        // a named one is Chrome's nonexistent-id error.
        return ids.isEmpty() ? regOk()
                             : regErr(QStringLiteral("not-found"));
    }
    if (ids.isEmpty()) {   // no filter -> unregister everything
        const QStringList all = it.value().keys();
        for (const QString &id : all)
            disarmRegisteredScripts(extensionId, id);
        it->clear();
        persistRegisteredScripts(extensionId);
        return regOk();
    }
    // Atomic like register — a missing id fails the whole call.
    for (const QString &id : ids) {
        if (!it->contains(id))
            return regErr(QStringLiteral("not-found"));
    }
    for (const QString &id : ids) {
        disarmRegisteredScripts(extensionId, id);
        it->remove(id);
    }
    persistRegisteredScripts(extensionId);
    return regOk();
}

QJsonArray ExtensionBackgroundRuntime::registeredContentScripts(
        const QString &extensionId, const QString &api,
        const QStringList &onlyIds) const
{
    QJsonArray out;
    const QHash<QString, QJsonObject> specs =
        m_regScripts.value(extensionId);
    for (const QJsonObject &spec : specs) {
        if (spec.value(QStringLiteral("api")).toString() != api)
            continue;
        const QString id = spec.value(QStringLiteral("id"))
            .toString();
        if (!onlyIds.isEmpty() && !onlyIds.contains(id))
            continue;
        out.append(regEcho(api, spec));
    }
    return out;
}

QJsonObject ExtensionBackgroundRuntime::updateContentScripts(
        const QString &extensionId, const QString &extensionPath,
        const QJsonArray &scripts)
{
    if (scripts.isEmpty())
        return regOk();
    auto it = m_regScripts.find(extensionId);
    // Validate the whole batch: every entry must name an existing
    // MV3 registration.
    QList<QJsonObject> merged;
    for (const QJsonValue &v : scripts) {
        const QJsonObject patch = v.toObject();
        const QString id = patch.value(QStringLiteral("id"))
            .toString();
        if (id.isEmpty())
            return regErr(QStringLiteral("invalid-argument"));
        if (it == m_regScripts.end() || !it->contains(id)
                || it->value(id).value(QStringLiteral("api"))
                       .toString() != QLatin1String("scripting"))
            return regErr(QStringLiteral("not-found"));
        QJsonObject spec = it->value(id);
        // Chrome updates only the provided fields.
        static const QStringList updatable = {
            QStringLiteral("allFrames"), QStringLiteral("css"),
            QStringLiteral("excludeMatches"), QStringLiteral("js"),
            QStringLiteral("matchAboutBlank"),
            QStringLiteral("matchOriginAsFallback"),
            QStringLiteral("matches"),
            QStringLiteral("persistAcrossSessions"),
            QStringLiteral("runAt"), QStringLiteral("world")};
        for (const QString &key : updatable) {
            if (patch.contains(key))
                spec.insert(key, patch.value(key));
        }
        merged.append(spec);
    }

    QStringList coverage = m_extHostPermissions.value(extensionId);
    if (ExtensionBackgroundHost *host = m_hosts.value(extensionId))
        coverage += host->contentScriptMatches();
    QList<QJsonObject> normalized;
    QStringList notes;
    for (const QJsonObject &spec : merged) {
        QString error;
        const QJsonObject revalidated = normalizeRegSpec(
            QStringLiteral("scripting"), spec, coverage,
            extensionPath, QString(), &error, &notes);
        if (!error.isEmpty())
            return regErr(error);
        normalized.append(revalidated);
    }
    for (const QJsonObject &spec : normalized) {
        const QString id = spec.value(QStringLiteral("id"))
            .toString();
        it.value()[id] = spec;
        disarmRegisteredScripts(extensionId, id);
        armRegisteredScript(extensionId, spec);
    }
    persistRegisteredScripts(extensionId);
    QJsonObject result;
    if (!notes.isEmpty()) {
        QJsonArray arr;
        for (const QString &n : notes)
            arr.append(n);
        result.insert(QStringLiteral("notes"), arr);
    }
    return regOk(result);
}

QJsonObject ChromeBridge::invokeScriptRegistration(
        const QString &method, const QJsonObject &args)
{
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
    if (!m_backend)
        return err(QStringLiteral("backend-gone"));
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    // The same call-level gate executeScript carries —
    // chrome.scripting requires the declared 'scripting' permission.
    if (!runtime->hasExtensionPermission(
            m_extensionId, QStringLiteral("scripting")))
        return err(QStringLiteral("no-permission"));

    if (method == QLatin1String("registerContentScripts")) {
        const QJsonValue value = args.value(QStringLiteral("scripts"));
        if (!value.isArray())
            return err(QStringLiteral("invalid-argument"));
        return runtime->registerContentScripts(
            m_extensionId, m_extensionPath, value.toArray(),
            QStringLiteral("scripting"));
    }
    if (method == QLatin1String("unregisterContentScripts")) {
        QStringList ids;
        for (const QJsonValue &v :
                 args.value(QStringLiteral("filter")).toObject()
                     .value(QStringLiteral("ids")).toArray())
            ids.append(v.toString());
        return runtime->unregisterContentScripts(m_extensionId, ids);
    }
    if (method == QLatin1String("getRegisteredContentScripts")) {
        QStringList ids;
        for (const QJsonValue &v :
                 args.value(QStringLiteral("filter")).toObject()
                     .value(QStringLiteral("ids")).toArray())
            ids.append(v.toString());
        return ok(runtime->registeredContentScripts(
            m_extensionId, QStringLiteral("scripting"), ids));
    }
    if (method == QLatin1String("updateContentScripts")) {
        const QJsonValue value = args.value(QStringLiteral("scripts"));
        if (!value.isArray())
            return err(QStringLiteral("invalid-argument"));
        return runtime->updateContentScripts(
            m_extensionId, m_extensionPath, value.toArray());
    }
    return err(QStringLiteral("unimplemented"));
}

QJsonObject ChromeBridge::invokeContentScripts(
        const QString &method, const QJsonObject &args)
{
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
    if (!m_backend)
        return err(QStringLiteral("backend-gone"));
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

    if (method == QLatin1String("register")) {
        const QJsonObject options =
            args.value(QStringLiteral("options")).toObject();
        if (options.isEmpty())
            return err(QStringLiteral("invalid-argument"));
        // No 'scripting' permission exists for the Firefox-shape
        // surface — the per-match host-coverage check inside
        // registration is the whole gate (the MV2 model).
        return runtime->registerContentScripts(
            m_extensionId, m_extensionPath, QJsonArray{options},
            QStringLiteral("contentScripts"));
    }
    if (method == QLatin1String("unregister")) {
        QString id = args.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            id = args.value(QStringLiteral("handle")).toObject()
                .value(QStringLiteral("id")).toString();
        }
        if (id.isEmpty())
            return err(QStringLiteral("invalid-argument"));
        return runtime->unregisterContentScripts(
            m_extensionId, QStringList{id});
    }
    if (method == QLatin1String("getScripts")) {
        return ok(runtime->registeredContentScripts(
            m_extensionId, QStringLiteral("contentScripts"),
            QStringList()));
    }
    return err(QStringLiteral("unimplemented"));
}

// ER11 — observe-only chrome.webRequest.  The shim's event objects
// register one record per addListener ('subscribe') and drop it on
// removeListener ('unsubscribe'); this bridge is the lifetime
// context, so a closing popup/background takes its listeners down
// like Chrome's.  The 'webRequest' manifest permission is the
// registration gate — matching fanOut's delivery gate.
QJsonObject ChromeBridge::invokeWebRequest(const QString &method,
                                           const QJsonObject &args)
{
    auto err = [](const QString &code) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), false);
        o.insert(QStringLiteral("error"), code);
        return o;
    };
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    if (!runtime->hasExtensionPermission(
            m_extensionId, QStringLiteral("webRequest")))
        return err(QStringLiteral("no-webRequest-permission"));
    ExtensionRequestWatcher *watcher =
        ExtensionRequestWatcher::instance();
    const QString regId = args.value(QLatin1String("regId")).toString();
    if (!ownsRelayKey(m_extensionId, regId))
        return err(QStringLiteral("invalid-argument"));
    if (method == QLatin1String("subscribe")) {
        return watcher->subscribe(
            this, m_extensionId, regId,
            args.value(QLatin1String("event")).toString(),
            args.value(QLatin1String("filter")).toObject(),
            args.value(QLatin1String("extraInfoSpec")).toArray());
    }
    if (method == QLatin1String("unsubscribe"))
        return watcher->unsubscribe(m_extensionId, regId);
    return err(QStringLiteral("unimplemented"));
}

QJsonObject ExtensionBackgroundRuntime::pageSend(
        ExtensionPageBridge *page, const QString &extensionId,
        const QString &mid, const QJsonValue &msg,
        const QJsonObject &sender)
{
    if (!page || mid.isEmpty() || msg.isUndefined()
            || !ownsRelayKey(extensionId, mid))
        return pageErr(QStringLiteral("invalid-argument"));

    // Wrap the raw user message in the relay envelope the shim's
    // envelopeOf() unwraps — {__arktosMsg:{mid,msg,extId,senderUrl}}
    // — stamping the C++-built sender on at the same time: a forged
    // senderUrl from the page side must not masquerade a message as
    // extension-internal (Bitwarden's senderIsInternal gate keys on
    // origin).
    const QJsonObject envelope{
        {QStringLiteral("__arktosMsg"),
         QJsonObject{{QStringLiteral("mid"), mid},
                     {QStringLiteral("msg"), msg},
                     {QStringLiteral("extId"), extensionId},
                     {QStringLiteral("sender"), sender},
                     {QStringLiteral("senderUrl"),
                      sender.value(QStringLiteral("url"))}}}};

    int peers = 0;
    const QJsonObject payload{{QStringLiteral("mid"), mid},
                              {QStringLiteral("msg"), envelope}};
    for (const RelayContext &ctx : m_relayContexts) {
        if (!ctx.bridge
                || ctx.bridge->extensionId() != extensionId)
            continue;   // never cross extension boundaries
        emit ctx.bridge->hostEvent(QStringLiteral("__arktos.relay"),
                                   payload);
        ++peers;
        nudgeBridge(ctx.bridge);
    }
    if (peers > 0) {
        PendingRelay pending;
        pending.origin = page;
        pending.extensionId = extensionId;
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
    return pageOk(QJsonObject{{QStringLiteral("peers"), peers}});
}

QJsonObject ExtensionBackgroundRuntime::pageRespond(
        ExtensionPageBridge *page, const QString &mid,
        const QJsonValue &response, bool hadListeners)
{
    auto it = m_pageDeliveries.find(mid);
    if (it == m_pageDeliveries.end()
            || it.value().target != page)
        return pageErr(QStringLiteral("unknown-mid"));
    const PendingPageDelivery delivery = it.value();
    m_pageDeliveries.erase(it);
    if (QTimer *t = m_pageDeliveryTimers.take(mid))
        t->deleteLater();
    if (delivery.waiter) {
        emitToEnd(delivery.waiter,
                  QStringLiteral("__arktos.deliverResponse"),
                  QJsonObject{{QStringLiteral("mid"), mid},
                              {QStringLiteral("response"), response},
                              {QStringLiteral("hadListeners"),
                               hadListeners}});
    }
    return pageOk();
}

QJsonObject ExtensionBackgroundRuntime::pagePortConnect(
        ExtensionPageBridge *page, const QString &extensionId,
        const QString &chan, const QString &portId,
        const QString &name, const QJsonObject &sender)
{
    if (!page || chan.isEmpty() || portId.isEmpty()
            || !ownsRelayKey(extensionId, chan))
        return pageErr(QStringLiteral("invalid-argument"));
    m_portEnds[chan].append(PortEnd{page, portId});
    int peers = 0;
    const QJsonObject payload{
        {QStringLiteral("chan"), chan},
        {QStringLiteral("name"), name},
        {QStringLiteral("sender"), sender}};
    for (const RelayContext &ctx : m_relayContexts) {
        if (!ctx.bridge
                || ctx.bridge->extensionId() != extensionId)
            continue;
        emit ctx.bridge->hostEvent(
            QStringLiteral("__arktos.portConnect"), payload);
        ++peers;
        nudgeBridge(ctx.bridge);
    }
    return pageOk(QJsonObject{{QStringLiteral("peers"), peers}});
}

QJsonObject ExtensionBackgroundRuntime::pagePortAccept(
        ExtensionPageBridge *page, const QString &chan,
        const QString &portId)
{
    if (!page || chan.isEmpty() || portId.isEmpty())
        return pageErr(QStringLiteral("invalid-argument"));
    for (const PortEnd &end : m_portEnds.value(chan)) {
        if (end.target == page && end.localPortId == portId)
            return pageOk();   // already registered
    }
    m_portEnds[chan].append(PortEnd{page, portId});
    return pageOk();
}

QJsonObject ExtensionBackgroundRuntime::pagePortMessage(
        ExtensionPageBridge *page, const QString &chan,
        const QString &portId, const QJsonValue &msg)
{
    if (!page || chan.isEmpty() || portId.isEmpty())
        return pageErr(QStringLiteral("invalid-argument"));
    int delivered = 0;
    const QList<PortEnd> ends = m_portEnds.value(chan);
    for (const PortEnd &end : ends) {
        if (!end.target)
            continue;
        if (end.target == page && end.localPortId == portId)
            continue;   // the sending end itself
        emitToEnd(end.target,
                  QStringLiteral("__arktos.portMessage"),
                  QJsonObject{{QStringLiteral("portId"),
                               end.localPortId},
                              {QStringLiteral("msg"), msg}});
        ++delivered;
        if (auto *cb = qobject_cast<ChromeBridge *>(end.target))
            nudgeBridge(cb);
    }
    return pageOk(QJsonObject{{QStringLiteral("delivered"),
                               delivered}});
}

QJsonObject ExtensionBackgroundRuntime::pagePortDisconnect(
        ExtensionPageBridge *page, const QString &chan,
        const QString &portId)
{
    if (!page || chan.isEmpty() || portId.isEmpty())
        return pageErr(QStringLiteral("invalid-argument"));
    auto it = m_portEnds.find(chan);
    if (it == m_portEnds.end())
        return pageOk();
    QList<PortEnd> &ends = it.value();
    for (int i = ends.size() - 1; i >= 0; --i) {
        if (ends.at(i).target == page
                && ends.at(i).localPortId == portId)
            ends.removeAt(i);
    }
    if (ends.isEmpty()) {
        m_portEnds.erase(it);
        return pageOk();
    }
    for (const PortEnd &end : std::as_const(ends)) {
        if (end.target) {
            emitToEnd(end.target,
                      QStringLiteral("__arktos.portDisconnect"),
                      QJsonObject{{QStringLiteral("portId"),
                                   end.localPortId}});
        }
    }
    return pageOk();
}

void ExtensionBackgroundRuntime::unregisterPageContext(
        ExtensionPageBridge *page)
{
    if (!page)
        return;
    // Pending sends this page originated die with it — the hosted
    // replies that arrive later find no slot (the page side is gone
    // anyway).
    const QStringList mids = m_relayPending.keys();
    for (const QString &mid : mids) {
        if (m_relayPending.value(mid).origin == page) {
            if (QTimer *t = m_relayPendingTimers.take(mid))
                t->deleteLater();
            m_relayPending.remove(mid);
        }
    }
    const QStringList dmids = m_pageDeliveries.keys();
    for (const QString &mid : dmids) {
        if (m_pageDeliveries.value(mid).target == page
                || m_pageDeliveries.value(mid).waiter == page) {
            m_pageDeliveries.remove(mid);
            if (QTimer *t = m_pageDeliveryTimers.take(mid))
                t->deleteLater();
        }
    }
    // Port ends die with the page; the surviving ends get the honest
    // onDisconnect (Bitwarden's setupExtensionDisconnectAction
    // depends on it).
    const QStringList chans = m_portEnds.keys();
    for (const QString &chan : chans) {
        QList<PortEnd> &ends = m_portEnds[chan];
        bool removed = false;
        for (int i = ends.size() - 1; i >= 0; --i) {
            if (!ends.at(i).target || ends.at(i).target == page) {
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
            if (end.target) {
                emitToEnd(end.target,
                          QStringLiteral("__arktos.portDisconnect"),
                          QJsonObject{{QStringLiteral("portId"),
                                       end.localPortId}});
            }
        }
    }
}

void ExtensionBackgroundRuntime::pageDeliver(
        ExtensionPageBridge *page, const QString &extensionId,
        const QString &mid, const QJsonValue &msg,
        const QJsonObject &sender, QObject *waiter)
{
    if (!page || mid.isEmpty())
        return;
    PendingPageDelivery delivery;
    delivery.waiter = waiter;
    delivery.target = page;
    delivery.extensionId = extensionId;
    m_pageDeliveries.insert(mid, delivery);
    // Expiry like the relay's: a page with no live agent (or one
    // whose listeners never answer) resolves the waiter with the
    // honest no-response shape instead of pinning the slot.
    QTimer *expiry = new QTimer(this);
    expiry->setSingleShot(true);
    expiry->setInterval(30000);
    connect(expiry, &QTimer::timeout, this, [this, mid]() {
        const auto it = m_pageDeliveries.find(mid);
        if (it == m_pageDeliveries.end())
            return;
        const PendingPageDelivery pending = it.value();
        m_pageDeliveries.erase(it);
        if (QTimer *t = m_pageDeliveryTimers.take(mid))
            t->deleteLater();
        if (pending.waiter) {
            emitToEnd(pending.waiter,
                      QStringLiteral("__arktos.deliverResponse"),
                      QJsonObject{
                          {QStringLiteral("mid"), mid},
                          {QStringLiteral("response"),
                           QJsonValue(QJsonValue::Null)},
                          {QStringLiteral("hadListeners"), false}});
        }
    });
    m_pageDeliveryTimers.insert(mid, expiry);
    expiry->start();
    // The 'ext' scope tag lets each agent on the page drop pushes
    // addressed to a sibling extension — the push reposts to every
    // world on the document, so the check lives payload-side.
    emit page->extEvent(
        QStringLiteral("__arktos.deliver"),
        QJsonObject{{QStringLiteral("mid"), mid},
                    {QStringLiteral("ext"), extensionId},
                    {QStringLiteral("msg"), msg},
                    {QStringLiteral("sender"), sender}});
}

// -------------------------------------------------------------------
// ER10 — chrome.offscreen.  Chrome's single-document model: at most
// one offscreen document per extension, created by
// createDocument({url, reasons, justification}).  Ours is a second-
// class host page: a hidden, navigation-locked QWebEngineView on the
// extension's profile loaded at the declared bundle-relative url,
// wired through attachExtensionPage so the EXT17 sibling shim lands
// the full chrome.* surface + relay context (kind 'offscreen' —
// getContexts reports OFFSCREEN_DOCUMENT).  Chrome resolves
// createDocument once the document is usable; the equivalent honest
// signal here is the document's shim registering its relay context —
// after it, runtime.sendMessage really reaches the page.
// -------------------------------------------------------------------

namespace {

// Chrome's offscreen.Reason enum — validated, never silently
// dropped; the recorded set is what offscreenInfo() echoes back.
const QSet<QString> &offscreenReasons()
{
    static const QSet<QString> reasons = {
        QStringLiteral("TESTING"), QStringLiteral("AUDIO_PLAYBACK"),
        QStringLiteral("IFRAME_SCRIPTING"),
        QStringLiteral("DOM_SCRAPING"), QStringLiteral("BLOBS"),
        QStringLiteral("DOM_PARSER"), QStringLiteral("USER_MEDIA"),
        QStringLiteral("DISPLAY_MEDIA"), QStringLiteral("WEB_RTC"),
        QStringLiteral("CLIPBOARD"), QStringLiteral("LOCAL_STORAGE"),
        QStringLiteral("WORKERS"), QStringLiteral("BATTERY_STATUS"),
        QStringLiteral("MATCH_MEDIA"), QStringLiteral("GEOLOCATION"),
        QStringLiteral("COLLECTION")};
    return reasons;
}

} // namespace

QJsonObject ChromeBridge::invokeOffscreen(const QString &method,
                                          const QJsonObject &args)
{
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
    if (!m_backend)
        return err(QStringLiteral("backend-gone"));
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    // Chrome exposes chrome.offscreen only to extensions declaring
    // the 'offscreen' permission — the call-level gate stands in for
    // the absent namespace, same as chrome.scripting's.
    if (!runtime->hasExtensionPermission(
            m_extensionId, QStringLiteral("offscreen")))
        return err(QStringLiteral("no-permission"));
    if (method == QLatin1String("hasDocument"))
        return ok(runtime->offscreenHasDocument(m_extensionId));
    if (method == QLatin1String("closeDocument"))
        return runtime->offscreenClose(m_extensionId);
    if (method != QLatin1String("createDocument"))
        return err(QStringLiteral("unimplemented"));

    const QString docId = args.value(QStringLiteral("docId"))
        .toString();
    if (docId.isEmpty() || !ownsRelayKey(m_extensionId, docId))
        return err(QStringLiteral("invalid-argument"));

    // Chrome resolves parameters.url against the extension's origin
    // and requires the document to be inside it — only a bundle-
    // relative path (or the same-extension absolute spelling of
    // one) may ever become the document.  Same hygiene as the
    // background/files loaders: no schemes, no leading '/', no
    // traversal.
    QString rel = args.value(QStringLiteral("url")).toString();
    const QString origin =
        QStringLiteral("chrome-extension://") + m_extensionId;
    if (rel.startsWith(origin + QLatin1Char('/')))
        rel = rel.mid(origin.size() + 1);
    if (rel.isEmpty() || rel.startsWith(QLatin1Char('/'))
            || rel.contains(QLatin1Char(':'))
            || rel.split(QLatin1Char('/')).contains(
                QStringLiteral("..")))
        return err(QStringLiteral("bad-path"));
    const QString filePath = rel.section(QLatin1Char('?'), 0, 0)
        .section(QLatin1Char('#'), 0, 0);
    const QFileInfo fileInfo(
        QDir(m_extensionPath).absoluteFilePath(filePath));
    if (!fileInfo.isFile() || fileInfo.size() > 8 * 1024 * 1024)
        return err(QStringLiteral("no-such-file"));

    const QJsonArray reasonValues =
        args.value(QStringLiteral("reasons")).toArray();
    QStringList reasons;
    for (const QJsonValue &v : reasonValues) {
        const QString reason = v.toString();
        if (!offscreenReasons().contains(reason))
            return err(QStringLiteral("invalid-argument"));
        if (!reasons.contains(reason))
            reasons.append(reason);
    }
    if (reasons.isEmpty())
        return err(QStringLiteral("invalid-argument"));
    const QString justification =
        args.value(QStringLiteral("justification")).toString();

    return runtime->offscreenCreate(
        this, docId, origin + QLatin1Char('/') + rel, reasons,
        justification);
}

QJsonObject ExtensionBackgroundRuntime::offscreenCreate(
        ChromeBridge *caller, const QString &docId, const QString &url,
        const QStringList &reasons, const QString &justification)
{
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
    if (!caller || docId.isEmpty())
        return err(QStringLiteral("invalid-argument"));
    const QString extId = caller->extensionId();
    ExtensionBackgroundHost *host = m_hosts.value(extId);
    if (!host || !host->page() || !host->page()->profile())
        return err(QStringLiteral("not-hosted"));
    QWebEngineProfile *profile = host->page()->profile();
    if (m_offscreenDocs.contains(extId))
        // Chrome's rule — the shim maps this code onto the verbatim
        // message Bitwarden compares against.
        return err(QStringLiteral("single-document"));

    // The second-class host page: hidden, navigation-locked to the
    // declared url, channel + shared-backend bridge attached BEFORE
    // the load so the DocumentCreation shim's channel poll lands.
    auto *view = new QWebEngineView;
    auto *page = new HostedBackgroundPage(profile, extId,
                                          QUrl(url), view);
    view->setPage(page);
    if (!attachExtensionPage(page, extId)) {
        delete view;
        return err(QStringLiteral("attach-failed"));
    }

    OffscreenDoc doc;
    doc.view = view;
    doc.page = page;
    doc.bridge = m_pageBridges.value(page);
    doc.url = url;
    doc.reasons = reasons;
    doc.justification = justification;
    m_offscreenDocs.insert(extId, doc);

    PendingOffscreen pending;
    pending.caller = caller;
    pending.extensionId = extId;
    m_offscreenOps.insert(docId, pending);
    QTimer *expiry = new QTimer(this);
    expiry->setSingleShot(true);
    expiry->setInterval(25000);
    connect(expiry, &QTimer::timeout, this, [this, docId]() {
        // The document exists but its shim never came up — honest
        // timeout; the doc stays (hasDocument stays truthful) and
        // its own bridge keeps retrying the channel.
        resolveOffscreenOp(docId, QStringLiteral("boot-timeout"));
    });
    m_offscreenOpTimers.insert(docId, expiry);
    expiry->start();

    connect(page, &QWebEnginePage::loadFinished, this,
            [this, extId, docId](bool loadOk) {
        if (loadOk)
            return;   // registration resolves the op
        resolveOffscreenOp(docId, QStringLiteral("load-failed"));
        dropOffscreenDocument(extId);
    });
    // Hidden pages get throttled — keep the document Active and
    // nudged like the background host so the crypto/clipboard work
    // it exists for stays answerable.  The nudge timer is parented
    // to the page: it dies with the document.
    QPointer<QWebEnginePage> guard(page);
    QTimer *nudge = new QTimer(page);
    nudge->setInterval(kNudgeMs);
    connect(nudge, &QTimer::timeout, page, [guard]() {
        if (guard)
            guard->runJavaScript(QStringLiteral(
                "window.__arktosWake&&__arktosWake()"));
    });
    nudge->start();
    page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
    page->load(QUrl(url));
    return ok();
}

QJsonObject ExtensionBackgroundRuntime::offscreenClose(
        const QString &extensionId)
{
    // Closing with no live document is a typed failure, not a silent
    // no-op — the caller learns the document it thought it held is
    // already gone.
    if (!m_offscreenDocs.contains(extensionId)) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), false);
        o.insert(QStringLiteral("error"),
                 QStringLiteral("no-document"));
        return o;
    }
    dropOffscreenDocument(extensionId);
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    return o;
}

bool ExtensionBackgroundRuntime::offscreenHasDocument(
        const QString &extensionId) const
{
    const OffscreenDoc doc = m_offscreenDocs.value(extensionId);
    return doc.page && doc.view;
}

QWebEnginePage *ExtensionBackgroundRuntime::offscreenPage(
        const QString &extensionId) const
{
    return m_offscreenDocs.value(extensionId).page;
}

QJsonObject ExtensionBackgroundRuntime::offscreenInfo(
        const QString &extensionId) const
{
    const OffscreenDoc doc = m_offscreenDocs.value(extensionId);
    QJsonObject info;
    if (doc.page)
        info.insert(QStringLiteral("url"), doc.url);
    if (!doc.reasons.isEmpty()) {
        QJsonArray arr;
        for (const QString &r : doc.reasons)
            arr.append(r);
        info.insert(QStringLiteral("reasons"), arr);
    }
    if (!doc.justification.isEmpty())
        info.insert(QStringLiteral("justification"),
                    doc.justification);
    return info;
}

bool ExtensionBackgroundRuntime::isOffscreenBridge(
        ChromeBridge *bridge) const
{
    if (!bridge)
        return false;
    const OffscreenDoc doc =
        m_offscreenDocs.value(bridge->extensionId());
    return doc.bridge && doc.bridge == bridge;
}

void ExtensionBackgroundRuntime::resolveOffscreenOp(
        const QString &docId, const QString &error)
{
    const auto it = m_offscreenOps.find(docId);
    if (it == m_offscreenOps.end())
        return;
    const PendingOffscreen op = it.value();
    m_offscreenOps.erase(it);
    if (QTimer *timer = m_offscreenOpTimers.take(docId))
        timer->deleteLater();
    if (!op.caller)
        return;
    QJsonObject done{{QStringLiteral("docId"), docId}};
    if (error.isEmpty()) {
        done.insert(QStringLiteral("ok"), true);
    } else {
        done.insert(QStringLiteral("ok"), false);
        done.insert(QStringLiteral("error"), error);
    }
    emit op.caller->hostEvent(
        QStringLiteral("__arktos.offscreenDone"), done);
}

void ExtensionBackgroundRuntime::resolveOffscreenOps(
        ChromeBridge *bridge)
{
    if (!bridge)
        return;
    const QStringList docIds = m_offscreenOps.keys();
    for (const QString &docId : docIds) {
        const PendingOffscreen op = m_offscreenOps.value(docId);
        if (op.extensionId != bridge->extensionId())
            continue;
        const OffscreenDoc doc =
            m_offscreenDocs.value(op.extensionId);
        if (doc.bridge && doc.bridge == bridge)
            resolveOffscreenOp(docId);
    }
}

void ExtensionBackgroundRuntime::dropOffscreenDocument(
        const QString &extensionId)
{
    const OffscreenDoc doc = m_offscreenDocs.take(extensionId);
    // A create op parked on a dying document must not hang — fail it
    // before the view goes away.
    const QStringList docIds = m_offscreenOps.keys();
    for (const QString &docId : docIds) {
        if (m_offscreenOps.value(docId).extensionId == extensionId)
            resolveOffscreenOp(docId, QStringLiteral("host-gone"));
    }
    if (!doc.view)
        return;
    // detach unregisters the relay context and drops the page's
    // bridge synchronously — hasDocument/getContexts are honest the
    // moment closeDocument resolves.  The view itself goes away on
    // the event loop: a closeDocument issued from inside the
    // document must not tear down the channel while its own reply
    // is still being marshaled.
    if (doc.page)
        detachExtensionPage(doc.page);
    doc.view->deleteLater();
}

void ExtensionBackgroundRuntime::nudgeBridge(ChromeBridge *bridge)
{
    if (!bridge)
        return;
    const QString extId = bridge->extensionId();
    if (ExtensionBackgroundHost *host = m_hosts.value(extId)) {
        if (bridge == host->bridge()) {
            host->nudge();
            return;
        }
    }
    const OffscreenDoc doc = m_offscreenDocs.value(extId);
    if (doc.page && doc.bridge == bridge) {
        doc.page->runJavaScript(QStringLiteral(
            "window.__arktosWake&&__arktosWake()"));
    }
}
