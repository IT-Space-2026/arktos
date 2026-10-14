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

#include "servobackend.h"

#ifdef ARORA_SERVO

#include "browserpaths.h"
#include "engineregistry.h"

#include <qaction.h>
#include <qapplication.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qevent.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qlibrary.h>
#include <qmetaobject.h>
#include <qmimedatabase.h>
#include <qmutex.h>
#include <qpainter.h>
#include <qpushbutton.h>
#include <qstylehints.h>

#include <cstring>

#include "servo_embed.h"

// ---- dlopen'd servo-embed ABI --------------------------------------
//
// The artifact is an optional out-of-tree build, so the backend never
// links against it — every entry point resolves through QLibrary at
// first use and a missing symbol leaves the whole backend unusable
// (artifactAvailable() == false, chrome hides the option).

namespace {

struct ServoApi {
    SeInstance (*init)(SeCallbacks, unsigned, unsigned, const char *) = nullptr;
    void (*spin)(SeInstance) = nullptr;
    int (*load)(SeInstance, const char *) = nullptr;
    void (*resize)(SeInstance, unsigned, unsigned) = nullptr;
    void (*mouseMove)(SeInstance, float, float) = nullptr;
    void (*mouseButton)(SeInstance, int, int, float, float) = nullptr;
    void (*wheel)(SeInstance, double, double, float, float) = nullptr;
    void (*key)(SeInstance, const char *) = nullptr;
    void (*goBack)(SeInstance) = nullptr;
    void (*setZoom)(SeInstance, float) = nullptr;
    void (*evalJs)(SeInstance, const char *) = nullptr;
    int (*frameSize)(SeInstance, unsigned *, unsigned *) = nullptr;
    long (*frameCopy)(SeInstance, unsigned char *, unsigned long) = nullptr;
    void (*destroy)(SeInstance) = nullptr;
    // ENG11: optional — artifacts built before se_version existed must
    // stay usable, so this one resolves outside the all-or-nothing set.
    const char *(*version)() = nullptr;
    // ENG16: optional — artifacts built before se_reload existed fall
    // back to a same-url se_load (which re-fetches but pushes a
    // duplicate servo-side history entry).
    void (*reload)(SeInstance) = nullptr;
    // ENG18: optional — artifacts predating the host-resolver export
    // leave qrc:/internal-scheme loads to Servo, which cannot fetch
    // them (the reported "Error loading page" on swapped start-page
    // tabs).  A missing symbol degrades to exactly that old behavior.
    void (*setResourceResolver)(void *, SeResourceResolver) = nullptr;
    // ENG20: optional — artifacts predating panic containment cannot
    // report poisoning (they abort instead); a missing symbol reads as
    // "never observed", which is the honest answer for them.
    int (*isPoisoned)(SeInstance) = nullptr;
    // ENG23: optional — artifacts predating the software-rasterizer
    // pin cannot report the painter's GL_RENDERER; a missing symbol
    // reads as "unverifiable".
    const char *(*glRenderer)(SeInstance) = nullptr;
    // ENG28: optional — explicit engine teardown.  A missing symbol
    // leaves the engine parked until process exit (threads die with
    // it), the same outcome artifacts had before the export existed.
    void (*shutdown)() = nullptr;
    // ENG27: optional — crash-recovery introspection.  Missing on
    // pre-ENG27 artifacts: the instance slot then reads occupied while
    // a view lives (the old spend-once contract) and a spent slot
    // stays terminal.
    int (*instanceActive)() = nullptr;
    int (*canInit)() = nullptr;
};

// ENG18: serves the schemes libservo cannot fetch straight out of the
// Qt resource system.  The shim's load_web_resource delegate runs
// ahead of scheme dispatch and calls this synchronously from inside
// se_spin() — the GUI thread — for EVERY resource load; anything that
// is not a qrc: url declines and stays on Servo's own fetch path.
int servoResourceResolve(void *, const char *url, SeHostReply *reply)
{
    const QUrl qurl = QUrl::fromEncoded(QByteArray(url));
    if (qurl.scheme() != QLatin1String("qrc"))
        return 0;
    QString path = qurl.path();
    // Qt resource names never contain ".." — keep the boundary honest
    // anyway rather than trusting a page-supplied url.
    if (path.contains(QLatin1String(".."))) {
        reply->status = 403;
        reply->mime[0] = '\0';
        reply->body = nullptr;
        reply->body_len = 0;
        return 1;
    }
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));
    QFile file(QLatin1Char(':') + path);
    // The shim copies the body synchronously during this call — a
    // function-static keeps the borrowed pointer valid that long.
    static QByteArray body;
    reply->mime[0] = '\0';
    if (!file.open(QIODevice::ReadOnly)) {
        // The host owns qrc: — a missing resource answers 404 here
        // rather than falling back to Servo's own failing fetch.
        reply->status = 404;
        body.clear();
        reply->body = nullptr;
        reply->body_len = 0;
        return 1;
    }
    body = file.readAll();
    const QByteArray mime = QMimeDatabase()
        .mimeTypeForFile(path, QMimeDatabase::MatchExtension)
        .name().toUtf8();
    reply->status = 200;
    if (mime.size() < int(sizeof(reply->mime)))
        memcpy(reply->mime, mime.constData(), size_t(mime.size()) + 1);
    reply->body = reinterpret_cast<const unsigned char *>(body.constData());
    reply->body_len = size_t(body.size());
    return 1;
}

ServoApi &api()
{
    static ServoApi instance;
    return instance;
}

QLibrary &library()
{
    static QLibrary lib;
    return lib;
}

