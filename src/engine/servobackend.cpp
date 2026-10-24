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

#include "servobackend.h"

#ifdef ARKTOS_SERVO

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
#include <qset.h>
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
    // ENG36: engine/view split.  se_engine_init builds the
    // once-per-process engine (idempotent); se_view_create /
    // se_view_destroy mint and retire per-tab WebViews on it — all
    // three are mandatory now: a pre-ENG36 artifact reports "missing
    // symbols" and stays unavailable rather than reviving the
    // one-view-at-a-time shim contract.
    int (*engineInit)(void (*)(void *), void *, const char *) = nullptr;
    SeInstance (*viewCreate)(SeCallbacks, unsigned, unsigned) = nullptr;
    void (*viewDestroy)(SeInstance) = nullptr;
    void (*spin)(SeInstance) = nullptr;
    int (*load)(SeInstance, const char *) = nullptr;
    void (*resize)(SeInstance, unsigned, unsigned) = nullptr;
    void (*mouseMove)(SeInstance, float, float) = nullptr;
    void (*mouseButton)(SeInstance, int, int, float, float) = nullptr;
    void (*wheel)(SeInstance, double, double, float, float) = nullptr;
    // ENG32: optional — the compositor-direct scroll export.  A
    // missing se_scroll keeps deliveries on se_wheel, the DOM-
    // correct but script-round-tripping path older artifacts have.
    void (*scroll)(SeInstance, double, double, float, float) = nullptr;
    void (*key)(SeInstance, const char *) = nullptr;
    void (*goBack)(SeInstance) = nullptr;
    void (*setZoom)(SeInstance, float) = nullptr;
    void (*evalJs)(SeInstance, const char *) = nullptr;
    int (*frameSize)(SeInstance, unsigned *, unsigned *) = nullptr;
    long (*frameCopy)(SeInstance, unsigned char *, unsigned long) = nullptr;
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
    // ENG35: optional — the host load gate.  Missing on artifacts
    // predating se_set_load_gate: loads then stay ungated (the old
    // observable-only behavior) and capabilities() reports
    // requestInterception=false honestly.
    void (*setLoadGate)(void *, SeLoadGate) = nullptr;
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
    // ENG27/ENG36: optional — lifecycle introspection.  Missing on
    // pre-ENG27 artifacts: live-view state is then unverifiable and a
    // spent engine stays terminal.  se_instance_active reads ">=1
    // view live" on ENG36 artifacts.
    int (*instanceActive)() = nullptr;
    int (*canInit)() = nullptr;
    int (*viewCount)() = nullptr;
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
    RESOLVE(engineInit, "se_engine_init")
    RESOLVE(viewCreate, "se_view_create")
    RESOLVE(viewDestroy, "se_view_destroy")
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
    // Optional ENG35 addition — the per-load veto/stub consult.
    // Absent on older artifacts: nothing can gate loads, so request
    // policies stay advisory only and requestInterception reads false.
    api().setLoadGate = reinterpret_cast<decltype(api().setLoadGate)>(
        library().resolve("se_set_load_gate"));
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
    // Optional ENG27/ENG36 additions — live-view introspection for
    // the crash-recovery path.  Missing on older artifacts: live
    // state is then unverifiable, exactly as before.
    api().instanceActive =
        reinterpret_cast<decltype(api().instanceActive)>(
            library().resolve("se_instance_active"));
    api().canInit = reinterpret_cast<decltype(api().canInit)>(
        library().resolve("se_can_init"));
    api().viewCount = reinterpret_cast<decltype(api().viewCount)>(
        library().resolve("se_view_count"));
    // Optional ENG32 addition — compositor-direct scroll.  Missing
    // on older artifacts: sendScrollStep falls back to se_wheel and
    // scroll keeps its DOM-dispatch cost, exactly as before.
    // ENG34 diagnostic: ARKTOS_SERVO_SCROLL_PATH=wheel suppresses the
    // resolve so the smoke bench can A/B both paths on one artifact
    // (the repaint-cost measurement needs same-build numbers), and
    // doubles as a live kill-switch — the compositor path skips DOM
    // wheel dispatch, so a page whose wheel-intercepting widget
    // regresses gets the DOM-correct path back without a rebuild.
    api().scroll = qgetenv("ARKTOS_SERVO_SCROLL_PATH") == "wheel"
        ? nullptr
        : reinterpret_cast<decltype(api().scroll)>(
              library().resolve("se_scroll"));
    state = ok ? 1 : -1;
    if (!ok)
        qWarning("servo-embed: artifact at %s is missing symbols",
                 qPrintable(path));
    // The resolver is a shim-side process global — registering it at
    // resolve time covers every view this process mints.  (The
    // ENG35 load gate registers in the ServoView ctor instead —
    // cbLoadGate is a private member resolve() cannot address.)
    if (ok && api().setResourceResolver)
        api().setResourceResolver(nullptr, &servoResourceResolve);
    return ok;
}

} // namespace

