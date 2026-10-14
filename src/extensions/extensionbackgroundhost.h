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

// The channel object the hosted page's shim talks to.  EXT14 lands
// the handshake + error reporting only; EXT15 adds the API surface
// behind invoke().  Everything the extension sends is scoped to its
// own id — the bridge never crosses extension boundaries.
class ChromeBridge : public QObject
{
    Q_OBJECT

public:
    explicit ChromeBridge(const QString &extensionId,
                          const QString &extensionName,
                          QObject *parent = nullptr);

    // First call the shim makes — answers the extension's identity
    // so the preamble can finish chrome.runtime.id-style fields.
    Q_INVOKABLE QJsonObject handshake(const QJsonObject &hello);

    // Hosted-side error forwarding: window error/unhandledrejection
    // events land here so a misbehaving background is diagnosed
    // through the health surface instead of vanishing into stderr.
    Q_INVOKABLE void reportError(const QString &message);

    // Generic op dispatch stub for EXT15 — every call answers a
    // typed error ({ok:false, error:"unimplemented"}) until the real
    // surface lands, so a shim call never hangs on a void.
    Q_INVOKABLE QJsonObject invoke(const QString &ns,
                                   const QString &method,
                                   const QJsonObject &args);

    QString extensionId() const { return m_extensionId; }
    bool handshakeDone() const { return m_handshakeDone; }

signals:
    void handshook();
    void errorReported(const QString &message);

private:
    QString m_extensionId;
    QString m_extensionName;
    bool m_handshakeDone = false;
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
                            const QStringList &backgroundScripts,
                            QObject *parent = nullptr);
    ~ExtensionBackgroundHost() override;

    QString extensionId() const { return m_extensionId; }
    State state() const { return m_state; }
    QString lastError() const { return m_error; }
    QStringList errors() const { return m_errors; }

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
};

#endif // EXTENSIONBACKGROUNDHOST_H