bool resolve()
{
    static int state = 0;   // 0 untried, 1 resolved, -1 failed
    if (state != 0)
        return state > 0;
    const QString path = ServoBackend::artifactPath();
    if (path.isEmpty()) {
        state = -1;
        return false;
    }
    library().setFileName(path);
    if (!library().load()) {
        qWarning("servo-embed: %s", qPrintable(library().errorString()));
        state = -1;
        return false;
    }
    // ENG29: a target/debug cdylib is pathologically slow — without
    // this line it masquerades as "the engine is just slow" (the user
    // report behind the task).  resolve()'s one-shot state makes the
    // warning once-per-process.
    if (ServoBackend::artifactIsDebug())
        qWarning("servo-embed: DEBUG artifact in use (%s) — an "
                 "unoptimized build, expect extreme slowness; the "
                 "canonical artifact is `cargo build --release` in "
                 "spikes/servo/servo-embed", qPrintable(path));
    bool ok = true;
#define RESOLVE(field, symbol) \
    api().field = reinterpret_cast<decltype(api().field)>( \
        library().resolve(symbol)); \
    ok = ok && api().field;
    RESOLVE(init, "se_init")
    RESOLVE(spin, "se_spin")
    RESOLVE(load, "se_load")
    RESOLVE(resize, "se_resize")
    RESOLVE(mouseMove, "se_mouse_move")
    RESOLVE(mouseButton, "se_mouse_button")
    RESOLVE(wheel, "se_wheel")
    RESOLVE(key, "se_key")
    RESOLVE(goBack, "se_go_back")
    RESOLVE(setZoom, "se_set_zoom")
    RESOLVE(evalJs, "se_eval_js")
    RESOLVE(frameSize, "se_frame_size")
    RESOLVE(frameCopy, "se_frame_copy")
    RESOLVE(destroy, "se_destroy")
#undef RESOLVE
    // Optional ENG11 addition — a missing se_version only degrades
    // displayName(), it must not fail artifactAvailable().
    api().version = reinterpret_cast<decltype(api().version)>(
        library().resolve("se_version"));
    // Optional ENG16 addition — a missing se_reload degrades reload to
    // a history-pushing same-url load, never to a broken backend.
    api().reload = reinterpret_cast<decltype(api().reload)>(
        library().resolve("se_reload"));
    // Optional ENG18 addition — registers the Qt-side resolver that
    // serves qrc: loads inside the shim's interception hook.  Absent
    // on older artifacts: qrc: pages then keep failing inside Servo.
    api().setResourceResolver =
        reinterpret_cast<decltype(api().setResourceResolver)>(
            library().resolve("se_set_resource_resolver"));
    // Optional ENG20 addition — panic containment.  Missing on older
    // artifacts: the best a host can do there is nothing (a panic
    // would already have aborted the process).
    api().isPoisoned = reinterpret_cast<decltype(api().isPoisoned)>(
        library().resolve("se_is_poisoned"));
    // Optional ENG23 addition — reports which rasterizer the painter's
    // context actually got so a check can verify the software pin
    // engaged rather than trust the env override took.
    api().glRenderer = reinterpret_cast<decltype(api().glRenderer)>(
        library().resolve("se_gl_renderer"));
    // Optional ENG28 addition — explicit engine shutdown for app
    // teardown.  Missing on older artifacts: the engine simply stays
    // resident until the process dies, as it always has.
    api().shutdown = reinterpret_cast<decltype(api().shutdown)>(
        library().resolve("se_shutdown"));
    // Optional ENG27 additions — instance-slot introspection for the
    // crash-recovery path.  Missing on older artifacts: the slot
    // keeps behaving as a one-way spend, exactly as before.
    api().instanceActive =
        reinterpret_cast<decltype(api().instanceActive)>(
            library().resolve("se_instance_active"));
    api().canInit = reinterpret_cast<decltype(api().canInit)>(
        library().resolve("se_can_init"));
    state = ok ? 1 : -1;
    if (!ok)
        qWarning("servo-embed: artifact at %s is missing symbols",
                 qPrintable(path));
    // The resolver is a shim-side process global — registering it at
    // resolve time covers the one SeInstance this process can host.
    if (ok && api().setResourceResolver)
        api().setResourceResolver(nullptr, &servoResourceResolve);
    return ok;
}

} // namespace

// ---- one LIVE SeInstance at a time (ENG12/ENG27/ENG28) -------------
//
// The engine is once-per-process; an INSTANCE is a webview over it.
// The slot guards concurrency, not lifetime: Free -> InUse on the
// view's se_init claim, -> Free again when the view is destroyed —
// the next se_init then manufactures a FRESH instance on the
// surviving engine (the ENG28 crash recovery).  -> Exhausted only
// when se_init itself fails: on a pre-ENG28 artifact that is also
// the answer every later attempt gets, so the failure stays
// terminal for the session instead of being retried on every swap.
//
// ENG27: the shim's se_destroy hands its instance slot back even
// when teardown panics (the webrender gl.rs panic used to leak the
// box AND strand the slot), so whether an Exhausted slot can host a
// fresh instance is the artifact's call — instanceAvailable()
// consults se_can_init.  Missing on old artifacts: the slot stays
// terminal, exactly as before.

