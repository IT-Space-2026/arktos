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

#ifndef EXTENSIONBACKGROUNDHOST_H
#define EXTENSIONBACKGROUNDHOST_H

#include <qobject.h>

#include <qjsonobject.h>   // moc needs the complete type for the
                          // by-value invokable signatures
#include <qjsonarray.h>
#include <qhash.h>
#include <qlist.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>

class ExtensionPageBridge;
class QTimer;
class QWebChannel;
class QWebEngineFrame;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

// EXT14 — hosted WebExtensions background runtime (the EXT13 spike's
// "Arch A" verdict made product code).  Qt WebEngine's MV3 host is a
// subset (EXT11: service workers can register, but half the chrome.*
// surface has no embedder delegate — Bitwarden's worker eval-dies).
// The hosted runtime gives every enabled extension that declares a
// background a hidden QWebEngineView on the extension's own profile,
// navigated to a generated wrapper document at the extension's
// chrome-extension://<id>/ origin.  A profile-level QWebEngineScript
// (per extension, gated on the wrapper URL fragment) lands at
// DocumentCreation in the main world — no package files are touched
// (SEC25 integrity pins stay valid): it installs the preamble
// (src/data/exthost.js: error capture, SW-globals, the qwebchannel
// handshake) then appends the extension's real background scripts in
// manifest order.  The page context keeps REAL chrome.runtime /
// storage / i18n; EXT15 fills the missing namespaces over the
// 'chromeBridge' channel registered here.
//
// Pieces:
//   ChromeBridge                 — the host-side QWebChannel object
//                                  EXT15's shim calls through.
//   ExtensionBackgroundHost      — one hidden hosted view.
//   ExtensionBackgroundRuntime   — the pool; reconcile() keeps the
//                                  set of live hosts in lock-step
//                                  with "enabled && has background"
//                                  on every prepared profile.
//
// Health: hosted state surfaces through ExtensionInfo::hostState /
// hostError.  EXT12's ExtensionHealth tiers live on the main lane —
// when they merge, a Ready host is the 'Hosted' tier outranking
// Limited/NotSupported.

class ExtensionShimBackend;

// The channel object the hosted page's shim talks to.  EXT14 lands
// the handshake + error reporting; EXT15 routes invoke() through
// ExtensionShimBackend (storage/alarms/tabs/action/i18n/permissions/
// contextMenus/sidePanel/notifications/runtime/shim) and handles the
// 'relay' namespace itself against ExtensionBackgroundRuntime's
// context registry.  Everything the extension sends is scoped to its
// own id — the bridge never crosses extension boundaries.
class ChromeBridge : public QObject
{
    Q_OBJECT

public:
    // EXT17: 'backend' shares an existing per-extension backend
    // (popup/sibling pages ride the same storage/alarms/action state
    // as the hosted background instead of forking it); nullptr builds
    // a private one owned by this bridge.
    explicit ChromeBridge(const QString &extensionId,
                          const QString &extensionName,
                          const QString &extensionPath,
                          ExtensionShimBackend *backend = nullptr,
                          QObject *parent = nullptr);

    // First call the shim makes — answers the extension's identity
    // plus the raw manifest.json so synchronous chrome.* lookups
    // (permissions/commands/getManifest fallback) never block.
    Q_INVOKABLE QJsonObject handshake(const QJsonObject &hello);

    // Hosted-side error forwarding: window error/unhandledrejection
    // events land here so a misbehaving background is diagnosed
    // through the health surface instead of vanishing into stderr.
    Q_INVOKABLE void reportError(const QString &message);

    // EXT15 generic op dispatch — backend namespaces answer real
    // results or typed errors; 'relay' is the messaging fan-out.
    Q_INVOKABLE QJsonObject invoke(const QString &ns,
                                   const QString &method,
                                   const QJsonObject &args);

    QString extensionId() const { return m_extensionId; }
    // Out-of-line — QPointer needs the complete type.
    ExtensionShimBackend *backend() const;

