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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef PICTUREINPICTURE_H
#define PICTUREINPICTURE_H

#include <qobject.h>
#include <qpointer.h>
#include <qvariant.h>

#include <functional>

class PipRequestBridge;
class PipWindow;
class WebView;

// PIP01: per-view Picture-in-Picture controller.
//
// Qt 6.12 exposes no PiP negotiation to embed — the deprecated
// QWebEnginePage::Feature enum's replacement,
// QWebEnginePermission::PermissionType, has no PiP entry, and
// Chromium's PictureInPictureWindowManager lives in //chrome/browser,
// outside the content layer QtWebEngine ships (the engine binary
// carries the content-side strings but no Qt wrapper exists).  So the
// documented fallback runs here: the video is "popped out" into
// PipWindow, an app-owned frameless stay-on-top window whose second
// WebEngine page — on the same profile, so auth/cookies apply —
// replays the stream, while the original element is paused, hidden
// and replaced by a placeholder (pip.js).  Closing the window writes
// the last reported position back into the tab.
//
// Entry points: the video context-menu item and View menu action
// (popOutContext/popOut), and the page's own requestPictureInPicture
// — wrapped by pip-shim.js (DocumentCreation script) and routed over
// the page's QWebChannel through PipRequestBridge, gated on transient
// user activation like Chromium.  MSE/blob: sources may refuse to
// attach cross-document; the player reports the media error and the
// element is restored.
class PictureInPicture : public QObject
{
    Q_OBJECT
    // The channel-facing half of the pair forwards JS calls here.
    friend class PipRequestBridge;

public:
    explicit PictureInPicture(WebView *view);

    bool isActive() const;
    PipWindow *window() const;

public slots:
    // View menu / toolbar path: pop out the largest (preferably
    // playing) video on the page.
    void popOut();
    // Context-menu path: the video under the click point, identified
    // by the request's media url when one exists.
    void popOutContext(const QUrl &mediaUrl, const QPoint &viewPos);
    // document.exitPictureInPicture() equivalent: restore + close.
    void exitPopOut();

signals:
    // One-line user-visible message for the status bar.
    void message(const QString &message);
    void activeChanged(bool active);

private:
    // PipRequestBridge relays the shim's channel calls through these.
    void requestPopOut(const QString &nonce);
    void closePopOut(const QString &nonce);

    void beginPopOut(const QString &nonce, const QVariantMap &spec);
    void onTagged(const QString &nonce, const QVariant &result);
    void detach(const QString &nonce);
    void onWindowClosing(const QVariantMap &state, bool raiseTab);
    void resolvePageRequest(const QString &nonce, bool ok,
                            const QString &reason = QString());
    // Runs program (the pip.js bundle plus a trailing call) in the
    // page's main world, lifting JavascriptEnabled for the injection
    // window when JSCTL has it off — same workaround as
    // ReaderMode::runDriver (a plain <video> still plays with scripts
    // blocked, so pop-out must not depend on page JS being allowed).
    void runPageJs(const QString &call,
                   const std::function<void(const QVariant &)> &callback
                       = std::function<void(const QVariant &)>());
    QString scriptBundle() const;

    WebView *m_view;
    PipRequestBridge *m_bridge;
    QPointer<PipWindow> m_window;
    QString m_activeNonce;
    int m_nonceCounter;
};

// Channel object registered as "aroraPip" on the page's existing
// QWebChannel (WebPage::init).  Reachable from arbitrary page script
// (SEC08), so it only ferries an opaque nonce — the C++ side re-finds
// the tagged element itself and the gesture gate lives in the shim
// (navigator.userActivation.isActive before the call).
class PipRequestBridge : public QObject
{
    Q_OBJECT

public:
    explicit PipRequestBridge(PictureInPicture *pip);

public slots:
    void requestPopOut(const QString &nonce);
    void exitPopOut(const QString &nonce);

signals:
    void popOutResolved(const QString &nonce, bool ok,
                        const QString &reason);
    void popOutClosed(const QString &nonce);

private:
    QPointer<PictureInPicture> m_pip;
};

#endif // PICTUREINPICTURE_H