namespace {

enum class ServoSlot { Free, InUse, Exhausted };

QMutex &servoMutex()
{
    static QMutex mutex;
    return mutex;
}

ServoSlot g_servoSlot = ServoSlot::Free;      // under servoMutex()
ServoView *g_liveView = nullptr;              // under servoMutex()

// ENG28: callback userdata is &g_liveView — a stable CELL, never the
// view itself.  The engine's event-loop waker was bound at
// ServoBuilder::build time to the FIRST instance's SeCallbacks; on a
// post-crash second instance it still wakes through that userdata,
// so only a shared indirection routes every generation's wake (and
// every delegate callback, uniformly) to the CURRENT view.  Anything
// that is not the cell reads as dead.
ServoView *viewFor(void *userdata)
{
    return userdata == &g_liveView ? g_liveView : nullptr;
}

} // namespace

// ---- ServoView ------------------------------------------------------

void ServoView::cbWake(void *userdata)
{
    // Any Servo thread — bounce to the GUI thread.  userdata is the
    // &g_liveView cell, so the live-view gate is the dereference
    // itself: a stale wake (engine shutting down, a dying view's last
    // frames) resolves to null and drops here.
    QMutexLocker lock(&servoMutex());
    ServoView *view = viewFor(userdata);
    if (!view)
        return;
    QMetaObject::invokeMethod(view, &ServoView::spinOnce,
                              Qt::QueuedConnection);
}

void ServoView::cbFrameReady(void *userdata)
{
    // Same live-view gate as cbWake — a frame_ready racing se_destroy
    // would otherwise grabFrame/update on a view being torn down, and
    // running the frame copy on the GUI thread also closes the
    // m_image write-vs-paintEvent race.
    QMutexLocker lock(&servoMutex());
    ServoView *view = viewFor(userdata);
    if (!view)
        return;
    QMetaObject::invokeMethod(view, [view]() {
        view->grabFrame();
        view->update();
    }, Qt::QueuedConnection);
}

void ServoView::cbUrlChanged(void *userdata, const char *url)
{
    // setUrl writes m_url and emits — both belong on the GUI thread,
    // and the live-view gate keeps a late callback off a dying view.
    QMutexLocker lock(&servoMutex());
    ServoView *view = viewFor(userdata);
    if (!view)
        return;
    const QUrl qurl(QString::fromUtf8(url));
    QMetaObject::invokeMethod(view, [view, qurl]() {
        view->setUrl(qurl);
    }, Qt::QueuedConnection);
}

void ServoView::cbTitleChanged(void *userdata, const char *title)
{
    QMutexLocker lock(&servoMutex());
    ServoView *view = viewFor(userdata);
    if (!view)
        return;
    const QString text(QString::fromUtf8(title));
    QMetaObject::invokeMethod(view, [view, text]() {
        view->setTitle(text);
    }, Qt::QueuedConnection);
}

void ServoView::cbLoadStatus(void *userdata, int status)
{
    QMutexLocker lock(&servoMutex());
    ServoView *view = viewFor(userdata);
    if (!view)
        return;
    QMetaObject::invokeMethod(view, [view, status]() {
        view->onLoadStatus(status);
    }, Qt::QueuedConnection);
}

void ServoView::cbConsole(void *, int level, const char *message)
{
    static const char *names[] = {"log", "debug", "info", "warn", "error"};
    qDebug("servo[%s] %s", names[qBound(0, level, 4)], message);
}

void ServoView::cbNavigation(void *, const char *url)
{
    // Observed only — the shim's delegate allows every navigation.
    // A policy callback needs a shim extension (SERVO-SPIKE.md).
    qDebug("servo navigation request: %s (allowed)", url);
}

void ServoView::cbResourceLoad(void *, const char *) {}

ServoView::ServoView(QWidget *parent)
    : QWidget(parent)
    , m_instance(nullptr)
    , m_loading(false)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(tr("Servo Page"));

    if (!resolve()) {
        qWarning("servo-embed: unavailable — ServoView inert");
        return;
    }
    {
        QMutexLocker lock(&servoMutex());
        if (g_servoSlot != ServoSlot::Free) {
            // Reached only via a caller that skipped createPage's
            // refusal — inert, never a concurrent second se_init.
            qWarning("servo-embed: refusing se_init — a live instance "
                     "already exists or init failed (see header)");
            return;
        }
        g_servoSlot = ServoSlot::InUse;
        g_liveView = this;
    }

    SeCallbacks callbacks = {};
    // &g_liveView, NOT this — the indirection cell the first-
    // generation engine waker keeps routing through (see viewFor).
    callbacks.userdata = &g_liveView;
    callbacks.wake = &ServoView::cbWake;
    callbacks.frame_ready = &ServoView::cbFrameReady;
    callbacks.url_changed = &ServoView::cbUrlChanged;
    callbacks.title_changed = &ServoView::cbTitleChanged;
    callbacks.load_status_changed = &ServoView::cbLoadStatus;
    callbacks.console_message = &ServoView::cbConsole;
    callbacks.navigation_request = &ServoView::cbNavigation;
    callbacks.resource_load = &ServoView::cbResourceLoad;

    const QByteArray configDir =
        ServoBackend::configDir().toUtf8();
    const int w = width() > 0 ? width() : 800;
    const int h = height() > 0 ? height() : 600;
    m_instance = api().init(callbacks, w, h, configDir.constData());
    if (m_instance) {
        // ENG28 self-check: a fresh instance counts only when its
        // first real pump stays panic-free — resize exercises the
        // painter bookkeeping, spin runs the event loop, and either
        // one tripping poison means the surviving engine is dead too
        // (the instance is then torn down here: the slot stays spent
        // and recovery is declared impossible, never half-alive).
        api().resize(m_instance, w, h);
        api().spin(m_instance);
        if (isPoisoned()) {
            api().destroy(m_instance);
            m_instance = nullptr;
            qWarning("servo-embed: fresh instance poisoned on its "
                     "init probe — engine unrecoverable");
        }
    }
    if (!m_instance) {
        QMutexLocker lock(&servoMutex());
        g_liveView = nullptr;
        // A failed se_init is terminal for the session — on an
        // artifact predating sequential instances every later attempt
        // fails identically; on a current one a dead engine does too.
        g_servoSlot = ServoSlot::Exhausted;
        qWarning("servo-embed: se_init failed");
    }
}