    // EXT17: the page this bridge serves — a popup/options document
    // resolves chrome.tabs.getCurrent against the tab hosting it.
    // The background host's own page is never a tab.  (Out-of-line —
    // QPointer needs the complete type.)
    void setContextPage(QWebEnginePage *page);
    QWebEnginePage *contextPage() const;

signals:
    void errorReported(const QString &message);
    // EXT15 health diagnostics — a shimmed API answered a typed
    // error (the extension touched something that cannot work).
    void gapReported(const QString &api, const QString &error);
    // Host->page push channel: relay envelopes/responses and the
    // EXT16 event deliveries the shim fans out to listeners.
    void hostEvent(const QString &name, const QJsonObject &payload);

private:
    QJsonObject invokeRelay(const QString &method,
                            const QJsonObject &args);
    // ER02: tabs.sendMessage — needs the calling bridge itself (the
    // page agent's response resolves onto it), so it lives at this
    // level like getCurrent rather than in the shared backend.
    QJsonObject invokeTabMessage(const QJsonObject &args);
    // ER07/ER08: chrome.scripting.executeScript/insertCSS/removeCSS —
    // async for the same reason: the per-frame results resolve onto
    // THIS bridge's hostEvent as '__arktos.execResult', so dispatch
    // lives here.
    QJsonObject invokeScripting(const QString &method,
                                const QJsonObject &args);
    // ER09: chrome.scripting.registerContentScripts /
    // unregisterContentScripts / getRegisteredContentScripts /
    // updateContentScripts — synchronous registry ops; the registry
    // itself lives on the runtime so every context of one extension
    // (background, popup, offscreen) shares it.
    QJsonObject invokeScriptRegistration(const QString &method,
                                         const QJsonObject &args);
    // ER09: chrome.contentScripts.register/unregister/getScripts —
    // the Firefox-shape surface the bundled webextension-polyfill
    // resolves browser.contentScripts.* to.  Same registry, MV2
    // option shapes.
    QJsonObject invokeContentScripts(const QString &method,
                                     const QJsonObject &args);
    // ER10: chrome.offscreen.createDocument/closeDocument/hasDocument.
    // createDocument is async like scripting.executeScript — the
    // answer lands on this bridge's hostEvent as
    // '__arktos.offscreenDone' once the spawned document's shim
    // registers its relay context (the messaging-ready signal).
    QJsonObject invokeOffscreen(const QString &method,
                                const QJsonObject &args);
    // ER11: chrome.webRequest.subscribe/unsubscribe — the observe-
    // only listener registry lives in ExtensionRequestWatcher; the
    // bridge is the listener's lifetime context.
    QJsonObject invokeWebRequest(const QString &method,
                                 const QJsonObject &args);
    // ER17: storage 'changed' — the writer's shim already fired its
    // own onChanged synchronously, so the sibling fan-out must skip
    // THIS bridge's context or the writer's listeners run twice.
    // Dispatch lives at this level because the bridge is the
    // poster's identity (the backend is shared per-extension).
    QJsonObject invokeStorageChanged(const QJsonObject &args);
    void finishExecFrame(const QString &execId, int slot,
                         int frameId, const QVariant &value,
                         QWebEngineFrame frame, quint32 worldId,
                         const QString &key);
    void finishExecSlot(const QString &execId, int slot,
                        const QJsonObject &entry);
    void pollExecResult(const QString &execId, int slot, int frameId,
                        QWebEngineFrame frame, quint32 worldId,
                        const QString &key, int triesLeft);

    // ER07/ER08: in-flight scripting fan-outs — execId -> one result
    // slot per selected frame plus an expiry so a frame whose eval
    // never answers cannot pin the call forever.
    struct PendingExec {
        int remaining = 0;
        QList<int> frameIds;   // slot -> DFS frame position
        QJsonArray results;
        QTimer *expiry = nullptr;
    };
    QHash<QString, PendingExec> m_pendingExecs;

