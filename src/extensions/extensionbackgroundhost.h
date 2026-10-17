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

class QTimer;
class QWebChannel;
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

    QString m_extensionId;
    QString m_extensionName;
    // QPointer — a shared backend dies with the host that owns it;
    // a popup bridge must answer typed errors, not dangle.
    QPointer<ExtensionShimBackend> m_backend;
    QPointer<QWebEnginePage> m_contextPage;
};

// One hidden hosted view for one extension.  Owns the view, page,
// channel and bridge; arms the per-extension injection on the
// profile; polls the page's __aroraHost.state until the handshake +
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
    // #arora-hosted tells the per-extension injection it is hosting
    // (and not, say, a health probe reading the same file).
    static QUrl hostUrl(const QString &extensionId);
    static QString injectionName(const QString &extensionId);
    // EXT17 sibling-page injection — preamble + shim (no background
    // code) on every other top-level document at the extension's own
    // chrome-extension://<id>/ origin: the action popup, options
    // pages, offscreen documents.
    static QString pageInjectionName(const QString &extensionId);

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
                     const QJsonObject &payload);

    // Whether url-bearing fields may reach this extension ('tabs'
    // permission or a host permission covering the url).  Public so
    // chrome.tabs replies in ExtensionShimBackend apply the same
    // scrub rule the event fan-out does.
    bool seesUrls(const QString &extensionId, const QString &url) const;

    // EXT17 — chrome.runtime.getContexts backing: the registered
    // shim contexts of one extension as spec-shaped context entries
    // (contextType from the registered kind — BACKGROUND for the
    // host, POPUP for the action popup, TAB for a page hosted in a
    // real tab).
    QJsonArray contextsJson(const QString &extensionId) const;

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
               const QString &onlyExtensionId);
    QJsonObject shapedPayload(const QString &extensionId,
                              const QString &name,
                              const QJsonObject &payload) const;

    // Relay: live shimmed contexts and in-flight deliveries
    // (mid -> the sending bridge; entries expire on a timer so a
    // dead peer cannot pin the slot forever).
    struct RelayContext {
        QPointer<ChromeBridge> bridge;
        QString kind;           // "background" | "popup" | "page" | ...
        QString contextId;
        QString url;            // documentUrl the context registered
    };
    QList<RelayContext> m_relayContexts;
    int m_nextContextId = 0;
    struct PendingRelay {
        QPointer<ChromeBridge> origin;
        int peers = 0;
        int replies = 0;
    };
    QHash<QString, PendingRelay> m_relayPending;
    QHash<QString, QTimer *> m_relayPendingTimers;

    // Port channels: chan -> the registered ends.  An end owns its
    // local port id; messages arrive addressed by the sender's id
    // and leave re-addressed to each remote end's own id.
    struct PortEnd {
        QPointer<ChromeBridge> bridge;
        QString localPortId;
    };
    QHash<QString, QList<PortEnd>> m_portEnds;

    // EXT17: sibling-page bridges keyed by their page so an open
    // popup's channel is unregistered when the view goes away.
    QHash<QWebEnginePage *, QPointer<ChromeBridge>> m_pageBridges;
};

#endif // EXTENSIONBACKGROUNDHOST_H