ServoView::~ServoView()
{
    {
        QMutexLocker lock(&servoMutex());
        if (g_liveView == this)
            g_liveView = nullptr;
        // ENG28: a destroyed instance hands the slot BACK — the shim
        // creates a fresh SeInstance on the surviving engine at the
        // next se_init, so recovery is possible the moment the dead
        // object graph is gone.  Only the view that held the claim
        // frees the slot — an inert never-initialized view must not
        // downgrade a live one.  ENG27: the release survives a
        // panicking teardown — the shim frees its instance slot in
        // se_destroy before the catch-wrapped drop.
        if (m_instance)
            g_servoSlot = ServoSlot::Free;
    }
    if (m_instance && resolve())
        api().destroy(m_instance);
}

bool ServoView::isReady() const
{
    return m_instance != nullptr;
}

bool ServoView::isPoisoned() const
{
    // ENG20: once the shim has caught a panic the instance is dead —
    // the C side no-ops every call, and the Qt side stops issuing them
    // (the wake->spin marshal also checks, so a poisoned view stops
    // pumping).  Pre-ENG20 artifacts lack the export; they can't be
    // asked and would have aborted instead, so read as not-poisoned.
    return m_instance && api().isPoisoned && api().isPoisoned(m_instance);
}

QString ServoView::glRenderer() const
{
    // ENG23: the rasterizer string the shim sampled at se_init —
    // "llvmpipe" on the pinned software path, empty when the artifact
    // predates se_gl_renderer (the pin is then absent anyway).
    if (!m_instance || !api().glRenderer)
        return QString();
    const char *renderer = api().glRenderer(m_instance);
    return renderer ? QString::fromUtf8(renderer) : QString();
}

bool ServoView::instanceAvailable()
{
    QMutexLocker lock(&servoMutex());
    if (g_servoSlot == ServoSlot::Free)
        return true;
    if (g_servoSlot == ServoSlot::InUse)
        return false;
    // Exhausted: the shim alone knows whether a fresh se_init would
    // take — "engine alive, slot free" once the re-init path exists,
    // "Opts consumed" on artifacts that still refuse a second init.
    // A missing export keeps the old terminal answer.
    return api().canInit && api().canInit();
}

bool ServoView::instanceActive()
{
    // ENG27: the shim's instance slot — the readback proving
    // se_destroy released it even when teardown panicked.
    return api().instanceActive && api().instanceActive();
}

bool ServoView::instanceInUse()
{
    QMutexLocker lock(&servoMutex());
    return g_servoSlot == ServoSlot::InUse;
}

void ServoView::load(const QUrl &url)
{
    if (m_instance && !isPoisoned())
        api().load(m_instance, url.toEncoded().constData());
}

void ServoView::spinOnce()
{
    if (m_instance && !isPoisoned())
        api().spin(m_instance);
}

void ServoView::goBack()
{
    if (m_instance && !isPoisoned())
        api().goBack(m_instance);
}

void ServoView::reload()
{
    // Nothing to re-fetch on a tab that never committed a url —
    // pre-registration pending loads flush through se_spin instead.
    if (!m_instance || m_url.isEmpty() || isPoisoned())
        return;
    if (api().reload) {
        api().reload(m_instance);
        return;
    }
    // Fallback for artifacts predating se_reload: a same-url se_load
    // still re-fetches and re-renders, but libservo's WebView::load
    // pushes a new session-history entry — servo's own back list
    // grows a duplicate the C++ history mirror never sees.
    api().load(m_instance, m_url.toEncoded().constData());
}

void ServoView::setZoom(qreal zoom)
{
    if (m_instance && !isPoisoned())
        api().setZoom(m_instance, static_cast<float>(zoom));
}

void ServoView::evaluateJavaScript(const QString &source)
{
    if (m_instance && !isPoisoned())
        api().evalJs(m_instance, source.toUtf8().constData());
}

void ServoView::grabFrame()
{
    if (!m_instance || isPoisoned())
        return;
    unsigned w = 0, h = 0;
    if (api().frameSize(m_instance, &w, &h) != 0 || !w || !h)
        return;
    // ENG25: reuse the backing store — a fresh QImage per frame cost a
    // multi-MB alloc on every scroll tick.  Same-size frames refill in
    // place; a resize reallocates once.
    if (m_image.width() != int(w) || m_image.height() != int(h)
            || m_image.format() != QImage::Format_RGBA8888) {
        m_image = QImage(int(w), int(h), QImage::Format_RGBA8888);
    }
    if (api().frameCopy(m_instance, m_image.bits(),
                        m_image.sizeInBytes()) < 0)
        return;
    ++m_frameCount;
}

void ServoView::setUrl(const QUrl &url)
{
    m_url = url;
    emit urlChanged(url);
}

void ServoView::setTitle(const QString &title)
{
    m_title = title;
    emit titleChanged(title);
}

void ServoView::onLoadStatus(int status)
{
    // SeCallbacks: 0 Started, 1 HeadParsed, 2 Complete — the only
    // progress signal the shim surfaces, so the bar interpolates.
    if (status == 0) {
        m_loading = true;
        emit loadStarted();
        emit loadProgress(20);
    } else if (status == 1) {
        emit loadProgress(60);
    } else if (status == 2) {
        m_loading = false;
        emit loadProgress(100);
        emit loadFinished(true);
    }
}