    QString m_extensionId;
    QString m_extensionName;
    // ER07: executeScript's files[] resolve against the unpacked
    // bundle — the bridge keeps its own copy since the backend's
    // accessor is private.
    QString m_extensionPath;
    // QPointer — a shared backend dies with the host that owns it;
    // a popup bridge must answer typed errors, not dangle.
    QPointer<ExtensionShimBackend> m_backend;
    QPointer<QWebEnginePage> m_contextPage;
};

// One hidden hosted view for one extension.  Owns the view, page,
// channel and bridge; arms the per-extension injection on the
// profile; polls the page's __arktosHost.state until the handshake +
// script boot settle (Ready) or something breaks (Failed).  A hosted
// JS crash can only take the render process — the host notices via
// renderProcessTerminated and reports through lastError(), never
// taking the application down with it.
class ExtensionBackgroundHost : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Starting,   // page loading / boot in progress
        Ready,      // scripts ran + chromeBridge handshake completed
        Failed      // boot failed — lastError() says why
    };

    ExtensionBackgroundHost(QWebEngineProfile *profile,
                            const QString &extensionId,
                            const QString &extensionName,
                            const QString &extensionPath,
                            const QStringList &backgroundScripts,
                            QObject *parent = nullptr);
    ~ExtensionBackgroundHost() override;

    QString extensionId() const { return m_extensionId; }
    QString extensionName() const { return m_extensionName; }
    QString extensionPath() const { return m_extensionPath; }
    State state() const { return m_state; }
    QString lastError() const { return m_error; }
    QStringList errors() const { return m_errors; }
    // EXT15 diagnostics: the typed-error gap list the hosted page
    // has surfaced so far.
    QStringList shimGaps() const;
    ChromeBridge *bridge() const { return m_bridge; }

    // Diagnostics + test seam: the hidden view/page actually hosting
    // the background scripts.
    QWebEngineView *view() const { return m_view; }
    QWebEnginePage *page() const { return m_page; }

    // The generated wrapper document is the extension's guaranteed-
    // present manifest.json under a marker fragment — the resource
    // resolves to the real manifest bytes (bindings intact), while
    // #arktos-hosted tells the per-extension injection it is hosting
    // (and not, say, a health probe reading the same file).
    static QUrl hostUrl(const QString &extensionId);
    static QString injectionName(const QString &extensionId);
    // EXT17 sibling-page injection — preamble + shim (no background
    // code) on every other top-level document at the extension's own
    // chrome-extension://<id>/ origin: the action popup, options
    // pages, offscreen documents.
    static QString pageInjectionName(const QString &extensionId);
    // ER01 page-agent injection — installs __arktosExtAgent +
    // a scoped chrome.* in the shared AgentWorldId on every
    // document; the union of this extension's content-script matches
    // (contentScriptMatches) plus its host_permissions decide where
    // chrome actually lands.
    static QString pageAgentInjectionName(const QString &extensionId);
    static QString contentScriptInjectionName(const QString &extensionId,
                                              int index);
    // ER09: a dynamically-registered content script's profile-script
    // name — 'arktos-extdcs-<id>-<regId>' (regId sanitized) so host
    // teardown can sweep them by prefix like the declared set.
    static QString dynamicContentScriptInjectionName(
            const QString &extensionId, const QString &registrationId);
    // Union of the manifest's content_scripts[].matches — the page
    // agent's js-side grant list and the C++ agentCovers check share
    // it with host_permissions.
    QStringList contentScriptMatches() const { return m_csMatches; }

    // One bounded wake eval — the runtime calls it after pushing a
    // hostEvent so the delivery drains even while the hidden page's
    // own nudge timer is between ticks (throttled contexts).
    void nudge();

signals:
    void stateChanged();

private:
    void setState(State state);
    void fail(const QString &why);
    void poll();

    QPointer<QWebEngineProfile> m_profile;
    QString m_extensionId;
    QString m_extensionName;
    QString m_extensionPath;
    QStringList m_scripts;
    QStringList m_csMatches;        // union content_scripts[].matches
    State m_state = State::Starting;
    QString m_error;
    QStringList m_errors;

    QWebEngineView *m_view = nullptr;
    QWebEnginePage *m_page = nullptr;
    QWebChannel *m_channel = nullptr;
    ChromeBridge *m_bridge = nullptr;
    QTimer *m_poll = nullptr;
    QTimer *m_nudge = nullptr;
};

