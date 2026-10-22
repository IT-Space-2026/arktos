/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#ifndef SERVOBACKEND_H
#define SERVOBACKEND_H

// ENG05 — the Servo backend for the Engine interface.
//
// Compiled only under `qmake servo=1` (ARKTOS_SERVO) and useful only
// when the servo-embed cdylib resolves at runtime — the artifact is a
// ~1.1GB out-of-tree build (spikes/servo/servo-embed, pinned libservo
// v0.7.0 family per the ENG03 verdict) that cannot be rebuilt inside
// a task-run budget, so the backend dlopens it and reports itself
// unavailable when it is absent; chrome hides the dead option rather
// than shipping a guaranteed-fail menu entry.
//
// Honest capability surface per .devin/SERVO-SPIKE.md: no SOCKS5
// (Tor windows stay Chromium — the swap path refuses), no per-webview
// settings, no find-in-page, no download delegate, no cert-error
// delegate, one-way JS eval, no devtools transport (servo's devtools
// crate speaks the Firefox remote protocol — reachable only as a
// TCP listener the BiDi panel cannot read, so devTools stays false
// and chrome disables the entry; ENG35).  Request interception IS
// enforced since ENG35 — the shim's load gate consults the tab's
// profile RequestPolicy per resource load; on artifacts predating
// se_set_load_gate the capability drops back to false honestly.
// Everything the backend cannot do is reported through
// capabilities()/no-op overrides, never faked.
//
// Rendering is the spike's software path — se_frame_copy blits an
// RGBA image into a QImage on frame_ready.  The C ABI exposes no GL
// surface handoff, so the QOpenGLWidget/surfman upgrade needs a shim
// extension first (noted in the spike doc).
//
// ENG36: any number of LIVE ServoViews at once.  libservo v0.7.0
// parks Opts in a process-global OnceLock — so the ENGINE is built
// once (se_engine_init, idempotent) and kept for the process
// lifetime — but it hosts N WebViews on that engine (a
// WebViewId→WebView map, a painter per rendering context).  A
// SeInstance is only a webview + delegate + rendering context over
// the engine, so the shim's se_view_create/se_view_destroy mint and
// retire per-tab views independently: closing (or crashing) a Servo
// tab destroys ONLY its view while the engine and every sibling view
// survive — the ENG28 fresh-instance crash recovery, now per-view.
// The one-tab-at-a-time limit this used to declare was the shim's
// g_servoSlot gate, never a libservo bound.
//
// What is still once-per-engine: the event-loop waker (bound at
// engine build to se_engine_init's wake fn + userdata for the
// process lifetime — the backend passes an engine-level indirection
// that pumps through whatever view is live, never a per-view
// pointer) and the failure state (a failed se_engine_init stays
// terminal for the session; a failed se_view_create does not).
//
// ENG27: the shim unregisters a view FIRST in se_view_destroy,
// before the catch-wrapped teardown, so a panicking drop (the
// desktop log's webrender GL panic) cannot strand the registry.
// The Qt side reads liveness back through se_instance_active/
// se_can_init/se_view_count on artifacts new enough to export them.

#include "engineinterface.h"

#include <qbasictimer.h>
#include <qelapsedtimer.h>
#include <qhash.h>
#include <qimage.h>
#include <qlist.h>
#include <qpointer.h>
#include <qpoint.h>
#include <qurl.h>
#include <qwidget.h>

class QPushButton;
class ServoPage;
struct SeLoadInfo;
struct SeHostReply;

// QWidget hosting one libservo WebView through the servo-embed C ABI
// (the ENG03 spike harness's ServoView, grown signal coverage for the
// Engine::Page contract).  Owns the SeInstance: painting, input
// forwarding and the event-loop wake marshaling all live here.
class ServoView : public QWidget
{
    Q_OBJECT

public:
    explicit ServoView(QWidget *parent = nullptr);
    ~ServoView() override;