// ---- N live SeInstances on one engine (ENG36) -----------------------
//
// The engine is once-per-process; a VIEW is a webview over it — and
// libservo hosts any number of those concurrently (the single-view
// limit this used to enforce was the shim's, never the engine's).
// The registry below guards callback ROUTING, not concurrency:
// every per-view callback carries the owning view's userdata, which
// must be a pointer still registered as live.  A stale callback —
// engine draining a dying view's last delegate work inside a spin —
// resolves to null and drops.
//
// What remains once-per-engine is the failure state: a failed
// se_engine_init is terminal for the session (a second build would
// consume nothing but panic the same way, and on older artifacts the
// refusal was likewise permanent).  A failed se_view_create is NOT
// terminal — the engine survives it, and the next view can retry.
//
// ENG27/ENG28 carried over per-view: se_view_destroy unregisters the
// view even when teardown panics, and instanceAvailable() consults
// se_can_init so the engine's own liveness answer wins over any
// bookkeeping of ours.

namespace {

QMutex &servoMutex()
{
    static QMutex mutex;
    return mutex;
}

QSet<ServoView *> g_views;                  // under servoMutex()
bool g_engineFailed = false;                // under servoMutex()

// userdata is the ServoView* itself (registered at view-create and
// removed at destroy) — anything not in the registry reads as dead.
ServoView *viewFor(void *userdata)
{
    ServoView *view = static_cast<ServoView *>(userdata);
    return g_views.contains(view) ? view : nullptr;
}

} // namespace

// ---- ServoView ------------------------------------------------------

void ServoView::cbWake(void *userdata)
{
    // Any Servo thread — bounce to the GUI thread.  ENG36: a wake
    // means "the shared engine has work", and se_spin on ANY live
    // view pumps the whole engine, so the queued call picks a live
    // view at DELIVERY time — queued on the application object, it
    // can never strand on a view that dies between queue and
    // delivery the way a per-view queued spinOnce could.
    Q_UNUSED(userdata);
    QObject *anchor = QCoreApplication::instance();
    if (!anchor)
        return;
    QMetaObject::invokeMethod(anchor, [] {
        ServoView *view = nullptr;
        {
            QMutexLocker lock(&servoMutex());
            // Prefer a HEALTHY view — a poisoned view's se_spin
            // no-ops under ffi_guard, so pumping through it would
            // stall the engine for every sibling.
            for (ServoView *candidate : g_views) {
                if (!candidate->isPoisoned()) {
                    view = candidate;
                    break;
                }
            }
            if (!view && !g_views.isEmpty())
                view = *g_views.begin();
        }
        if (view)
            view->spinOnce();
    }, Qt::QueuedConnection);
}

