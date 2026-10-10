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

#ifndef PIPWINDOW_H
#define PIPWINDOW_H

#include <qpointer.h>
#include <qvariant.h>
#include <qwidget.h>

class PipPlayerBridge;
class QMouseEvent;
class QToolButton;
class QWebChannel;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

// PIP01: the floating Picture-in-Picture window.  Qt WebEngine has no
// PiP surface to negotiate (Chromium's PictureInPictureWindowManager
// lives in //chrome/browser, outside the content layer), so the video
// is replayed in an app-owned frameless always-on-top window: a second
// QWebEngineView on the SOURCE page's profile (cookies/auth apply)
// running the bundled pip-player.html document.  The strip carries
// "return to tab" and close buttons; closing hands the last reported
// play state back so the caller can restore the original element.
class PipWindow : public QWidget
{
    Q_OBJECT
    // The channel-facing half of the pair forwards JS calls here.
    friend class PipPlayerBridge;

public:
    // baseUrl must be the SOURCE page's URL: setHtml() stamps it as
    // the player document's origin, which is what lets file:// pages'
    // media load (a qrc: base would leave the player unable to reach
    // local files) and keeps same-origin checks honest for the rest.
    PipWindow(QWebEngineProfile *profile, const QVariantMap &video,
              const QUrl &baseUrl, QWidget *parent = nullptr);

    // Player document constructed its video element (channel hello).
    bool isReady() const { return m_ready; }
    // The media errored in the player (unreachable blob:, DRM, ...).
    bool hasError() const { return m_error; }
    QWebEngineView *playerView() const { return m_view; }
    // Where the browser chrome should land the video again; false once
    // the source view is gone — the close button then acts as "stop".
    void setReturnEnabled(bool enabled);
    // What the strip's return button does — emits returnToTab() with
    // the last reported state (smoke/tests drive it directly).
    void returnToTabNow() { emit returnToTab(m_lastState); }

signals:
    void ready();
    void failed(const QString &reason);
    // Both carry the last reported playback state for the caller to
    // write back into the source element; returnToTab also raises it.
    void closing(const QVariantMap &state);
    void returnToTab(const QVariantMap &state);

protected:
    void closeEvent(QCloseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void noteReady();
    void noteState(double time, bool paused, bool ended,
                   double duration);
    void noteError(int code);

    QWebEngineView *m_view;
    QWebEnginePage *m_page;
    QWebChannel *m_channel;
    PipPlayerBridge *m_bridge;
    QToolButton *m_returnButton;
    QVariantMap m_lastState;
    bool m_ready;
    bool m_error;
    bool m_closing;
    QPoint m_dragPos;
};

// Channel object registered as "aroraPipPlayer" on the player page.
// The player document is ours, so the trust boundary is narrow, but
// keep it to plain value types like the other bridges.
class PipPlayerBridge : public QObject
{
    Q_OBJECT

public:
    explicit PipPlayerBridge(PipWindow *window);

public slots:
    void notifyReady();
    void notifyError(int code);
    void reportState(double time, bool paused, bool ended,
                     double duration);

private:
    QPointer<PipWindow> m_window;
};

#endif // PIPWINDOW_H