    bool isReady() const;
    // ENG20: the shim caught a panic inside servo and poisoned this
    // instance — every further export is a no-op and the tab is dead
    // (retry in place via restartRequested, or swap back to Chromium
    // to keep browsing).  Always false on artifacts predating
    // se_is_poisoned.
    bool isPoisoned() const;
    // ENG23: GL_RENDERER of the painter's context — names the software
    // rasterizer once the shim pins it, empty on artifacts predating
    // se_gl_renderer.
    QString glRenderer() const;
    // Whether a ServoView may be constructed — ENG36: engine-alive,
    // not slot-free.  false only after a terminally failed engine
    // build (se_engine_init): a live sibling view is no longer a
    // reason to refuse.
    static bool instanceAvailable();
    // ENG27/ENG36: the shim-side live-view registry — true while at
    // least one SeInstance is live, false once se_view_destroy
    // released the last one (even when teardown panicked).  false on
    // artifacts predating se_instance_active.
    static bool instanceActive();
    void load(const QUrl &url);
    // One se_spin — GUI thread.  Pumps the SHARED engine, so any
    // live view can drive the loop for all of them (ENG36).
    void spinOnce();
    void goBack();
    // True reload when the artifact exports se_reload, a history-
    // pushing same-url se_load on artifacts predating ENG16; a no-op
    // until a url has committed.
    void reload();
    void setZoom(qreal zoom);
    void evaluateJavaScript(const QString &source);

    QUrl url() const { return m_url; }
    QString title() const { return m_title; }
    bool isLoading() const { return m_loading; }
    // ENG25: frames blitted from the shim since construction — the
    // smoke's scroll bench uses it to split paint cost (frames/s)
    // from input pace (px/notch) without intercepting events.
    int frameCount() const { return m_frameCount; }
    // ENG31: the rest of the scroll instrumentation — se_wheel
    // deliveries since the last reset (input-side rate) and the
    // inter-frame_ready interval histogram (paint-side pacing).
    // ENG32: largest single delivered step — the jump-size half of
    // the smoothness story (a notch now lands as an ease-out series
    // of slices instead of one hard jump).
    int wheelEventsSent() const { return m_wheelEventsSent; }
    QList<qint64> frameIntervalsMs() const { return m_frameIntervalsMs; }
    qreal largestWheelStepPx() const { return m_wheelLargestStepPx; }
    void resetScrollStats();

signals:
    void urlChanged(const QUrl &url);
    void titleChanged(const QString &title);
    void loadStarted();
    void loadProgress(int progress);
    void loadFinished(bool ok);
    void statusChanged(const QString &status);
    // ENG28: the crashed-view surface's "Try Servo again" affordance —
    // the host (EngineTab) recreates page+view on a fresh SeInstance;
    // the dead object graph is torn down first, never resurrected.
    void restartRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void timerEvent(QTimerEvent *event) override;

private:
    void grabFrame();
    void flushWheelDelta();
    void drainWheelDelta();
    void sendScrollStep(const QPointF &delta);
    QPointF wheelSliceStep(const QPointF &remaining) const;
    void setUrl(const QUrl &url);
    void setTitle(const QString &title);
    void onLoadStatus(int status);
    // ENG28: lazily create/position the poisoned view's retry button.
    void ensureCrashUi();

    // SeCallbacks trampolines — static members so the C ABI reaches
    // private state without friend pollution.
    static void cbWake(void *userdata);
    static void cbFrameReady(void *userdata);
    static void cbUrlChanged(void *userdata, const char *url);
    static void cbTitleChanged(void *userdata, const char *title);
    static void cbLoadStatus(void *userdata, int status);
    static void cbConsole(void *userdata, int level, const char *message);
    static void cbNavigation(void *userdata, const char *url);
    static void cbResourceLoad(void *userdata, const char *request);
    // ENG35: the shim's se_set_load_gate callback — consults the
    // page's profile RequestPolicy and serves a reply (deny = an
    // empty 403) instead of letting Servo fetch.  Runs on the GUI
    // thread inside se_spin.
    static int cbLoadGate(void *userdata, const SeLoadInfo *info,
                          SeHostReply *reply);