void ServoView::ensureCrashUi()
{
    if (m_retryButton)
        return;
    m_retryButton = new QPushButton(tr("Try Servo Again"), this);
    m_retryButton->setAccessibleName(tr("Restart the Servo engine"));
    m_retryButton->setToolTip(
        tr("Tear down the crashed engine view and create a fresh "
           "Servo instance on the still-running engine."));
    connect(m_retryButton, &QPushButton::clicked,
            this, &ServoView::restartRequested);
    m_retryButton->adjustSize();
    m_retryButton->move(12, height()
                            - m_retryButton->height() - 12);
    m_retryButton->show();
}

void ServoView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().window());
    if (!m_image.isNull())
        painter.drawImage(QPoint(0, 0), m_image);
    if (!m_instance) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect().adjusted(12, 12, -12, -12),
                         Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                         tr("Servo engine unavailable — the "
                            "servo-embed artifact could not be loaded."));
    } else if (isPoisoned()) {
        // ENG20/ENG28: the shim caught a panic inside servo — the
        // instance is dead, but the app survived.  Say so on the
        // surface and offer BOTH honest paths: "Try Servo Again"
        // tears down this view and creates a fresh SeInstance on the
        // surviving engine (restartRequested -> EngineTab), or the
        // tab can be swapped back to Chromium.
        ensureCrashUi();
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect().adjusted(12, 12, -12, -12),
                         Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                         tr("The Servo engine crashed — this tab is "
                            "disabled. Try Servo again, or switch it "
                            "back to Chromium to keep browsing."));
    }
}

void ServoView::resizeEvent(QResizeEvent *)
{
    if (m_instance && !isPoisoned())
        api().resize(m_instance, width(), height());
    // Keep the crash surface's retry button parked at the bottom —
    // a no-op while the instance is healthy.
    if (m_retryButton)
        m_retryButton->move(12, height()
                                - m_retryButton->height() - 12);
}

void ServoView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_instance && !isPoisoned())
        api().mouseMove(m_instance,
                        event->position().x(), event->position().y());
}

static int servoButton(Qt::MouseButton button)
{
    if (button == Qt::MiddleButton)
        return 1;
    if (button == Qt::RightButton)
        return 2;
    return 0;
}

void ServoView::mousePressEvent(QMouseEvent *event)
{
    if (m_instance && !isPoisoned())
        api().mouseButton(m_instance, 1, servoButton(event->button()),
                          event->position().x(), event->position().y());
}

void ServoView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_instance && !isPoisoned())
        api().mouseButton(m_instance, 0, servoButton(event->button()),
                          event->position().x(), event->position().y());
}

// ENG25: wheel delta -> device pixels.  Servo v0.7.0's painter never
// consults WheelDelta::mode on the scroll path — the delta is applied
// as raw device pixels (paint/webview_renderer.rs folds -delta into
// Scroll::Delta; mode only mirrors into the DOM event's deltaMode),
// so the embedder owns the units.  angleDelta()/8 was 15px per wheel
// notch — ~10x under the ~150-300px Chrome/Firefox produce (3 lines x
// ~53px).  Scale notches to wheelScrollLines() x kLineStepPx;
// pixelDelta (trackpads, hi-res wheels) is already device px and
// passes through untouched.
static constexpr qreal kServoWheelLineStepPx = 53.0;

static QPointF servoWheelPixels(const QWheelEvent &event)
{
    if (!event.pixelDelta().isNull())
        return QPointF(event.pixelDelta());
    const int scrollLines =
        qMax(1, QApplication::styleHints()->wheelScrollLines());
    return QPointF(event.angleDelta())
        * (scrollLines * kServoWheelLineStepPx / 120.0);
}

void ServoView::wheelEvent(QWheelEvent *event)
{
    if (!m_instance || isPoisoned())
        return;
    // Coalesce a wheel burst into one se_wheel per ~frame.  Each
    // notify_input_event ends in a full-viewport software raster +
    // readback (this shim's display contract), so forwarding raw
    // notches 1:1 outruns llvmpipe — measured ~46ms/frame at 900x600
    // in the debug artifact — and reads as molasses regardless of the
    // delta size.  The first event of a burst goes out immediately
    // (a lone notch stays low-latency); later deltas accumulate and
    // flush on a 16ms cadence.  Servo already sums same-mode pending
    // wheel deltas internally (document_event_handler.rs), so merging
    // here bounds IPC without changing scroll semantics.
    const QPointF delta = servoWheelPixels(*event);
    if (!m_pendingWheelDelta.isNull()
            && event->modifiers() != m_pendingWheelMods)
        flushWheelDelta();  // modifier change ends the burst
    m_pendingWheelDelta += delta;
    m_pendingWheelPos = event->position();
    m_pendingWheelMods = event->modifiers();
    if (!m_wheelTimer.isActive()) {
        flushWheelDelta();
        m_wheelTimer.start(16, this);
    }
}

void ServoView::flushWheelDelta()
{
    if (m_pendingWheelDelta.isNull() || !m_instance || isPoisoned())
        return;
    api().wheel(m_instance,
                m_pendingWheelDelta.x(), m_pendingWheelDelta.y(),
                m_pendingWheelPos.x(), m_pendingWheelPos.y());
    m_pendingWheelDelta = QPointF();
}