void ServoView::cbFrameReady(void *userdata)
{
    // The registry gate — a frame_ready racing se_view_destroy would
    // otherwise grabFrame/update on a view being torn down, and
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
    // and the registry gate keeps a late callback off a dying view.
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

// ENG35: servo fetch-destination string -> Engine::ResourceType.
// The adblock rule engine keys on QWebEngineUrlRequestInfo's values
// and Engine::ResourceType mirrors them for the common types; the
// tail (preload/websocket/json) has no servo destination and stays
// unmapped — anything unclassified lands on SubResource ("other").
static Engine::ResourceType servoDestType(const char *dest)
{
    const QLatin1String d(dest ? dest : "");
    if (d == QLatin1String("document"))
        return Engine::ResourceType::MainFrame;
    if (d == QLatin1String("iframe") || d == QLatin1String("frame"))
        return Engine::ResourceType::SubFrame;
    if (d == QLatin1String("script"))
        return Engine::ResourceType::Script;
    if (d == QLatin1String("image"))
        return Engine::ResourceType::Image;
    if (d == QLatin1String("style"))
        return Engine::ResourceType::Stylesheet;
    if (d == QLatin1String("font"))
        return Engine::ResourceType::FontResource;
    if (d == QLatin1String("object") || d == QLatin1String("embed"))
        return Engine::ResourceType::Object;
    if (d == QLatin1String("audio") || d == QLatin1String("video")
        || d == QLatin1String("track"))
        return Engine::ResourceType::Media;
    if (d == QLatin1String("worker") || d == QLatin1String("audioworklet")
        || d == QLatin1String("paintworklet") || d == QLatin1String("xslt"))
        return Engine::ResourceType::Worker;
    if (d == QLatin1String("sharedworker"))
        return Engine::ResourceType::SharedWorker;
    if (d == QLatin1String("serviceworker"))
        return Engine::ResourceType::ServiceWorker;
    if (d == QLatin1String("report"))
        return Engine::ResourceType::CspReport;
    if (d.isEmpty())
        // XHR/fetch()/beacon all carry an empty destination in this
        // servo pin — Xhr is the dominant member of that bucket.
        return Engine::ResourceType::Xhr;
    return Engine::ResourceType::SubResource;
}

int ServoView::cbLoadGate(void *userdata, const SeLoadInfo *info,
                            SeHostReply *reply)
{
    // GUI thread, inside se_spin — the same call context every
    // delegate callback gets, so decide() runs synchronously here.
    // ENG36: userdata is the OWNING view's (the shim delivers the
    // calling webview's callbacks.userdata), so the policy resolves
    // through the OWNING profile (MPROF): view -> page -> profile ->
    // requestPolicy — never an ambient manager singleton, so a
    // profile with no policy installed is genuinely ungated.
    QMutexLocker lock(&servoMutex());
    ServoView *view = viewFor(userdata);
    if (!view || !view->m_page || !info || !info->url)
        return 0;
    Engine::Profile *profile = view->m_page->profile();
    Engine::RequestPolicy *policy =
        profile ? profile->requestPolicy() : nullptr;
    if (!policy)
        return 0;

    Engine::NavigationRequest request;
    request.url = QUrl::fromEncoded(QByteArray(info->url));
    request.firstPartyUrl = QUrl(QString::fromUtf8(
        info->first_party_url ? info->first_party_url : ""));
    request.isMainFrame = info->is_for_main_frame != 0;
    request.resourceType = servoDestType(info->destination);

    const Engine::RequestDecision decision = policy->decide(request);
    switch (decision.action) {
    case Engine::RequestAction::Allow:
        return 0;
    case Engine::RequestAction::Block:
        // Deny: empty 403 — the request never reaches the wire.
        reply->status = 403;
        reply->mime[0] = '\0';
        reply->body = nullptr;
        reply->body_len = 0;
        return 1;
    case Engine::RequestAction::Redirect:
        // Servo's interception hook can only SERVE, never re-issue —
        // a policy redirect becomes the ready-made stub the policy
        // carried (bundled $redirect= resource or data: payload); a
        // bare URL rewrite ($removeparam) has no servo expression
        // and degrades to Allow — documented in the header comment.
        if (decision.stubBody.isEmpty())
            return 0;
        reply->status = 200;
        reply->mime[0] = '\0';
        if (!decision.stubMime.isEmpty()
            && decision.stubMime.size() < int(sizeof(reply->mime)))
            memcpy(reply->mime, decision.stubMime.constData(),
                   size_t(decision.stubMime.size()) + 1);
        // The shim copies the body synchronously during this call —
        // a function-static keeps the borrowed pointer valid that
        // long (same trick as servoResourceResolve).
        static QByteArray stub;
        stub = decision.stubBody;
        reply->body = reinterpret_cast<const unsigned char *>(stub.constData());
        reply->body_len = size_t(stub.size());
        return 1;
    }
    return 0;
}

ServoView::ServoView(QWidget *parent)
    : QWidget(parent)
    , m_instance(nullptr)
    , m_loading(false)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(tr("Servo Page"));
    m_frameClock.start();

    if (!resolve()) {
        qWarning("servo-embed: unavailable — ServoView inert");
        return;
    }
    {
        QMutexLocker lock(&servoMutex());
        if (g_engineFailed) {
            // Reached only via a caller that skipped createPage's
            // refusal — inert, never a doomed engine rebuild.
            qWarning("servo-embed: refusing view — engine init "
                     "already failed (see header)");
            return;
        }
    }

    // ENG36: the engine is built once for the process — idempotent
    // on later views.  Its waker binds to this call's wake+userdata
    // for the engine's lifetime, so the userdata is the engine-level
    // nullptr and cbWake routes to whatever view is live at delivery
    // (see cbWake): never this — a raw view pointer would dangle in
    // the waker the moment this view dies first.
    const QByteArray configDir = ServoBackend::configDir().toUtf8();
    if (api().engineInit(&ServoView::cbWake, nullptr,
                         configDir.constData()) != 0) {
        QMutexLocker lock(&servoMutex());
        // A failed engine build is terminal for the session — Opts
        // is consumed, every later attempt fails identically.
        g_engineFailed = true;
        qWarning("servo-embed: se_engine_init failed — Servo is out "
                 "for the session");
        return;
    }

    SeCallbacks callbacks = {};
    // Per-view userdata — the shim delivers it back on every
    // delegate callback (and the resolver/gate consults), so
    // viewFor can route to this exact view.  Only the engine-level
    // waker is shared, and it does not carry this pointer.
    callbacks.userdata = this;
    callbacks.wake = &ServoView::cbWake;
    callbacks.frame_ready = &ServoView::cbFrameReady;
    callbacks.url_changed = &ServoView::cbUrlChanged;
    callbacks.title_changed = &ServoView::cbTitleChanged;
    callbacks.load_status_changed = &ServoView::cbLoadStatus;
    callbacks.console_message = &ServoView::cbConsole;
    callbacks.navigation_request = &ServoView::cbNavigation;
    callbacks.resource_load = &ServoView::cbResourceLoad;

    // ENG35: the load gate is a shim-side process global — register
    // it once (the shim's OnceLock ignores repeats).  The shim
    // invokes it with the OWNING view's userdata, so per-view policy
    // resolution needs no per-view registration.  Absent on pre-
    // ENG35 artifacts: loads stay ungated there.
    if (api().setLoadGate)
        api().setLoadGate(nullptr, &ServoView::cbLoadGate);

    const int w = width() > 0 ? width() : 800;
    const int h = height() > 0 ? height() : 600;
    m_instance = api().viewCreate(callbacks, w, h);
    if (m_instance) {
        // Register before the probe spin — delegates can fire the
        // moment the loop first pumps.
        {
            QMutexLocker lock(&servoMutex());
            g_views.insert(this);
        }
        // ENG28 self-check: a fresh view counts only when its first
        // real pump stays panic-free — resize exercises the painter
        // bookkeeping, spin runs the event loop.  A poisoned probe
        // now costs only THIS view (per-view poison) — it is torn
        // down here while the engine and any sibling views survive.
        api().resize(m_instance, w, h);
        api().spin(m_instance);
        if (isPoisoned()) {
            {
                QMutexLocker lock(&servoMutex());
                g_views.remove(this);
            }
            api().viewDestroy(m_instance);
            m_instance = nullptr;
            qWarning("servo-embed: fresh view poisoned on its init "
                     "probe — view torn down, engine survives");
        }
    }
    if (!m_instance) {
        // A failed view create is NOT engine-terminal — the engine
        // survives, and the next view gets a real attempt.
        qWarning("servo-embed: se_view_create failed");
    }
}