// The pool — one hidden host per enabled extension declaring a
// background.  attachProfile() is driven from
// ExtensionManager::installOnProfile(); syncNow() reconciles the
// live set with the engine's reported enabled/loaded state (called
// on every ExtensionManager::changed() and re-checked on a slow
// heartbeat so an enable the signals miss — e.g. the bundled-
// install path driving the Qt manager directly — still lands).
class ExtensionBackgroundRuntime : public QObject
{
    Q_OBJECT

public:
    static ExtensionBackgroundRuntime *instance();

    void attachProfile(QWebEngineProfile *profile);
    void syncNow();

    ExtensionBackgroundHost *host(const QString &extensionId) const;

    // EXT17 — wire an extension-owned page (the action popup, an
    // options tab, an offscreen document) into the shim: attaches a
    // QWebChannel carrying a ChromeBridge so the page-side
    // injection's handshake lands and its chrome.* calls reach the
    // hosted background through the relay.  Call BEFORE the page
    // finishes loading (the DocumentCreation preamble polls briefly
    // for the channel).  Returns false when the extension is not
    // hosted or the page is on an off-the-record/tor profile — a
    // plain popup still works natively then.  extensionId may be
    // left empty to derive it from an already-set chrome-extension
    // page url.
    bool attachExtensionPage(QWebEnginePage *page,
                             const QString &extensionId = QString());
    // The page navigated away from the extension document — its
    // relay context and bridge die with the document it served.
    void detachExtensionPage(QWebEnginePage *page);
    // EXT19 diagnostics/test seam — true while the page carries a
    // live chromeBridge.
    bool hasExtensionPage(QWebEnginePage *page) const;

    // EXT15 messaging relay — shimmed contexts (the hosted
    // background page and EXT17's injected sibling pages) register
    // here keyed by extension id; relaySend() fans a
    // wrapped message out to the sender's sibling contexts via each
    // bridge's hostEvent push, and relayRespond() routes the
    // answering context's sendResponse back to the sender.  The
    // registry never crosses extension boundaries.
    QString registerRelayContext(ChromeBridge *bridge,
                                 const QString &kind,
                                 const QString &url = QString());
    void unregisterRelayContext(ChromeBridge *bridge);
    int relaySend(ChromeBridge *from, const QString &mid,
                  const QJsonObject &envelope);
    bool relayRespond(ChromeBridge *responder, const QString &mid,
                      const QJsonValue &response);

    // EXT15 port routing — chrome.runtime.connect() links two
    // shimmed contexts over addressed channels ('chan').  Each end
    // registers its own local port id; postMessage is re-addressed
    // to the remote end's local id so both halves of a self-connect
    // can live in one context without colliding.
    int relayPortConnect(ChromeBridge *from, const QString &chan,
                         const QString &portId, const QString &name,
                         const QString &senderUrl);
    void relayPortAccept(ChromeBridge *accepter, const QString &chan,
                         const QString &portId);
    int relayPortMessage(ChromeBridge *from, const QString &chan,
                         const QString &portId,
                         const QJsonValue &msg);
    void relayPortDisconnect(ChromeBridge *from, const QString &chan,
                             const QString &portId);

    // EXT16 event pushes — browser-originated events (tabs,
    // webNavigation, contextMenus, windows) fan out through every
    // registered shim context's hostEvent signal.  broadcastEvent
    // reaches all contexts; deliverEvent targets one extension's
    // contexts (contextMenus.onClicked goes only to the item's
    // owner).  Chrome permission parity: webNavigation/contextMenus
    // events reach only extensions declaring the namespace's
    // permission, and url-bearing fields in tabs/windows payloads
    // are scrubbed unless the extension holds 'tabs' or a matching
    // host permission; host-internal pushes (relay, ports, alarms)
    // are ungated.
    int broadcastEvent(const QString &name,
                       const QJsonObject &payload);
    int deliverEvent(const QString &extensionId, const QString &name,
                     const QJsonObject &payload,
                     ChromeBridge *exclude = nullptr);

