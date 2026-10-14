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
    explicit ChromeBridge(const QString &extensionId,
                          const QString &extensionName,
                          const QString &extensionPath,
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
    bool handshakeDone() const { return m_handshakeDone; }
    ExtensionShimBackend *backend() const { return m_backend; }

signals:
    void handshook();
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
    bool m_handshakeDone = false;
    ExtensionShimBackend *m_backend = nullptr;
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
    State state() const { return m_state; }
    QString lastError() const { return m_error; }
    QStringList errors() const { return m_errors; }
    // EXT15 diagnostics: benign shim notes and the typed-error gap
    // list the hosted page has surfaced so far.
    QStringList shimNotes() const;
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

signals:
    void stateChanged();

private:
    void setState(State state);
    void fail(const QString &why);
    void poll();
    void nudge();

    QPointer<QWebEngineProfile> m_profile;
    QString m_extensionId;
    QString m_extensionName;
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
    QStringList hostedIds() const;

    // EXT15 messaging relay — shimmed contexts (the hosted
    // background page now, action popups once EXT17 injects them)
    // register here keyed by extension id; relaySend() fans a
    // wrapped message out to the sender's sibling contexts via each
    // bridge's hostEvent push, and relayRespond() routes the
    // answering context's sendResponse back to the sender.  The
    // registry never crosses extension boundaries.
    QString registerRelayContext(ChromeBridge *bridge,
                                 const QString &kind);
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
                         const QString &portId, const QString &name);
    void relayPortAccept(ChromeBridge *accepter, const QString &chan,
                         const QString &portId);
    int relayPortMessage(ChromeBridge *from, const QString &chan,
                         const QString &portId,
                         const QJsonValue &msg);
    void relayPortDisconnect(ChromeBridge *from, const QString &chan,
                             const QString &portId);

signals:
    void hostChanged(const QString &extensionId);

private:
    explicit ExtensionBackgroundRuntime(QObject *parent = nullptr);
    void teardown(const QString &id);

    QList<QPointer<QWebEngineProfile>> m_profiles;
    QHash<QString, ExtensionBackgroundHost *> m_hosts;
    // Ids whose manifest carries no background scripts — cached so a
    // sync does not re-read the manifest on every pass.
    QSet<QString> m_noBackground;
    QTimer *m_heartbeat = nullptr;

    // Relay: live shimmed contexts and in-flight deliveries
    // (mid -> the sending bridge; entries expire on a timer so a
    // dead peer cannot pin the slot forever).
    struct RelayContext {
        QPointer<ChromeBridge> bridge;
        QString kind;           // "background" | "popup" | ...
        QString contextId;
    };
    QList<RelayContext> m_relayContexts;
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
};

#endif // EXTENSIONBACKGROUNDHOST_H