void ServoView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_wheelTimer.timerId()) {
        flushWheelDelta();
        if (m_pendingWheelDelta.isNull())
            m_wheelTimer.stop();
        return;
    }
    QWidget::timerEvent(event);
}

void ServoView::keyPressEvent(QKeyEvent *event)
{
    if (!m_instance || isPoisoned()) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (!event->text().isEmpty())
        api().key(m_instance, event->text().toUtf8().constData());
    else
        QWidget::keyPressEvent(event);
}

// ---- ServoPage ------------------------------------------------------

ServoPage::ServoPage(Engine::Profile *profile, QObject *parent)
    : Engine::Page(parent)
    , m_view(nullptr)
    , m_current(-1)
    , m_zoom(1.0)
    , m_loading(false)
    , m_lifecycle(LifecycleState::Active)
    , m_profile(profile)
{
}

void ServoPage::attachView(ServoView *view)
{
    m_view = view;
    if (!m_view)
        return;
    connect(m_view, &ServoView::urlChanged,
            this, [this](const QUrl &url) {
        recordNavigation(url);
        emit urlChanged(url);
    });
    connect(m_view, &ServoView::titleChanged,
            this, &ServoPage::titleChanged);
    connect(m_view, &ServoView::loadStarted,
            this, [this]() {
        m_loading = true;
        emit loadStarted();
    });
    connect(m_view, &ServoView::loadProgress,
            this, &ServoPage::loadProgress);
    connect(m_view, &ServoView::loadFinished,
            this, [this](bool ok) {
        m_loading = false;
        emit loadFinished(ok);
    });
    connect(m_view, &ServoView::statusChanged,
            this, [this](const QString &status) {
        emit linkHovered(status);
    });
    if (!m_pendingUrl.isEmpty()) {
        const QUrl url = m_pendingUrl;
        m_pendingUrl = QUrl();
        load(url);
    }
}

void ServoPage::load(const QUrl &url)
{
    if (!url.isValid())
        return;
    if (!m_view) {
        m_pendingUrl = url;
        return;
    }
    m_view->load(url);
}

void ServoPage::stop()
{
    // The shim exposes no stop — the navigation runs to completion.
}

void ServoPage::reload()
{
    // The view owns the reload (true se_reload when the artifact
    // exports it, same-url se_load otherwise).  Pre-attach there is
    // only the pending load, which attachView flushes once anyway.
    if (m_view)
        m_view->reload();
}

QUrl ServoPage::url() const
{
    if (m_view && !m_view->url().isEmpty())
        return m_view->url();
    return m_pendingUrl;
}

void ServoPage::recordNavigation(const QUrl &url)
{
    if (m_current >= 0 && m_current < m_entries.count()
        && m_entries.at(m_current).url == url)
        return;
    while (m_entries.count() > m_current + 1)
        m_entries.removeLast();
    Engine::HistoryEntry entry;
    entry.url = url;
    m_entries.append(entry);
    m_current = m_entries.count() - 1;
    for (int i = 0; i < m_entries.count(); ++i)
        m_entries[i].index = i;
}

bool ServoPage::canGoBack() const
{
    return m_current > 0;
}

bool ServoPage::canGoForward() const
{
    // se_go_forward does not exist in the shim.
    return false;
}

void ServoPage::back()
{
    if (canGoBack() && m_view) {
        m_view->goBack();
        --m_current;
        if (m_current >= 0)
            emit urlChanged(m_entries.at(m_current).url);
    }
}

void ServoPage::forward()
{
}

int ServoPage::historyCount() const
{
    return m_entries.count();
}

int ServoPage::currentHistoryIndex() const
{
    return m_current;
}

QList<Engine::HistoryEntry> ServoPage::historyItems() const
{
    return m_entries;
}

QList<Engine::HistoryEntry> ServoPage::backItems(int maxItems) const
{
    QList<Engine::HistoryEntry> items;
    for (int i = qMax(0, m_current - maxItems); i < m_current; ++i)
        items.append(m_entries.at(i));
    return items;
}

QList<Engine::HistoryEntry> ServoPage::forwardItems(int) const
{
    return {};
}

void ServoPage::goToHistoryEntry(const Engine::HistoryEntry &entry)
{
    if (entry.index == m_current - 1)
        back();
}

void ServoPage::setZoomFactor(qreal factor)
{
    m_zoom = factor;
    if (m_view)
        m_view->setZoom(factor);
}

qreal ServoPage::zoomFactor() const
{
    return m_zoom;
}

void ServoPage::findText(const QString &, Engine::FindFlags)
{
    // No find-in-page delegate in the shim.
    Engine::FindResult result;
    emit findTextFinished(result);
}

void ServoPage::runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback)
{
    // One-way eval: the shim reports results only via the console
    // channel (JSValue has no marshaling), so callers always get an
    // invalid variant — capabilities().scriptEvaluation stays false.
    if (m_view)
        m_view->evaluateJavaScript(source);
    if (resultCallback)
        resultCallback(QVariant());
}

void ServoPage::runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback,
                       quint32 worldId)
{
    // Servo has a single script world — the isolated-world split is
    // meaningless there, so the id is accepted and ignored.
    Q_UNUSED(worldId);
    runJavaScript(source, resultCallback);
}

void ServoPage::toHtml(const std::function<void(const QString &)> &resultCallback)
{
    if (resultCallback)
        resultCallback(QString());
}