    // ER17: one mutation's change record fanned out to BOTH event
    // surfaces — chrome.storage.onChanged AND the per-area
    // chrome.storage.<area>.onChanged — each fired with the
    // (changes, areaName) listener arguments Chrome passes.
    // 'exclude' skips the posting context: its shim already fired
    // the event locally when the mutation resolved.
    void fanOutStorageChanged(const QString &extensionId,
                              const QString &area,
                              const QJsonObject &changes,
                              ChromeBridge *exclude = nullptr);

    // Whether url-bearing fields may reach this extension ('tabs'
    // permission or a host permission covering the url).  Public so
    // chrome.tabs replies in ExtensionShimBackend apply the same
    // scrub rule the event fan-out does.
    bool seesUrls(const QString &extensionId, const QString &url) const;

    // ER11: whether a request to `url` may reach this extension's
    // webRequest events — Chrome scopes them by host_permissions
    // (the 'webRequest' permission gates the namespace itself; a
    // listener's urls filter grants no coverage of its own).
    bool requestVisibleTo(const QString &extensionId,
                          const QUrl &url) const;

    // ------------------------------------------------------------------
    // ER09 — dynamic content-script registration.  Chrome's
    // scripting.registerContentScripts and Firefox's
    // contentScripts.register share one per-extension registry here —
    // any shim context of the extension reaches the same set.  A
    // registered spec joins the page-agent injection set as a
    // profile-level QWebEngineScript in AgentWorldId (same world and
    // gate shape as a declared content script); every match pattern is
    // re-proved against the extension's host coverage BEFORE the spec
    // is armed, and specs with persistAcrossSessions live in
    // ProfileSettings under 'extensions/regscripts/<id>' so a runtime
    // restart re-arms them exactly like Chrome.
    //
    // Replies use the shared {ok,result}|{ok:false,error} shapes;
    // 'not-found'/'invalid-argument'/'no-host-access'/'unsupported'
    // are the typed failures.
    QJsonObject registerContentScripts(
            const QString &extensionId, const QString &extensionPath,
            const QJsonArray &scripts, const QString &api);
    QJsonObject unregisterContentScripts(
            const QString &extensionId, const QStringList &ids);
    QJsonArray registeredContentScripts(
            const QString &extensionId, const QString &api,
            const QStringList &onlyIds = QStringList()) const;
    QJsonObject updateContentScripts(
            const QString &extensionId, const QString &extensionPath,
            const QJsonArray &scripts);
    // syncNow: a fresh host re-arms every persisted + session
    // registration of its extension onto its profile.
    void restoreRegisteredScripts(const QString &extensionId);
    // Uninstall: drop the whole in-memory registry (and its armed
    // profile scripts) — Chrome discards registrations with the
    // extension, settings record included.
    void dropRegisteredScripts(const QString &extensionId);
    // Test seam: the raw normalized spec set (id -> spec) — the same
    // map getScripts/getRegisteredContentScripts read.
    QJsonObject registeredScriptSpecs(const QString &extensionId) const;

    // Whether the extension's manifest declares the named API
    // permission — the page agent's chrome.storage gate.
    bool hasExtensionPermission(const QString &extensionId,
                                const QString &permission) const;

    // ------------------------------------------------------------------
    // ER01 — page agent plumbing.  The content-script half of the
    // hosted runtime: each hosted extension's host arms a profile-
    // wide QWebEngineScript pair (the page agent + its declared
    // content_scripts) inside AgentWorldId — an isolated world the
    // page's own script can share the DOM with but never see
    // chrome.* in (Qt caps script worlds at 0..256, so the agents of
    // every extension share ONE realm; each content script runs in a
    // closure receiving its own extension's `chrome` binding).
    // The agent's calls reach the per-WebPage 'arktosExt' channel
    // object (ExtensionPageBridge) through the SEC24 whitelisted
    // postMessage forwarder; because that transport is reachable by
    // any world on the page, agentCovers() is re-run on EVERY call —
    // the extension's host_permissions or declared content-script
    // matches must cover the page's url, else the typed
    // 'no-host-access' absent-API reply, never a silent no-op.
    static constexpr quint32 AgentWorldId = 0x40;