ServoView::~ServoView()
{
    {
        QMutexLocker lock(&servoMutex());
        // Unregister first so a callback racing teardown resolves
        // dead.  ENG27: the shim's se_view_destroy unregisters its
        // own pointer even when teardown panics — only this view's
        // webview graph dies; the engine and siblings survive, and
        // the next view mints on it (per-view crash recovery).
        g_views.remove(this);
    }
    if (m_instance && resolve())
        api().viewDestroy(m_instance);
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
    // ENG36: engine-alive, not slot-free — a live view is no longer a
    // reason to refuse.  False only for a terminally failed engine
    // (our own sticky flag or the shim's answer that Opts was
    // consumed with nothing built); a missing se_can_init reads as
    // "unverifiable" and stays available, matching the artifact's
    // honest pre-ENG27 behavior.
    QMutexLocker lock(&servoMutex());
    if (g_engineFailed)
        return false;
    return !api().canInit || api().canInit();
}

bool ServoView::instanceActive()
{
    // ENG27/ENG36: the shim's live-view registry — the readback
    // proving se_view_destroy released a view even when teardown
    // panicked.  ">=1 live views" now.
    return api().instanceActive && api().instanceActive();
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

// ENG31: scroll instrumentation history depth — the smoke reads the
// whole list, so a modest cap keeps a long session from growing it.
static constexpr int kFrameIntervalHistory = 256;

void ServoView::grabFrame()
{
    // ENG31: frame_ready marks one COMPLETED paint — this is the
    // pacing point for wheel delivery, and it fires even when the
    // copy below fails, so the bookkeeping stays unconditional.
    // Record the inter-frame interval for the bench histogram,
    // release the in-flight gate, and flush whatever wheel delta
    // accumulated as ONE summed se_wheel per frame (the backlog
    // bound: input can never queue deeper than one delivery).
    const qint64 now = m_frameClock.elapsed();
    if (m_lastFrameReadyMs >= 0) {
        m_frameIntervalsMs.append(now - m_lastFrameReadyMs);
        if (m_frameIntervalsMs.size() > kFrameIntervalHistory)
            m_frameIntervalsMs.removeFirst();
    }
    m_lastFrameReadyMs = now;
    m_wheelInFlight = false;
    flushWheelDelta();
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

// ENG31: the wheel watchdog interval.  Not a pacing cadence — a
// delivered delta releases its in-flight gate on the frame_ready it
// produces (grabFrame), so the timer only covers the case where NO
// frame comes back (overscroll clamp, a deduped input).  The window
// sits well above a healthy llvmpipe frame time (~35ms release) so
// it never preempts real pacing, and bounds the stuck-gate latency
// to something imperceptible.
static constexpr int kWheelFrameGuardMs = 100;

// ENG32 — the smooth-scroll slice curve.  Each frame-paced delivery
// sends an ease-out fraction of the REMAINING delta instead of the
// whole sum: a ~145px notch lands over ~4-6 paints (~150-200ms at
// llvmpipe's ~34ms/frame) as 64/38/24/24/9px rather than one hard
// jump — the shape real smooth-scroll animations produce.  Being
// proportional, the same curve covers a huge queued burst quickly
// (big remainders take big first slices) without ever teleporting:
// the per-step cap of 1.5 viewport heights keeps a burst reading as
// fast scrolling, not a page cut.  The finishing step ends the
// asymptotic tail in one send.
static constexpr qreal kWheelSliceFraction = 0.4;
static constexpr qreal kWheelMinStepPx = 24.0;
static constexpr qreal kWheelMaxViewportFraction = 1.5;

static qreal servoWheelSliceAxis(qreal remaining, qreal viewport)
{
    if (qAbs(remaining) <= kWheelMinStepPx)
        return remaining;
    const qreal step = qBound(kWheelMinStepPx,
                              qAbs(remaining) * kWheelSliceFraction,
                              qMax(kWheelMinStepPx,
                                   viewport * kWheelMaxViewportFraction));
    return remaining < 0 ? -step : step;
}

QPointF ServoView::wheelSliceStep(const QPointF &remaining) const
{
    return QPointF(servoWheelSliceAxis(remaining.x(), width()),
                   servoWheelSliceAxis(remaining.y(), height()));
}

// ENG32 — notch acceleration (the "still slow" half).  A lone notch
// keeps the wheelScrollLines x ~53px baseline (~145-160px, Firefox
// territory); each notch inside the window of the previous one
// raises the multiplier by the step, capped — rapid flicks cover
// progressively more ground, like the system-scroll acceleration
// Chrome/Firefox apply.  A dominant-axis reversal restarts the ramp
// (and cancels the in-flight remainder on the opposed axis, handled
// in wheelEvent) so a flick back is crisp instead of mushy.
static constexpr int kWheelAccelWindowMs = 400;
static constexpr qreal kWheelAccelStep = 0.5;
static constexpr qreal kWheelAccelMax = 3.5;

static int servoWheelDominantSign(const QPointF &delta)
{
    const qreal axis =
        qAbs(delta.y()) >= qAbs(delta.x()) ? delta.y() : delta.x();
    return axis > 0 ? 1 : axis < 0 ? -1 : 0;
}

void ServoView::wheelEvent(QWheelEvent *event)
{
    if (!m_instance || isPoisoned())
        return;
    // Pace deliveries on frame COMPLETION, not a wall-clock cadence
    // (ENG31): the first slice of a burst still goes out immediately
    // (a lone notch stays low-latency), but while its frame is in
    // flight later deltas accumulate and flush once per frame_ready —
    // input rate locks to paint rate and the queue depth is bounded
    // at one delivery.
    const QPointF delta = servoWheelPixels(*event);
    if (!m_pendingWheelDelta.isNull()
            && event->modifiers() != m_pendingWheelMods)
        drainWheelDelta();  // modifier change ends the burst
    const bool highRes = !event->pixelDelta().isNull();
    QPointF scaled = delta;
    if (!highRes) {
        const qint64 now = m_frameClock.elapsed();
        const int dominant = servoWheelDominantSign(delta);
        const bool reversed = dominant != 0 && m_lastNotchDominant != 0
            && dominant != m_lastNotchDominant;
        if (reversed) {
            // Turn the wheel and the view must turn — cancel the
            // queued remainder on the opposed axis instead of
            // gliding out the old direction first.
            if (delta.y() * m_pendingWheelDelta.y() < 0)
                m_pendingWheelDelta.setY(0);
            if (delta.x() * m_pendingWheelDelta.x() < 0)
                m_pendingWheelDelta.setX(0);
        }
        const bool rapid = m_lastNotchMs >= 0
            && now - m_lastNotchMs <= kWheelAccelWindowMs;
        m_notchAccel = rapid && !reversed
            ? qMin(m_notchAccel + kWheelAccelStep, kWheelAccelMax)
            : 1.0;
        m_lastNotchMs = now;
        if (dominant != 0)
            m_lastNotchDominant = dominant;
        scaled *= m_notchAccel;
    }
    m_pendingWheelDelta += scaled;
    m_pendingWheelHiRes = m_pendingWheelHiRes || highRes;
    m_pendingWheelPos = event->position();
    m_pendingWheelMods = event->modifiers();
    if (!m_wheelInFlight)
        flushWheelDelta();
}

void ServoView::sendScrollStep(const QPointF &delta)
{
    // ENG37: ARKTOS_SERVO_SCROLL_DROP drops every delivery on the
    // floor — the --servo-smoke broken-scroll verify.  The input
    // side still paces, slices and counts sends exactly as a real
    // delivery, but scrollY can never move, so the movement legs
    // must fail on deadline expiry (never silently).
    if (qgetenv("ARKTOS_SERVO_SCROLL_DROP") == "1")
        return;
    // ENG32: prefer the compositor-direct export — se_scroll lands
    // the delta in the painter's scroll tree on the next update (a
    // webrender APZ scroll-offset transaction — no re-layout, a
    // damage-limited repaint), while se_wheel round-trips the
    // constellation + script thread for DOM wheel dispatch and its
    // preventDefault wait BEFORE the same scroll is allowed to run.
    // Per notch that is the difference between scroll bounded by the
    // raster frame alone and scroll bounded by raster PLUS script
    // latency — the chubby half of the report.  Trade-off, accepted
    // for this experimental backend and documented in the shim:
    // DOM wheel listeners do not fire and preventDefault cannot
    // veto, so wheel-intercepting widgets degrade to a normal
    // scroll.  Artifacts predating se_scroll keep the DOM path.
    if (api().scroll) {
        api().scroll(m_instance,
                     delta.x(), delta.y(),
                     m_pendingWheelPos.x(), m_pendingWheelPos.y());
        return;
    }
    api().wheel(m_instance,
                delta.x(), delta.y(),
                m_pendingWheelPos.x(), m_pendingWheelPos.y());
}

void ServoView::flushWheelDelta()
{
    if (m_pendingWheelDelta.isNull())
        return;
    if (!m_instance || isPoisoned()) {
        m_pendingWheelDelta = QPointF();
        m_pendingWheelHiRes = false;
        return;
    }
    // Hi-res remainders (trackpads, pixel wheels) deliver whole —
    // they are already continuous; slicing only delays them.
    const QPointF step = m_pendingWheelHiRes
        ? m_pendingWheelDelta
        : wheelSliceStep(m_pendingWheelDelta);
    sendScrollStep(step);
    m_pendingWheelDelta -= step;
    if (m_pendingWheelDelta.isNull())
        m_pendingWheelHiRes = false;
    ++m_wheelEventsSent;
    m_wheelLargestStepPx = qMax(m_wheelLargestStepPx,
                                qMax(qAbs(step.x()), qAbs(step.y())));
    m_wheelInFlight = true;
    // The watchdog releases the gate if this delivery never produces
    // a frame_ready; when frames ARE flowing it never gets to fire
    // ahead of the real pacing path.
    if (!m_wheelTimer.isActive())
        m_wheelTimer.start(kWheelFrameGuardMs, this);
}

void ServoView::drainWheelDelta()
{
    // Whole-remainder delivery — the burst ends here (a modifier
    // change), so there is nothing left to animate.  Same guards and
    // bookkeeping as the sliced flush.
    if (m_pendingWheelDelta.isNull())
        return;
    if (!m_instance || isPoisoned()) {
        m_pendingWheelDelta = QPointF();
        m_pendingWheelHiRes = false;
        return;
    }
    const QPointF step = m_pendingWheelDelta;
    sendScrollStep(step);
    m_pendingWheelDelta = QPointF();
    m_pendingWheelHiRes = false;
    ++m_wheelEventsSent;
    m_wheelLargestStepPx = qMax(m_wheelLargestStepPx,
                                qMax(qAbs(step.x()), qAbs(step.y())));
    m_wheelInFlight = true;
    if (!m_wheelTimer.isActive())
        m_wheelTimer.start(kWheelFrameGuardMs, this);
}

void ServoView::resetScrollStats()
{
    m_wheelEventsSent = 0;
    m_wheelLargestStepPx = 0;
    m_frameIntervalsMs.clear();
    m_lastFrameReadyMs = -1;
    // Bench runs get a deterministic accel ramp — the previous leg's
    // notches must not pre-load this one's multiplier.
    m_notchAccel = 1.0;
    m_lastNotchMs = -1;
    m_lastNotchDominant = 0;
}

void ServoView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_wheelTimer.timerId()) {
        // Watchdog — the last delivered delta produced no frame inside
        // the guard window, so its gate would otherwise latch.  Clear
        // and push the NEXT SLICE (not the whole remainder — a merely
        // slow frame must not turn the ease-out tail into a jump);
        // keep running while in flight or pending so a later delivery
        // stays covered.
        m_wheelInFlight = false;
        flushWheelDelta();
        if (!m_wheelInFlight && m_pendingWheelDelta.isNull())
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
    // ENG35: the back-pointer the load gate walks to find this
    // page's profile (owning-profile policy resolution).
    if (view)
        view->m_page = this;
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
    // No per-page attribute application on this backend — the
    // override exists so the interface's free-form keys are ignored
    // harmlessly instead of misapplied.
    Q_UNUSED(name);
    Q_UNUSED(on);
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

qint64 ServoPage::residentMemoryKb() const
{
    // ENG26: libservo renders in-process — the embed ABI exposes no
    // per-instance memory stat (the se_* surface is load/input/frame
    // + request-log only), so no per-tab figure exists.  The honest
    // answer is the shared host-process RSS — real, but covering the
    // chrome and every other tab too; memoryIsShared() is what makes
    // the widget label it instead of faking a per-tab number.
    return processMemoryKb(QCoreApplication::applicationPid());
}

bool ServoPage::memoryIsShared() const
{
    return true;
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
    // No UA override hook in the shim — accepted and ignored.
    Q_UNUSED(userAgent);
}

void ServoProfile::setRequestPolicy(Engine::RequestPolicy *policy)
{
    // ENG35: enforced — the shim's se_set_load_gate consults this
    // policy for every resource load on this profile's tabs (deny =
    // empty 403, stub = served reply).  On artifacts predating the
    // export the stored policy stays advisory-only and
    // capabilities() reports requestInterception=false.
    m_policy = policy;
}

Engine::RequestPolicy *ServoProfile::requestPolicy() const
{
    return m_policy;
}

void ServoProfile::setCookieFilter(
        const std::function<bool(const Engine::CookieAttempt &)> &filter)
{
    // The shim has no per-cookie consult — accepted and ignored
    // (capabilities().cookieFilter stays false).
    Q_UNUSED(filter);
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
    // Free-form keys land here when the apply path doesn't know the
    // backend — ignored rather than misapplied.
    Q_UNUSED(name);
    Q_UNUSED(value);
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
    const QByteArray env = qgetenv("ARKTOS_SERVO_EMBED_LIB");
    QStringList candidates;
    if (!env.isEmpty())
        candidates << QString::fromLocal8Bit(env);
    // The cdylib's name follows the host platform.
#if defined(Q_OS_MACOS)
    const QString libName = QStringLiteral("libservo_embed.dylib");
#elif defined(Q_OS_WIN)
    const QString libName = QStringLiteral("servo_embed.dll");
#else
    const QString libName = QStringLiteral("libservo_embed.so");
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << appDir + QLatin1Char('/') + libName
               << appDir + QStringLiteral("/../lib/") + libName
#if defined(Q_OS_MACOS)
               // .app bundle layouts: the binary is Contents/MacOS/,
               // the artifact ships beside it or in Frameworks/ or
               // the Contents/lib payload dir next to bundled tor.
               << appDir + QStringLiteral("/../Frameworks/") + libName
               << appDir + QStringLiteral("/../lib/") + libName
#endif
               // Dev-tree layouts: the binary lands in the repo root
               // (or src/ in shadow-less builds); the artifact sits in
               // spikes/servo/servo-embed/target/*.
               << appDir + QStringLiteral(
                    "/spikes/servo/servo-embed/target/release/") + libName
               << appDir + QStringLiteral(
                    "/spikes/servo/servo-embed/target/debug/") + libName
               << appDir + QStringLiteral(
                    "/../spikes/servo/servo-embed/target/release/") + libName
               << appDir + QStringLiteral(
                    "/../spikes/servo/servo-embed/target/debug/") + libName
               << appDir + QStringLiteral(
                    "/../../spikes/servo/servo-embed/target/release/") + libName
               << appDir + QStringLiteral(
                    "/../../spikes/servo/servo-embed/target/debug/") + libName;
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
    // ENG35: the profile RequestPolicy is enforced through the shim's
    // load gate — true only when the resolved artifact actually
    // exports se_set_load_gate.  devTools stays false: servo's
    // devtools crate speaks the Firefox remote protocol on a TCP
    // listener (prefs devtools_server_enabled / :7000), which the
    // BiDi panel's CDP client cannot read — no fake channel.
    caps.requestInterception = resolve() && api().setLoadGate;
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
        const Engine::ProfileOptions &options, QObject *)
{
    // One shared browsing context per persona NAME — servo's profile
    // objects don't exist engine-side; a per-tab profile would fake
    // isolation it can't give.  The name carries the owning persona
    // (TabWidget passes the "arktos"/"arktos-<id>" contract names) so
    // profileId() binds this profile's pages to the persona's
    // facade-bound stores — a servo visit lands in the same history.db
    // a Chromium tab on that persona writes.  An empty name keeps the
    // legacy "servo" context, which resolves the ambient persona.
    const QString name = options.storageName.isEmpty()
        ? QStringLiteral("servo") : options.storageName;
    QPointer<ServoProfile> &profile = m_profiles[name];
    if (!profile)
        profile = new ServoProfile(name, this);
    return profile;
}

Engine::Page *ServoBackend::createPage(Engine::Profile *profile,
                                       QObject *parent)
{
    // ENG36: any number of concurrent pages — libservo hosts N
    // WebViews on the shared engine, so live views no longer refuse.
    // The remaining refusal is a dead ENGINE (se_engine_init failed:
    // terminal for the session), which still fails the whole
    // EngineTab upstream (addEngineTab drops it, reloadTabInEngine
    // keeps the old tab, a servo-default newTab() falls back to
    // Chromium) instead of manufacturing a doomed tab.
    if (!ServoView::instanceAvailable()) {
        qWarning("servo: refusing engine tab — the Servo engine "
                 "failed to start");
        return nullptr;
    }
    return new ServoPage(profile, parent);
}

QString ServoBackend::pageUnavailableReason() const
{
    // ENG36: no "one tab at a time" anymore — unavailable now means
    // only that the engine itself could not start, which is
    // terminal for the session.
    if (ServoView::instanceAvailable())
        return QString();
    return tr("the engine could not be started — "
              "restart Arktos to try again");
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
    // qApp-owned: the registry lists backends without owning them,
    // so an unparented registered backend would leak the object, its
    // members and its aboutToQuit connection node at exit.
    ServoBackend *candidate = new ServoBackend(qApp);
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

#endif // ARKTOS_SERVO