QAction *ServoPage::action(Engine::StandardAction action)
{
    // The chrome's WebActionMapper binds menu/toolbar entries and
    // shortcuts to these — nullptr stays "disabled" per the
    // Engine::Page contract.  Only what the shim can actually do is
    // exposed: Back and Reload (no se_go_forward, no stop).
    switch (action) {
    case Engine::StandardAction::Back:
        if (!m_backAction) {
            m_backAction = new QAction(this);
            m_backAction->setEnabled(canGoBack());
            connect(m_backAction, &QAction::triggered,
                    this, &ServoPage::back);
            connect(this, &ServoPage::urlChanged,
                    m_backAction, [this] {
                m_backAction->setEnabled(canGoBack());
            });
        }
        return m_backAction;
    case Engine::StandardAction::Reload:
        if (!m_reloadAction) {
            m_reloadAction = new QAction(this);
            connect(m_reloadAction, &QAction::triggered,
                    this, &ServoPage::reload);
        }
        return m_reloadAction;
    default:
        return nullptr;
    }
}

bool ServoPage::isLoading() const
{
    return m_loading;
}

bool ServoPage::recentlyAudible() const
{
    return false;
}

bool ServoPage::isOffTheRecord() const
{
    return m_profile && m_profile->isOffTheRecord();
}

void ServoPage::setPageAttribute(const QString &name, bool on)
{
    m_attributes.insert(name, on);
}

void ServoPage::setLifecycleState(LifecycleState state)
{
    m_lifecycle = state;
}

Engine::Page::LifecycleState ServoPage::lifecycleState() const
{
    return m_lifecycle;
}

qint64 ServoPage::renderProcessId() const
{
    return -1;
}

Engine::Page *ServoPage::createWindow(Engine::WebWindowType)
{
    return nullptr;
}

void ServoPage::insertScript(const Engine::Script &script)
{
    // Recorded but never injected — the shim has no script-collection
    // API (capabilities().scriptInjection is false).
    m_scripts.insert(script.name, script);
}

void ServoPage::removeScript(const QString &name)
{
    m_scripts.remove(name);
}

QList<Engine::Script> ServoPage::scripts() const
{
    return m_scripts.values();
}

void ServoPage::download(const QUrl &)
{
    // No download delegate in the shim (capabilities().downloads).
}

QWidget *ServoPage::view() const
{
    return m_view;
}

// ---- ServoProfile ---------------------------------------------------

ServoProfile::ServoProfile(const QString &storageName, QObject *parent)
    : Engine::Profile(parent)
    , m_storageName(storageName)
{
}

bool ServoProfile::isOffTheRecord() const
{
    // Servo has no profile/off-the-record concept — the swap path
    // refuses private tabs rather than pretending one exists.
    return false;
}

QString ServoProfile::storageName() const
{
    return m_storageName;
}

void ServoProfile::setUserAgent(const QString &userAgent)
{
    // Stored only — no UA override hook in the shim.
    m_userAgent = userAgent;
}

void ServoProfile::setRequestPolicy(Engine::RequestPolicy *policy)
{
    // Stored, never enforced — servo resource loads are observable
    // but cannot be blocked through the current ABI.
    m_policy = policy;
}

Engine::RequestPolicy *ServoProfile::requestPolicy() const
{
    return m_policy;
}

void ServoProfile::setCookieFilter(
        const std::function<bool(const Engine::CookieAttempt &)> &filter)
{
    m_cookieFilter = filter;
}

void ServoProfile::insertScript(const Engine::Script &script)
{
    m_scripts.insert(script.name, script);
}

void ServoProfile::removeScript(const QString &name)
{
    m_scripts.remove(name);
}

QList<Engine::Script> ServoProfile::scripts() const
{
    return m_scripts.values();
}

void ServoProfile::clear(Engine::StorageAreas)
{
}

void ServoProfile::setProfileAttribute(const QString &name, const QVariant &value)
{
    m_attributes.insert(name, value);
}

// ---- ServoBackend ----------------------------------------------------

ServoBackend::ServoBackend(QObject *parent)
    : Engine::Backend(parent)
{
    // ENG28: the engine's lifetime is the process's — it outlives
    // every SeInstance by design.  The graceful shutdown (Servo's
    // Exit + shutdown spin) must run while the GUI thread is still
    // intact, so it is driven from aboutToQuit rather than left to
    // the shim's TLS teardown, where its own TLS may already be gone.
    if (qApp) {
        connect(qApp, &QCoreApplication::aboutToQuit,
                this, &ServoBackend::shutdownEngine);
    }
}

void ServoBackend::shutdownEngine()
{
    if (resolve() && api().shutdown)
        api().shutdown();
}

ServoBackend *ServoBackend::instance()
{
    if (!artifactAvailable())
        return nullptr;
    static ServoBackend *backend = new ServoBackend(qApp);
    return backend;
}

QString ServoBackend::artifactPath()
{
    const QByteArray env = qgetenv("ARORA_SERVO_EMBED_LIB");
    QStringList candidates;
    if (!env.isEmpty())
        candidates << QString::fromLocal8Bit(env);
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << appDir + QStringLiteral("/libservo_embed.so")
               << appDir + QStringLiteral("/../lib/libservo_embed.so")
               // Dev-tree layouts: the binary lands in the repo root
               // (or src/ in shadow-less builds); the artifact sits in
               // spikes/servo/servo-embed/target/*.
               << appDir + QStringLiteral(
                    "/spikes/servo/servo-embed/target/release/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/spikes/servo/servo-embed/target/debug/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../spikes/servo/servo-embed/target/release/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../spikes/servo/servo-embed/target/debug/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../../spikes/servo/servo-embed/target/release/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../../spikes/servo/servo-embed/target/debug/libservo_embed.so");
    for (const QString &candidate : candidates) {
        const QString clean = QDir(candidate).absolutePath();
        if (QFileInfo::exists(clean))
            return clean;
    }
    return QString();
}