    // True while a hosted background exists for the id.
    bool isHosted(const QString &extensionId) const;
    // Per-call security check: is pageUrl covered by the extension's
    // host_permissions or declared content-script matches?  Plain
    // Chrome match semantics — originless documents (data:, about:)
    // are never covered; the page-level channel bootstrap does not
    // inject there anyway, so no bridge traffic could exist.
    bool agentCovers(const QString &extensionId,
                     const QUrl &pageUrl) const;

    // The ExtensionPageBridge ops — send routes a page-originated
    // chrome.runtime.sendMessage into the extension's registered
    // shim contexts (host background, popup, options); respond
    // resolves a pending host->page delivery (ER02's fan-in).
    QJsonObject pageSend(ExtensionPageBridge *page,
                         const QString &extensionId,
                         const QString &mid,
                         const QJsonValue &msg,
                         const QJsonObject &sender);
    QJsonObject pageRespond(ExtensionPageBridge *page,
                            const QString &mid,
                            const QJsonValue &response,
                            bool hadListeners);
    // Ports: a page-originated runtime.connect() registers the page
    // end and fans '__arktos.portConnect' to the extension's shim
    // contexts (their onConnect fires -> portAccept registers the
    // remote ends).  Message/disconnect re-address to the surviving
    // ends of either kind (ChromeBridge or ExtensionPageBridge).
    QJsonObject pagePortConnect(ExtensionPageBridge *page,
                                const QString &extensionId,
                                const QString &chan,
                                const QString &portId,
                                const QString &name,
                                const QJsonObject &sender);
    QJsonObject pagePortAccept(ExtensionPageBridge *page,
                               const QString &chan,
                               const QString &portId);
    QJsonObject pagePortMessage(ExtensionPageBridge *page,
                                const QString &chan,
                                const QString &portId,
                                const QJsonValue &msg);
    QJsonObject pagePortDisconnect(ExtensionPageBridge *page,
                                   const QString &chan,
                                   const QString &portId);
    // The page (or its bridge) died — drop its pending sends and port
    // ends; surviving ends get the honest portDisconnect push.
    void unregisterPageContext(ExtensionPageBridge *page);
    // ER02 seam — push a host->page message ('__arktos.deliver');
    // the agent's 'respond' op resolves it back to 'waiter' as
    // '__arktos.deliverResponse'.
    void pageDeliver(ExtensionPageBridge *page,
                     const QString &extensionId,
                     const QString &mid,
                     const QJsonValue &msg,
                     const QJsonObject &sender,
                     QObject *waiter);

    // EXT17 — chrome.runtime.getContexts backing: the registered
    // shim contexts of one extension as spec-shaped context entries
    // (contextType from the registered kind — BACKGROUND for the
    // host, POPUP for the action popup, OFFSCREEN_DOCUMENT for an
    // ER10 offscreen page, TAB for a page hosted in a real tab).
    // ER15: tabId/windowId resolve to the real browser tab when the
    // context's page lives in one; every registration change pushes
    // '__arktos.contexts' to the extension's contexts so a shim's
    // synchronous chrome.extension.getViews cache stays honest.
    QJsonArray contextsJson(const QString &extensionId) const;
    // ER15 — chrome.extension.getViews handle capability: reload the
    // document of one registered context (the cross-context
    // Window.location.reload() a real Window reference would carry).
    // Scoped to the caller's own extension like every relay op.
    QJsonObject reloadContextPage(const QString &extensionId,
                                  const QString &contextId);