    void *m_instance;
    // ENG35: the page that owns this view — attachView sets it so
    // the load gate can resolve the OWNING profile's RequestPolicy
    // (the MPROF lesson: never an ambient manager lookup).
    QPointer<ServoPage> m_page;
    QImage m_image;
    QUrl m_url;
    QString m_title;
    bool m_loading;
    QPushButton *m_retryButton = nullptr;
    // ENG25+ENG31+ENG32: wheel deltas coalesce into one scroll
    // delivery per COMPLETED frame.  Every scroll ends in a
    // software raster + readback on this display path, so any
    // fixed-cadence forwarding outruns llvmpipe (~35ms/frame on the
    // release artifact) and queues stale deltas — the choppy half of
    // ENG31.  m_wheelInFlight gates delivery: set when a slice is
    // sent, cleared by the next frame_ready inside grabFrame, which
    // then flushes the next slice.  The timer is only a watchdog
    // for a delivered delta that produces no frame at all
    // (overscroll clamp, deduped input) — it is not the pacing
    // mechanism.
    //
    // ENG32 adds DISTANCE and SMOOTHNESS on top of that pacing:
    //  * m_notchAccel — a notch arriving within the accel window of
    //    the previous notch bumps the px multiplier by the step
    //    (capped), the system-scroll acceleration Firefox/Chrome
    //    run for rapid flicks; a pause or a dominant-axis reversal
    //    restarts the ramp.  m_lastNotchMs/Dominant track it.
    //  * m_pendingWheelDelta is now a REMAINDER — each frame
    //    delivers an ease-out slice (a fraction of what is left,
    //    floored by a finishing step and capped below a teleport)
    //    so a notch lands over ~4-6 paints like a native smooth-
    //    scroll animation instead of one hard jump.  A dominant-
    //    axis reversal cancels the in-flight remainder on the
    //    opposed axis — the view turns, it does not glide home.
    //  * m_pendingWheelHiRes marks a remainder containing raw
    //    pixelDelta (trackpad) input — those stay whole-delta:
    //    hi-res deltas are already continuous, slicing them would
    //    only add latency.
    // m_wheelEventsSent + m_frameIntervalsMs + m_wheelLargestStepPx
    // feed the smoke's events-vs-frames, frame-time and jump-size
    // instrumentation.
    QPointF m_pendingWheelDelta;
    QPointF m_pendingWheelPos;
    Qt::KeyboardModifiers m_pendingWheelMods = Qt::NoModifier;
    QBasicTimer m_wheelTimer;
    bool m_wheelInFlight = false;
    bool m_pendingWheelHiRes = false;
    qreal m_notchAccel = 1.0;
    qint64 m_lastNotchMs = -1;
    int m_lastNotchDominant = 0;
    int m_frameCount = 0;
    int m_wheelEventsSent = 0;
    qreal m_wheelLargestStepPx = 0;
    QElapsedTimer m_frameClock;
    qint64 m_lastFrameReadyMs = -1;
    QList<qint64> m_frameIntervalsMs;

    friend class ServoPage;
};

class ServoPage : public Engine::Page
{
    Q_OBJECT

public:
    explicit ServoPage(Engine::Profile *profile, QObject *parent = nullptr);

    void load(const QUrl &url) override;
    void stop() override;
    void reload() override;
    QUrl url() const override;

    bool canGoBack() const override;
    bool canGoForward() const override;
    void back() override;
    void forward() override;

    int historyCount() const override;
    int currentHistoryIndex() const override;
    QList<Engine::HistoryEntry> historyItems() const override;
    QList<Engine::HistoryEntry> backItems(int maxItems) const override;
    QList<Engine::HistoryEntry> forwardItems(int maxItems) const override;
    void goToHistoryEntry(const Engine::HistoryEntry &entry) override;

    void setZoomFactor(qreal factor) override;
    qreal zoomFactor() const override;

    void findText(const QString &subString, Engine::FindFlags options) override;