bool ServoBackend::artifactIsDebug()
{
    // Path-profile probe: the artifactPath() candidate order already
    // prefers target/release at every layout depth, so landing on a
    // .../target/debug/ path means no release artifact outranked it.
    // An env-override or appdir-staged .so outside a target/<profile>
    // layout reads as not-debug — there is nothing to warn about a
    // path that does not claim a profile.
    const QString path = artifactPath();
    if (path.isEmpty())
        return false;
    QDir dir = QFileInfo(path).absoluteDir();
    return dir.dirName() == QLatin1String("debug")
        && dir.cdUp()
        && dir.dirName() == QLatin1String("target");
}

bool ServoBackend::artifactAvailable()
{
    return resolve();
}

QString ServoBackend::id() const
{
    return QStringLiteral("servo");
}

QString ServoBackend::displayName() const
{
    // ENG11: real libservo version when the artifact exports
    // se_version — same shape as the Chromium entry. The pinned
    // artifact may predate the export; honest fallback, never a faked
    // number.
    // ENG29: the debug-artifact marker rides the name so every
    // surface that shows it — indicator tooltip, settings combo, tab
    // title, --engine-list — carries the warning with zero extra
    // plumbing.
    const QString debugSuffix = artifactIsDebug()
        ? QStringLiteral(" (debug build)") : QString();
    if (resolve() && api().version) {
        if (const char *version = api().version(); version && *version)
            return QStringLiteral("Servo %1%2")
                    .arg(QString::fromUtf8(version), debugSuffix);
    }
    return QStringLiteral("Servo (version unknown)%1").arg(debugSuffix);
}

QString ServoBackend::engineNote() const
{
    // ENG29: the honest baseline next to the selector — even a
    // release Servo is slower than Chromium on real pages; a debug
    // artifact escalates the note to the extreme-slowness warning.
    if (artifactIsDebug())
        return tr("Debug servo-embed artifact — expect extreme "
                  "slowness");
    return tr("Experimental engine — page loads are slower than "
              "Chromium");
}

Engine::Capabilities ServoBackend::capabilities() const
{
    // Deliberately near-empty — the degraded-feature table the spike
    // doc grades; nothing here is faked.  downloads is true in the
    // struct's default so it is pinned off explicitly.
    Engine::Capabilities caps;
    caps.downloads = false;
    return caps;
}

bool ServoBackend::initialize()
{
    return artifactAvailable();
}

QString ServoBackend::configDir()
{
    const QString dir = BrowserPaths::dataFilePath(QLatin1String("servo"));
    QDir().mkpath(dir);
    return dir;
}

Engine::Profile *ServoBackend::createProfile(
        const Engine::ProfileOptions &, QObject *)
{
    // One shared browsing context — servo's profile objects don't
    // exist yet; a per-tab profile would fake isolation it can't give.
    if (!m_defaultProfile)
        m_defaultProfile = new ServoProfile(QStringLiteral("servo"), this);
    return m_defaultProfile;
}

Engine::Page *ServoBackend::createPage(Engine::Profile *profile,
                                       QObject *parent)
{
    // ENG12/ENG28: one LIVE WebView at a time — a second se_init while
    // an instance is alive is refused, and a failed init stays
    // terminal.  Refusing the page fails the whole EngineTab upstream
    // (addEngineTab drops it, reloadTabInEngine keeps the old tab, a
    // servo-default newTab() falls back to Chromium) instead of
    // manufacturing the crash.  A slot that freed itself when its
    // view died reaches the ctor probe normally — that IS the ENG28
    // recovery path.
    if (!ServoView::instanceAvailable()) {
        qWarning("servo: refusing engine tab — a live Servo instance "
                 "already exists or the engine failed to start");
        return nullptr;
    }
    return new ServoPage(profile, parent);
}

QString ServoBackend::pageUnavailableReason() const
{
    // ENG12/ENG28: the single live-instance slot.  InUse reads as
    // "one tab at a time" (a crashed-but-still-alive view counts —
    // the dead tab stays disabled until it is retried or closed);
    // Exhausted means se_init itself failed, which is terminal for
    // the session and worth saying differently.
    if (ServoView::instanceAvailable())
        return QString();
    if (!ServoView::instanceInUse())
        return tr("the engine could not be started — "
                  "restart Arora to try again");
    return tr("already in use — Servo supports one tab at a time");
}

QWidget *ServoBackend::createView(Engine::Page *page, QWidget *parent)
{
    ServoView *view = new ServoView(parent);
    if (auto *servoPage = qobject_cast<ServoPage *>(page))
        servoPage->attachView(view);
    return view;
}

namespace {

// ENG05: self-registration without a registry->backend symbol
// dependency — this translation unit exists only in servo=1 objects,
// yet engineregistry.o is linked into subproject binaries (tools,
// autotests) that never see it.  The probe runs on the first
// EngineRegistry::backends() call: the servo-embed artifact either
// resolves once and stays registered, or the option hides for the
// process lifetime.
void probeServoBackend()
{
    static bool probed = false;
    if (probed)
        return;
    probed = true;
    ServoBackend *candidate = new ServoBackend();
    if (candidate->initialize())
        EngineRegistry::registerBackend(candidate);
    else
        delete candidate;
}

struct ServoProbeRegistration {
    ServoProbeRegistration()
    {
        EngineRegistry::addProbe(&probeServoBackend);
    }
} servoProbeRegistration;

} // namespace

#endif // ARORA_SERVO