    // ------------------------------------------------------------------
    // ER10 — chrome.offscreen.createDocument/hasDocument/closeDocument.
    // Chrome's single-document model: at most one offscreen document
    // per extension, a hidden navigation-locked QWebEngineView on the
    // extension's own profile loaded at the declared bundle-relative
    // URL — a second-class host page with NO toolbar wiring and the
    // full EXT17 sibling shim (own ChromeBridge, shared backend,
    // relay registration of kind 'offscreen'), so chrome.* calls and
    // runtime messaging inside it work exactly like the popup's do.
    // This is what Bitwarden's Argon2/localStorage/clipboard helpers
    // need.  The engine's native binding is overridden shim-side —
    // its document would carry no channel and no relay context.
    //
    // createDocument is async: the accepted call parks a pending op
    // keyed by the caller-minted docId and resolves it (hostEvent
    // '__arktos.offscreenDone') when the document's shim registers —
    // the earliest moment a sendMessage to the document can land.
    QJsonObject offscreenCreate(ChromeBridge *caller,
                                const QString &docId,
                                const QString &url,
                                const QStringList &reasons,
                                const QString &justification);
    QJsonObject offscreenClose(const QString &extensionId);
    bool offscreenHasDocument(const QString &extensionId) const;
    // Test/diagnostics seams: the live page, and the recorded create
    // parameters {url, reasons, justification}.
    QWebEnginePage *offscreenPage(const QString &extensionId) const;
    QJsonObject offscreenInfo(const QString &extensionId) const;

    // ER05 test seam — a simulated browser relaunch inside one
    // process: clears the per-session runtime.onStartup dedupe so
    // the next host Ready re-evaluates the version record exactly
    // like a restart (the record itself lives in ProfileSettings
    // and persists, as on disk).
    void restartLifecycleSession();

signals:
    void hostChanged(const QString &extensionId);

protected:
    // EXT19: application-level watch — the live action popup (and
    // any other extension document embedder that does not know about
    // attachExtensionPage) self-wires.  See the cpp for the details.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit ExtensionBackgroundRuntime(QObject *parent = nullptr);
    void teardown(const QString &id);
    void watchExtensionViews();
    void watchView(QWebEngineView *view);
    void syncPageBridge(QWebEngineView *view);

    QList<QPointer<QWebEngineProfile>> m_profiles;
    QHash<QString, ExtensionBackgroundHost *> m_hosts;
    // Ids whose manifest carries no background scripts — cached so a
    // sync does not re-read the manifest on every pass.
    QSet<QString> m_noBackground;
    // EXT16: manifest permission snapshots per extension id — the
    // event fan-out gates on these so delivery never re-reads the
    // manifest from disk.
    QHash<QString, QStringList> m_extPermissions;
    QHash<QString, QStringList> m_extHostPermissions;
    QTimer *m_heartbeat = nullptr;
    bool m_viewFilterInstalled = false;

    int fanOut(const QString &name, const QJsonObject &payload,
               const QString &onlyExtensionId,
               ChromeBridge *exclude = nullptr);
    QJsonObject shapedPayload(const QString &extensionId,
                              const QString &name,
                              const QJsonObject &payload) const;

    // ER05 — the runtime.onInstalled/onStartup dispatch a host
    // Ready runs.  See the cpp for the version-record semantics.
    void dispatchRuntimeLifecycle(const QString &extensionId);
    // Ids whose onStartup disposition this app session already
    // decided — a mid-session respawn or re-enable never re-fires
    // it; the set clears only on restartLifecycleSession().
    QSet<QString> m_lifecycleSeen;

    // ER09 — the dynamic content-script registry: extId -> regId ->
    // normalized spec.  Session lifetime for
    // persistAcrossSessions:false entries; persisted entries also
    // live in ProfileSettings (extensions/regscripts/<id>) and are
    // merged back in by restoreRegisteredScripts() when a host
    // respawns.  Armed state is a profile QWebEngineScript named
    // arktos-extdcs-<extId>-<regId> — host teardown sweeps them by
    // prefix with the declared set.
    void armRegisteredScript(const QString &extensionId,
                           const QJsonObject &spec);
    void disarmRegisteredScripts(const QString &extensionId,
                                 const QString &registrationId);
    void persistRegisteredScripts(const QString &extensionId);
    QString regProfileId(const QString &extensionId) const;
    QHash<QString, QHash<QString, QJsonObject>> m_regScripts;