    void runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override;
    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>(),
                       quint32 worldId = 0) override;
    void toHtml(const std::function<void(const QString &)> &resultCallback) override;

    QAction *action(Engine::StandardAction action) override;

    bool isLoading() const override;
    bool recentlyAudible() const override;
    bool isOffTheRecord() const override;

    void setPageAttribute(const QString &name, bool on) override;
    void setLifecycleState(LifecycleState state) override;
    LifecycleState lifecycleState() const override;
    qint64 renderProcessId() const override;
    qint64 residentMemoryKb() const override;
    bool memoryIsShared() const override;

    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;

    void download(const QUrl &url) override;
    QWidget *view() const override;

    // The backend binds the widget the page renders into — loads
    // issued before it lands queue behind the attach.
    void attachView(ServoView *view);

    // ENG35: the profile this page belongs to — the load gate
    // resolves request policy through it (owning-profile lookup).
    Engine::Profile *profile() const { return m_profile; }

private:
    void recordNavigation(const QUrl &url);

    QPointer<ServoView> m_view;
    // Lazily-created standard actions (action()) — only the subset
    // the shim can actually perform; everything else reports nullptr
    // so chrome disables it (ENG16).
    QPointer<QAction> m_backAction;
    QPointer<QAction> m_reloadAction;
    QUrl m_pendingUrl;
    QList<Engine::HistoryEntry> m_entries;
    int m_current;
    qreal m_zoom;
    bool m_loading;
    LifecycleState m_lifecycle;
    Engine::Profile *m_profile;
    QHash<QString, Engine::Script> m_scripts;
};

class ServoProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit ServoProfile(const QString &storageName,
                          QObject *parent = nullptr);

    bool isOffTheRecord() const override;
    QString storageName() const override;
    void setUserAgent(const QString &userAgent) override;
    void setRequestPolicy(Engine::RequestPolicy *policy) override;
    Engine::RequestPolicy *requestPolicy() const override;
    void setCookieFilter(
            const std::function<bool(const Engine::CookieAttempt &)> &filter) override;
    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;
    void clear(Engine::StorageAreas areas) override;
    void setProfileAttribute(const QString &name, const QVariant &value) override;

private:
    QString m_storageName;
    Engine::RequestPolicy *m_policy = nullptr;
    QHash<QString, Engine::Script> m_scripts;
};

class ServoBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit ServoBackend(QObject *parent = nullptr);

    // The process backend, or nullptr when the servo-embed artifact
    // is absent/unresolvable — callers must treat null as "Servo is
    // not installed" and hide the choice.
    static ServoBackend *instance();
    // Filesystem+symbol resolution only; does not start servo.
    static bool artifactAvailable();
    static QString artifactPath();
    // ENG29: true when the resolved artifact is an unoptimized
    // target/debug cdylib — page loads on it are pathologically slow,
    // so the backend warns loudly rather than letting a debug Servo
    // masquerade as "the engine is just slow".
    static bool artifactIsDebug();
    // ENG28: drive the engine's graceful shutdown while the GUI
    // thread is intact — no-op when the artifact predates
    // se_shutdown (the engine then dies with the process, as it
    // always has).  Wired to aboutToQuit automatically; harnesses
    // that exit without exec() call it explicitly.
    static void shutdownEngine();

    QString id() const override;
    QString displayName() const override;
    QString engineNote() const override;
    Engine::Capabilities capabilities() const override;
    bool initialize() override;

    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override;
    Engine::Page *createPage(Engine::Profile *profile,
                             QObject *parent = nullptr) override;
    QString pageUnavailableReason() const override;
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override;

    // The servo config dir shared by every webview on a profile —
    // under the app's data path so cookie/storage state lands where
    // the rest of the profile lives.
    static QString configDir();

private:
    // RC09: one shared ServoProfile per storage name — libservo has no
    // profile objects, so these carry only the persona identity the
    // app-side facade stores (history/settings/decisions) key on.
    // Engine-side state stays process-global (configDir()); a named
    // profile never fakes per-persona engine isolation.
    QHash<QString, QPointer<ServoProfile>> m_profiles;
};

#endif // SERVOBACKEND_H