    // Relay: live shimmed contexts and in-flight deliveries
    // (mid -> the sending bridge; entries expire on a timer so a
    // dead peer cannot pin the slot forever).
    struct RelayContext {
        QPointer<ChromeBridge> bridge;
        // ER15: stored at registration — a dead bridge still lets a
        // contexts push reach the rest of its extension.
        QString extensionId;
        QString kind;           // "background" | "popup" | "page" | ...
        QString contextId;
        QString url;            // documentUrl the context registered
    };
    // ER15: push '__arktos.contexts' (the current contextsJson) to
    // every live context of the extension — the synchronous
    // getViews cache each shim keeps.
    void pushContexts(const QString &extensionId);
    QList<RelayContext> m_relayContexts;
    int m_nextContextId = 0;
    // ER01: pending origins are generalized to QObject — a page
    // agent's sendMessage can wait for a shim context's reply
    // exactly like a popup's can (the response is emitted on the
    // right signal for each kind: hostEvent vs extEvent).
    struct PendingRelay {
        QPointer<QObject> origin;
        QString extensionId;
        int peers = 0;
        int replies = 0;
    };
    QHash<QString, PendingRelay> m_relayPending;
    QHash<QString, QTimer *> m_relayPendingTimers;

    // Port channels: chan -> the registered ends.  An end owns its
    // local port id; messages arrive addressed by the sender's id
    // and leave re-addressed to each remote end's own id.  ER01: an
    // end's target is a ChromeBridge (shim context) or an
    // ExtensionPageBridge (page agent) — deliveries are emitted on
    // hostEvent / extEvent respectively.
    struct PortEnd {
        QPointer<QObject> target;
        QString localPortId;
    };
    QHash<QString, QList<PortEnd>> m_portEnds;

    // ER01/ER02: host->page deliveries awaiting the page agent's
    // 'respond' — mid -> {the page asked, the context awaiting the
    // answer}.  Entries expire like relay sends so an absent or
    // silent receiver resolves the caller instead of pinning it.
    struct PendingPageDelivery {
        QPointer<QObject> waiter;
        QPointer<ExtensionPageBridge> target;
        QString extensionId;
    };
    QHash<QString, PendingPageDelivery> m_pageDeliveries;
    QHash<QString, QTimer *> m_pageDeliveryTimers;

    // EXT17: sibling-page bridges keyed by their page so an open
    // popup's channel is unregistered when the view goes away.
    QHash<QWebEnginePage *, QPointer<ChromeBridge>> m_pageBridges;

    // ER10 — offscreen documents: extId -> the one document Chrome's
    // single-document rule allows, and in-flight createDocument ops
    // keyed by the caller-minted "<extId>#..." docId (the same
    // capability-prefix convention as relay mids/execIds).
    struct OffscreenDoc {
        QPointer<QWebEngineView> view;   // owns page/bridge/channel
        QPointer<QWebEnginePage> page;
        QPointer<ChromeBridge> bridge;
        QString url;
        QStringList reasons;
        QString justification;
    };
    struct PendingOffscreen {
        QPointer<ChromeBridge> caller;
        QString extensionId;
    };
    QHash<QString, OffscreenDoc> m_offscreenDocs;
    QHash<QString, PendingOffscreen> m_offscreenOps;
    QHash<QString, QTimer *> m_offscreenOpTimers;
    bool isOffscreenBridge(ChromeBridge *bridge) const;
    void resolveOffscreenOp(const QString &docId,
                            const QString &error = QString());
    // The offscreen document's shim registered its relay context —
    // every pending createDocument op on that bridge resolves.
    void resolveOffscreenOps(ChromeBridge *bridge);
    void dropOffscreenDocument(const QString &extensionId);
    // Push-then-wake for the hidden contexts an event/replay targets
    // — the host page and any offscreen document.
    void nudgeBridge(ChromeBridge *bridge);
};

#endif // EXTENSIONBACKGROUNDHOST_H
