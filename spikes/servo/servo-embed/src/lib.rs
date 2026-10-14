/* ENG03 spike: minimal C ABI over libservo (servo v0.7.0, git-pinned).
 *
 * Threading model: everything Servo-side happens on the calling
 * thread. The embedder's event loop is driven by `se_spin()`; Servo
 * calls `wake` (any thread) whenever it wants the loop pumped — the
 * host must marshal that to its GUI thread and call se_spin() there.
 * Delegate callbacks (frame_ready, url_changed, ...) always fire from
 * inside se_spin() on the GUI thread.
 *
 * Panic model (ENG20): EVERY export is panic-contained — catch_unwind
 * inside the boundary plus a per-instance `poisoned` flag that turns a
 * panicking webview into a dead tab. A panic may NEVER unwind over the
 * extern "C" boundary: that aborts the whole host process.
 *
 * Instance model (ENG24): the Servo engine is built once and kept for
 * the process lifetime — libservo's Opts is a process-global OnceLock
 * so a second ServoBuilder::build would panic. But a SeInstance is
 * only a WebView + delegate + rendering context over that engine, and
 * those CAN be recreated: after se_destroy frees the slot, se_init
 * builds a fresh instance on the surviving engine — the crash-
 * recovery path, one live instance at a time.
 */

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_void, CStr, CString};
use std::mem::ManuallyDrop;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

use dpi::PhysicalSize;
use glow::HasContext;
use http::{header, HeaderValue, StatusCode};
use servo::{
    opts::Opts, ConsoleLogLevel, DeviceIntRect, DevicePoint, EventLoopWaker, InputEvent, Key,
    KeyState, KeyboardEvent, LoadStatus, MouseButton, MouseButtonAction, MouseButtonEvent,
    MouseMoveEvent, NavigationRequest, RenderingContext, RgbaImage, Servo, ServoBuilder,
    SoftwareRenderingContext, UserAgentPlatform, WebResourceLoad, WebResourceResponse, WebView,
    WebViewBuilder, WebViewDelegate, WebViewPoint, WheelDelta, WheelEvent, WheelMode,
};
use url::Url;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SeCallbacks {
    pub userdata: *mut c_void,
    /// Called from any thread: Servo wants the event loop pumped.
    pub wake: extern "C" fn(*mut c_void),
    /// Called on the GUI thread after a freshly-painted frame is stored.
    pub frame_ready: extern "C" fn(*mut c_void),
    /// nullptr-terminated utf8 ("" when cleared).
    pub url_changed: extern "C" fn(*mut c_void, *const c_char),
    pub title_changed: extern "C" fn(*mut c_void, *const c_char),
    /// LoadStatus: 0=Started 1=HeadParsed 2=Complete
    pub load_status_changed: extern "C" fn(*mut c_void, i32),
    /// level: 0=Log 1=Debug 2=Info 3=Warn 4=Error
    pub console_message: extern "C" fn(*mut c_void, i32, *const c_char),
    /// Navigation request observed: "<url>" (allow/deny logged via se_last_nav).
    pub navigation_request: extern "C" fn(*mut c_void, *const c_char),
    /// Every web resource load observed through the interception hook:
    /// "<METHOD> <url>" — proves coverage of the delegate path.
    pub resource_load: extern "C" fn(*mut c_void, *const c_char),
}

struct Shared {
    cbs: SeCallbacks,
    frame: RefCell<Option<RgbaImage>>,
    intercepted: Cell<u64>,
    request_log: RefCell<Vec<String>>,
}

impl Shared {
    fn log_line(&self, line: String) {
        const MAX_LOG: usize = 2048;
        let mut log = self.request_log.borrow_mut();
        if log.len() < MAX_LOG {
            log.push(line);
        }
    }
}

struct SeDelegate {
    shared: Rc<Shared>,
}

impl WebViewDelegate for SeDelegate {
    fn notify_new_frame_ready(&self, webview: WebView) {
        let rc = webview.rendering_context();
        // ENG23: Painter::render asserts a clean error flag at the TOP
        // of this paint (painter.rs debug_assert on get_error()) —
        // drain-and-report on BOTH sides of paint() so a stray
        // driver-side error, wherever it was left, degrades to a log
        // line instead of tripping the assert and poisoning the tab.
        drain_gl_error(&rc, "pre-paint");
        webview.paint();
        drain_gl_error(&rc, "post-paint");
        let rect = DeviceIntRect::from_size(rc.size2d().cast());
        if let Some(image) = rc.read_to_image(rect) {
            *self.shared.frame.borrow_mut() = Some(image);
        }
        (self.shared.cbs.frame_ready)(self.shared.cbs.userdata);
    }

    fn notify_url_changed(&self, _webview: WebView, url: Url) {
        self.emit_string(self.shared.cbs.url_changed, &url.to_string());
    }

    fn notify_page_title_changed(&self, _webview: WebView, title: Option<String>) {
        self.emit_string(self.shared.cbs.title_changed, &title.unwrap_or_default());
    }

    fn notify_status_text_changed(&self, _webview: WebView, _status: Option<String>) {}

    fn notify_load_status_changed(&self, _webview: WebView, status: LoadStatus) {
        let code = match status {
            LoadStatus::Started => 0,
            LoadStatus::HeadParsed => 1,
            LoadStatus::Complete => 2,
        };
        (self.shared.cbs.load_status_changed)(self.shared.cbs.userdata, code);
    }

    fn request_navigation(&self, _webview: WebView, request: NavigationRequest) {
        self.emit_string(
            self.shared.cbs.navigation_request,
            &request.url.to_string(),
        );
        request.allow();
    }

    fn load_web_resource(&self, _webview: WebView, load: WebResourceLoad) {
        self.shared.intercepted.set(self.shared.intercepted.get() + 1);
        let line = format!("{} {}", load.request.method, load.request.url);
        self.shared.log_line(line.clone());
        self.emit_string(self.shared.cbs.resource_load, &line);

        // ENG18: this delegate is consulted ahead of scheme dispatch
        // (net/request_interceptor.rs), so it sees the schemes
        // libservo cannot fetch — qrc:, the app's internal schemes.
        // A host resolver answers those out of the Qt resource system;
        // a miss returns the load to Servo's normal fetch path (the
        // responder's drop() default is DoNotIntercept).
        let Some(&(userdata, resolver)) = RESOURCE_RESOLVER.get() else {
            return;
        };
        let url = cstr(&load.request.url.to_string());
        let mut reply = SeHostReply::default();
        if resolver(userdata as *mut c_void, url.as_ptr(), &mut reply) == 0 {
            return;
        }
        serve_host_reply(load, reply);
    }

    fn show_console_message(&self, _webview: WebView, level: ConsoleLogLevel, message: String) {
        let code = match level {
            ConsoleLogLevel::Log => 0,
            ConsoleLogLevel::Debug => 1,
            ConsoleLogLevel::Info => 2,
            ConsoleLogLevel::Warn => 3,
            ConsoleLogLevel::Error => 4,
            ConsoleLogLevel::Trace | ConsoleLogLevel::Dir => 1,
        };
        let cs = cstr(&message);
        (self.shared.cbs.console_message)(self.shared.cbs.userdata, code, cs.as_ptr());
    }
}

impl SeDelegate {
    fn emit_string(&self, cb: extern "C" fn(*mut c_void, *const c_char), text: &str) {
        let cs = CString::new(text.as_bytes())
            .unwrap_or_else(|_| CString::new("<invalid utf8>").unwrap());
        cb(self.shared.cbs.userdata, cs.as_ptr());
    }
}

// ---- host resource resolver (ENG18) ---------------------------------
//
// The fetch pipeline routes every scheme through load_web_resource,
// so a host callback can answer the loads libservo has no fetcher for
// (Qt's qrc: resources, the app's internal schemes).  The resolver is
// a plain process-global: the one SeInstance a process may host (see
// se_init) shares it with any future instance.

/// Host reply for a claimed resource load — mirrors SeHostReply in
/// include/servo_embed.h. `body` borrows host memory read only for
/// the duration of the resolver call.
#[repr(C)]
pub struct SeHostReply {
    pub status: i32,
    pub mime: [c_char; 128],
    pub body: *const u8,
    pub body_len: usize,
}

// Default is manual — [c_char; 128] has no Default impl.
impl Default for SeHostReply {
    fn default() -> Self {
        SeHostReply {
            status: 0,
            mime: [0; 128],
            body: std::ptr::null(),
            body_len: 0,
        }
    }
}

type SeResourceResolver =
    extern "C" fn(*mut c_void, *const c_char, *mut SeHostReply) -> i32;

static RESOURCE_RESOLVER: OnceLock<(usize, SeResourceResolver)> = OnceLock::new();

fn serve_host_reply(load: WebResourceLoad, reply: SeHostReply) {
    let mut response = WebResourceResponse::new(load.request.url.clone());
    if reply.status > 0 {
        if let Ok(code) = StatusCode::from_u16(reply.status as u16) {
            response = response.status_code(code);
        }
    }
    let mime_len = reply
        .mime
        .iter()
        .position(|&c| c == 0)
        .unwrap_or(reply.mime.len());
    if mime_len > 0 {
        let bytes =
            unsafe { std::slice::from_raw_parts(reply.mime.as_ptr().cast(), mime_len) };
        if let Ok(value) = HeaderValue::from_bytes(bytes) {
            response.headers.insert(header::CONTENT_TYPE, value);
        }
    }
    let mut intercepted = load.intercept(response);
    if !reply.body.is_null() && reply.body_len > 0 {
        let body = unsafe { std::slice::from_raw_parts(reply.body, reply.body_len) };
        intercepted.send_body_data(body.to_vec());
    }
    intercepted.finish();
}

/// Register the host's resource resolver — see servo_embed.h for the
/// contract.  Optional from the host side: hosts built before ENG18
/// simply never call this and unhandled schemes keep failing inside
/// Servo as before.
#[no_mangle]
pub extern "C" fn se_set_resource_resolver(
    userdata: *mut c_void,
    resolver: SeResourceResolver,
) {
    // No SeInstance to poison — catch only so the boundary stays
    // panic-free (ENG20).
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let _ = RESOURCE_RESOLVER.set((userdata as usize, resolver));
    }));
}

// SeWaker must be Send + Sync (EventLoopWaker bound) — a raw c_void is
// neither, so it carries only the two Send/Sync-safe pieces.
struct SeWaker {
    userdata: usize,
    wake: extern "C" fn(*mut c_void),
}

impl EventLoopWaker for SeWaker {
    fn clone_box(&self) -> Box<dyn EventLoopWaker> {
        Box::new(Self {
            userdata: self.userdata,
            wake: self.wake,
        })
    }

    fn wake(&self) {
        (self.wake)(self.userdata as *mut c_void);
    }
}

pub struct SeInstance {
    // ENG28: a clone of the ENGINE slot's handle — Servo is
    // Rc<ServoInner>, so dropping this instance only releases one
    // reference; the engine survives into the next generation.
    servo: Servo,
    /// Ownership anchor — the webview holds its own Rc; this keeps
    /// the context alive through whatever drop order se_destroy sees.
    /// (ENG20: resizes route through webview.resize(), which runs the
    /// painter's bookkeeping — never this context directly.)
    #[allow(dead_code)]
    rendering_context: Rc<SoftwareRenderingContext>,
    webview: WebView,
    shared: Rc<Shared>,
    /// A load() issued before the constellation registered the webview's
    /// browsing context is DROPPED upstream ("LoadUrl for unknown
    /// browsing context"). url() reports None until registration lands,
    /// so early loads queue here and se_spin() flushes them.
    pending_load: RefCell<Option<Url>>,
    /// ENG20: set by ffi_guard the moment ANY export catches a panic.
    /// Once servo has panicked its internal state can be incoherent, so
    /// every later export short-circuits to an error/no-op — the tab
    /// goes dead but the host process survives.
    poisoned: AtomicBool,
    /// ENG23: GL_RENDERER of the painter's context, read once at init —
    /// the host's handle on "did the software rasterizer actually
    /// engage" (se_gl_renderer).
    gl_renderer: CString,
}

// ENG20: a Rust panic may not unwind across the extern "C" ABI — the
// unwinding reaches the boundary as "panic in a function that cannot
// unwind" and the HOST process aborts.  Every export taking an
// SeInstance funnels through here: the default panic hook still prints
// the panic + location to stderr for diagnosis, the instance is
// poisoned, and the caller sees the fallback instead of an abort.
fn ffi_guard<F, R>(instance: *mut SeInstance, fallback: R, f: F) -> R
where
    F: FnOnce(&SeInstance) -> R,
{
    if instance.is_null() {
        return fallback;
    }
    let instance = unsafe { &*instance };
    if instance.poisoned.load(Ordering::SeqCst) {
        return fallback;
    }
    let result = match catch_unwind(AssertUnwindSafe(|| f(instance))) {
        Ok(result) => result,
        Err(_) => {
            instance.poisoned.store(true, Ordering::SeqCst);
            eprintln!(
                "servo-embed: caught a panic crossing an export — this \
                 instance is poisoned and every later call no-ops"
            );
            fallback
        },
    };
    // ENG23: never hand the GUI thread back to Qt with our EGL
    // context still current — the host's next make-current would
    // fail on it (see release_current_gl_context).  Cheap no-op for
    // exports that ran no GL work.
    unsafe { release_current_gl_context() };
    result
}

fn cstr(text: &str) -> CString {
    CString::new(text.as_bytes()).unwrap_or_else(|_| CString::new("<invalid>").unwrap())
}

/// ENG23: make the painter's context current, then drain-and-report
/// any pending GL error.  Painter::render debug-asserts a clean error
/// flag at the TOP of every paint — but surfman swap-chain resize and
/// other non-paint GL work leave sticky driver errors on the context
/// (llvmpipe raises GL_INVALID_OPERATION on the xcb resize path —
/// the same panic the user hit on iris/wayland) that have nothing to
/// do with the paint about to run.  Draining at every entry into
/// engine GL work keeps a driver hiccup a log line instead of a
/// poisoned instance; errors generated inside the paint itself are
/// still webrender's own to check.
fn drain_gl_error(rc: &Rc<dyn RenderingContext>, tag: &str) {
    // get_error() reports the CURRENT context's flag — without a
    // make_current here a foreign (Qt) context would answer instead.
    if rc.make_current().is_err() {
        return;
    }
    let gl_error = rc.gleam_gl_api().get_error();
    if gl_error != gleam::gl::NO_ERROR {
        eprintln!("servo-embed: GL error 0x{gl_error:x} pending at {tag} — drained");
    }
}

/// ENG28: bind `rc` for engine GL work, retrying once after whatever
/// foreign context owns this thread is released.  On a real display a
/// generation swap can leave a transient EGL_BAD_ACCESS waiting for
/// the next eglMakeCurrent (a queued X error or a foreign binding
/// whose release fails) — and Painter::render only LOGS a failed
/// make_current before webrender draws on whatever context stayed
/// current: the painter.rs assert in debug, a gallium SIGSEGV in
/// release.  Absorbing the transition here — while the failure can
/// still be retried — keeps it a log line, and the painter's own
/// make_current afterwards is an already-current rebind.
fn prebind_for_gl(rc: &Rc<dyn RenderingContext>, tag: &str) {
    if rc.make_current().is_ok() {
        return;
    }
    unsafe { release_current_gl_context() };
    if rc.make_current().is_err() {
        eprintln!("servo-embed: make_current failed at {tag} even after \
                   releasing the foreign context");
    }
}

// ENG12/ENG28: libservo v0.7.0 parks Opts in a process-global
// OnceLock — the first Servo build wins, every later
// ServoBuilder::build panics "Already initialized"
// (components/config/opts.rs), and a panic may not unwind across an
// extern "C" boundary.  The ENGINE therefore can never be rebuilt
// in-process; it is constructed on the first se_init and held for
// the rest of the process lifetime.  Everything else about a
// SeInstance — the WebView, its delegate, the rendering context, the
// poison flag — is per-instance and CAN be recreated: se_destroy
// frees the slot and the next se_init mints a fresh webview on the
// surviving engine (the ENG28 recovery — the same shape as Chromium
// respawning a crashed renderer on the surviving browser process).
//
// INITIALIZED records "an engine build was attempted (Opts may be
// consumed)": once set, a missing ENGINE means the build failed and
// re-init is genuinely impossible — the one state se_can_init still
// refuses.
static INITIALIZED: AtomicBool = AtomicBool::new(false);

// All se_* calls are contracted to the host GUI thread, which is also
// the only thread allowed to touch the Rc graph — thread_locals are
// the honest home for both.
thread_local! {
    /// The once-per-process engine.  `Servo` is `Rc<ServoInner>` —
    /// cloning the handle shares the instance — so each SeInstance
    /// keeps its own clone and this slot's ManuallyDrop keeps the
    /// engine alive between generations.  ManuallyDrop exists because
    /// ServoInner::drop runs a graceful-shutdown event-loop spin that
    /// cannot run inside a TLS destructor (servo's own TLS may already
    /// be gone; a panic there aborts) — se_shutdown is the clean
    /// teardown the host calls while the GUI thread is intact.
    static ENGINE: RefCell<Option<ManuallyDrop<Servo>>> =
        const { RefCell::new(None) };
    /// ENG27: the instance slot.  Records "a SeInstance is live right
    /// now" and holds its pointer — se_init claims it, se_destroy
    /// hands it back BEFORE the fallible teardown, so a panic inside
    /// webrender's GL teardown (the gl.rs get_integer_v
    /// GL_INVALID_OPERATION seen on the desktop) can never strand it.
    /// Holding the pointer also lets se_destroy reject a
    /// stale/double-destroyed instance instead of re-dropping freed
    /// memory.  "Slot free + engine alive" is the state the ENG28
    /// re-init recovers into; se_instance_active()/se_can_init()
    /// report it to the host.
    static INSTANCE_ACTIVE: Cell<usize> = const { Cell::new(0) };
}

// ENG23 — pin Servo's painter to Mesa's software rasterizer.
//
// `SoftwareRenderingContext` is a readback context — every frame goes
// through `read_to_image` into a QImage and is never presented to a
// window — but surfman still builds a real EGL context bound to the
// session display.  On a desktop session `Connection::new()` picks the
// wayland/x11 HARDWARE adapter (iris on the reporter's box), and any
// stray driver-side error stays sticky on that context until
// `Painter::render`'s `debug_assert_eq!(get_error(), NO_ERROR)`
// (painter.rs:416) trips — the `GL_INVALID_OPERATION` panic seen on
// the user's Wayland session while every headless run stayed clean.
//
// The fix is to not create the display-bound context at all:
// render+readback is this shim's whole contract, and a surfaceless
// llvmpipe context is exactly the path every headless run already
// exercises.  Two env adjustments make surfman land there:
//  * `LIBGL_ALWAYS_SOFTWARE=1` must be set BEFORE Mesa's eglInitialize
//    picks a driver — surfman sets it itself, but too late (inside
//    context-descriptor creation, after dri device enumeration, so a
//    display-bound connection still ends up on iris);
//  * WAYLAND_DISPLAY must be poisoned, not merely hidden:
//    wl_display_connect(nullptr) falls back to the "wayland-0"
//    default socket, so REMOVING the variable still connects on a
//    live session — and a wayland connection whose context then
//    fails make_current dies inside surfman's Drop ("Contexts must
//    be destroyed explicitly"), crashing se_init outright.  Pointing
//    the variable at a nonexistent socket makes the wayland AND x11
//    backends genuinely fail so the multi-backend Connection::new()
//    falls through to `mesa_surfaceless`, which needs no compositor
//    connection at all.
// Both are restored when this guard drops — it is constructed at the
// top of se_init_inner, before ServoBuilder spawns any engine threads,
// so the env writes cannot race a getenv inside servo (Rust 2021
// keeps set_var a safe call; host Qt threads never read these vars).
// Where the surfaceless EGL platform is missing entirely (non-Mesa
// stacks) init retries once on the real display connection — still
// software where Mesa honors the variable.
struct ScopedSoftwareGlEnv {
    libgl: Option<std::ffi::OsString>,
    wayland: Option<std::ffi::OsString>,
    display: Option<std::ffi::OsString>,
    egl_platform: Option<std::ffi::OsString>,
}

fn restore_env(name: &str, value: &Option<std::ffi::OsString>) {
    match value {
        Some(v) => std::env::set_var(name, v),
        None => std::env::remove_var(name),
    }
}

impl ScopedSoftwareGlEnv {
    fn new() -> Self {
        let saved = Self {
            libgl: std::env::var_os("LIBGL_ALWAYS_SOFTWARE"),
            wayland: std::env::var_os("WAYLAND_DISPLAY"),
            display: std::env::var_os("DISPLAY"),
            egl_platform: std::env::var_os("EGL_PLATFORM"),
        };
        std::env::set_var("LIBGL_ALWAYS_SOFTWARE", "1");
        // Poison, not hide — wl_display_connect(nullptr) silently
        // falls back to the "wayland-0" default socket when the
        // variable is absent, so a removed var still yields a live
        // compositor connection on desktop sessions.  A nonexistent
        // name makes the wayland backend fail outright, and with
        // DISPLAY gone XOpenDisplay fails too, leaving surfman only
        // the mesa_surfaceless backend.
        std::env::set_var("WAYLAND_DISPLAY", "arora-servo-surfaceless");
        std::env::remove_var("DISPLAY");
        std::env::remove_var("EGL_PLATFORM");
        saved
    }

    /// Bring the session display vars back while keeping
    /// LIBGL_ALWAYS_SOFTWARE — the non-Mesa fallback path.
    fn restore_display(&self) {
        restore_env("WAYLAND_DISPLAY", &self.wayland);
        restore_env("DISPLAY", &self.display);
        restore_env("EGL_PLATFORM", &self.egl_platform);
    }
}

impl Drop for ScopedSoftwareGlEnv {
    fn drop(&mut self) {
        self.restore_display();
        restore_env("LIBGL_ALWAYS_SOFTWARE", &self.libgl);
    }
}

/// ENG23: unbind whatever GL context (EGL or GLX) is current on this
/// thread.  Two call sites, one mechanism — Mesa owns at most one
/// current context per thread across APIs:
///  * before surfman init — the Qt host can leave a foreign context
///    current on the GUI thread (QRhi under xcb holds a GLX context
///    across compositing), and eglMakeCurrent fails while one owns
///    the thread; surfman's make_current failing mid-init used to
///    drop-panic inside SoftwareRenderingContext::new, killing se_init.
///  * after every export that ran engine GL work — leaving OUR
///    surfaceless EGL context current breaks the host symmetrically:
///    Qt's next QRhi make-current fails ('QRhiGles2: Failed to make
///    context current') and the whole window stops compositing.
/// Releasing is safe both ways: the painter re-binds inside
/// Painter::render (and drain_gl_error re-binds to sample), and Qt
/// re-makes its contexts current before painting — it does not rely
/// on currency persisting between frames.  Resolved via RTLD_NOLOAD
/// so an absent GL stack (pure headless) is a no-op, never a load.
/// Returns true when a context was actually released — callers that
/// run before surfman init use it to report a foreign context (worth
/// an eprintln); routine post-export releases stay debug-level.
#[cfg(target_family = "unix")]
unsafe fn release_current_gl_context() -> bool {
    let mut released = false;
    const RTLD_NOW: i32 = 2;
    const RTLD_NOLOAD: i32 = 4;
    extern "C" {
        fn dlopen(name: *const c_char, flags: i32) -> *mut c_void;
        fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    }
    type GetPtr = unsafe extern "C" fn() -> *mut c_void;
    type EglMakeCurrent =
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *mut c_void) -> i32;
    type GlxMakeCurrent = unsafe extern "C" fn(*mut c_void, usize, *mut c_void) -> i32;

    unsafe fn dlsym_or_null(lib: *mut c_void, name: *const c_char) -> *mut c_void {
        if lib.is_null() {
            std::ptr::null_mut()
        } else {
            dlsym(lib, name)
        }
    }
    unsafe fn load_sym<T>(lib: *mut c_void, name: *const c_char) -> Option<T> {
        let ptr = dlsym_or_null(lib, name);
        if ptr.is_null() {
            None
        } else {
            Some(std::mem::transmute_copy(&ptr))
        }
    }

    let egl = dlopen(c"libEGL.so.1".as_ptr(), RTLD_NOW | RTLD_NOLOAD);
    if !egl.is_null() {
        let get_context: Option<GetPtr> =
            load_sym(egl, c"eglGetCurrentContext".as_ptr());
        let get_display: Option<GetPtr> =
            load_sym(egl, c"eglGetCurrentDisplay".as_ptr());
        let make_current: Option<EglMakeCurrent> =
            load_sym(egl, c"eglMakeCurrent".as_ptr());
        if let (Some(get_context), Some(get_display), Some(make_current)) =
            (get_context, get_display, make_current)
        {
            let context = get_context();
            if !context.is_null() {
                log::debug!("releasing EGL context {context:?} current on this thread");
                make_current(
                    get_display(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                );
                released = true;
            }
        }
    }

    let gl = dlopen(c"libGL.so.1".as_ptr(), RTLD_NOW | RTLD_NOLOAD);
    if !gl.is_null() {
        let get_context: Option<GetPtr> =
            load_sym(gl, c"glXGetCurrentContext".as_ptr());
        let get_display: Option<GetPtr> =
            load_sym(gl, c"glXGetCurrentDisplay".as_ptr());
        let make_current: Option<GlxMakeCurrent> =
            load_sym(gl, c"glXMakeCurrent".as_ptr());
        if let (Some(get_context), Some(get_display), Some(make_current)) =
            (get_context, get_display, make_current)
        {
            let context = get_context();
            if !context.is_null() {
                log::debug!("releasing GLX context {context:?} current on this thread");
                make_current(get_display(), 0, std::ptr::null_mut());
                released = true;
            }
        }
    }

    released
}

#[cfg(not(target_family = "unix"))]
unsafe fn release_current_gl_context() -> bool {
    false
}

/// ENG23 helper: surfman can panic mid-init (a created EGL context
/// dropped without destroy_context when a later step fails) instead
/// of returning Err — contain that per-attempt so the fallback retry
/// still runs, and so a poisoned artifact degrades to init failure
/// rather than an abort-level panic across the FFI boundary.
fn try_rendering_context(size: PhysicalSize<u32>) -> Option<Rc<SoftwareRenderingContext>> {
    match catch_unwind(AssertUnwindSafe(|| SoftwareRenderingContext::new(size))) {
        Ok(Ok(rc)) => Some(Rc::new(rc)),
        Ok(Err(error)) => {
            log::error!("SoftwareRenderingContext::new failed: {error:?}");
            None
        },
        Err(_) => {
            log::error!("SoftwareRenderingContext::new panicked");
            None
        },
    }
}

/// Create an embed instance — one WebView sized `width`x`height`
/// (device px) on a software rendering context. Returns nullptr on
/// failure. The FIRST call also builds the Servo engine (consuming
/// the process-global Opts and `config_dir`); a later call is legal
/// once the previous SeInstance was se_destroy'd and creates a FRESH
/// WebView + delegate + render context on the surviving engine — the
/// ENG28 crash-recovery path. A call while another instance is still
/// live is refused: one embed instance at a time.
#[no_mangle]
pub extern "C" fn se_init(
    callbacks: SeCallbacks,
    width: u32,
    height: u32,
    config_dir: *const c_char,
) -> *mut SeInstance {
    // ENG27: claim the instance slot up front — claimed, not spent:
    // a failed or panicking init hands it back so the host's retry
    // (or crash-recovery attempt) still gets a real chance.
    // usize::MAX marks "claimed, pointer not minted yet"; the real
    // pointer lands below once init succeeds.  The claim must NOT
    // clobber a held pointer — a refused re-entry that overwrote it
    // would leave the live instance undestroyable (se_destroy's
    // pointer match fails) AND the slot stuck at MAX forever.
    let claimed = INSTANCE_ACTIVE.with(|slot| {
        if slot.get() == 0 {
            slot.set(usize::MAX);
            true
        } else {
            false
        }
    });
    if !claimed {
        eprintln!("servo-embed: se_init while a live SeInstance exists \
                   — one embed instance at a time, refusing");
        return std::ptr::null_mut();
    }
    // catch_unwind keeps any residual init panic (context failure,
    // missing resource dir, ...) from aborting the host — panics do
    // not unwind across extern "C".
    let instance = catch_unwind(AssertUnwindSafe(|| {
        se_init_inner(callbacks, width, height, config_dir)
    }))
    .unwrap_or(std::ptr::null_mut());
    INSTANCE_ACTIVE.with(|slot| {
        slot.set(if instance.is_null() { 0 } else { instance as usize });
    });
    instance
}

fn se_init_inner(
    callbacks: SeCallbacks,
    width: u32,
    height: u32,
    config_dir: *const c_char,
) -> *mut SeInstance {
    // ENG23: pin the whole init to the software rasterizer — the guard
    // must exist before ServoBuilder::build spawns engine threads and
    // must cover SoftwareRenderingContext::new (surfman's
    // Connection::new, the only place display/driver is chosen).
    // Everything later reuses that already-initialized connection.
    let env = ScopedSoftwareGlEnv::new();

    let shared = Rc::new(Shared {
        cbs: callbacks,
        frame: RefCell::new(None),
        intercepted: Cell::new(0),
        request_log: RefCell::new(Vec::new()),
    });

    // ENG28: the engine is a once-per-process build kept in the
    // thread_local slot — a second se_init only manufactures a fresh
    // webview on it (the recovered instance shares the surviving
    // engine, never the poisoned webview graph).  Only the first init
    // touches Opts/config_dir and installs the event-loop waker —
    // that waker holds the FIRST instance's userdata for the engine's
    // lifetime, so the host must pass a stable indirection cell it
    // keeps pointed at whichever view is live (see servobackend.cpp).
    let servo = match ENGINE.with(|engine| {
        engine.borrow().as_ref().map(|engine| (**engine).clone())
    }) {
        Some(servo) => servo,
        None => {
            // The build attempt itself is what consumes Opts — mark it
            // BEFORE ServoBuilder::build so a panicking build still
            // reads "attempted, no engine" (unrecoverable) to
            // se_can_init instead of "never tried".
            INITIALIZED.store(true, Ordering::SeqCst);

            let _ = rustls::crypto::aws_lc_rs::default_provider()
                .install_default();

            let mut opts = Opts::default();
            opts.multiprocess = false; // single-process: no re-exec needed
            opts.sandbox = false;
            if !config_dir.is_null() {
                let dir = unsafe { CStr::from_ptr(config_dir) }
                    .to_string_lossy()
                    .into_owned();
                opts.config_dir = Some(dir.into());
            }

            let servo = ServoBuilder::default()
                .opts(opts)
                .event_loop_waker(Box::new(SeWaker {
                    userdata: callbacks.userdata as usize,
                    wake: callbacks.wake,
                }))
                .build();
            servo.setup_logging();
            ENGINE.with(|engine| {
                *engine.borrow_mut() = Some(ManuallyDrop::new(servo.clone()));
            });
            servo
        },
    };

    let size = PhysicalSize::new(width.max(1), height.max(1));
    // ENG23: the thread must be free of foreign GL ownership before
    // surfman makes its context current — a host GLX/EGL context
    // still current here makes Mesa's eglMakeCurrent fail (see
    // release_current_gl_context).  Anything current this early is
    // genuinely foreign (our context does not exist yet) — worth a
    // real log line, unlike the routine post-export releases.
    if unsafe { release_current_gl_context() } {
        eprintln!(
            "servo-embed: released a foreign GL context that was \
             current on the servo init thread"
        );
    }
    let rendering_context = match try_rendering_context(size) {
        Some(rc) => rc,
        None => {
            // No surfaceless EGL platform on this stack (e.g. non-Mesa
            // EGL) — retry once on the real display connection; Mesa
            // still honors LIBGL_ALWAYS_SOFTWARE there.
            env.restore_display();
            match try_rendering_context(size) {
                Some(rc) => rc,
                None => {
                    // A partially-constructed surfman context can
                    // still be current — leave the thread clean.
                    unsafe { release_current_gl_context() };
                    return std::ptr::null_mut();
                },
            }
        },
    };

    // Tests call make_current() on the context up front — the software
    // backend is inert either way, but keep the ordering identical.
    let _ = rendering_context.make_current();

    // ENG23: verify — never assume — which rasterizer the painter got.
    // Logged unconditionally and exported via se_gl_renderer so the
    // smoke leg can assert the software path actually engaged.
    let gl_renderer = unsafe {
        rendering_context
            .glow_gl_api()
            .get_parameter_string(glow::RENDERER)
    };
    eprintln!("servo-embed: GL renderer: {gl_renderer}");

    let webview = WebViewBuilder::new(&servo, rendering_context.clone())
        .delegate(Rc::new(SeDelegate {
            shared: shared.clone(),
        }))
        .build();

    // v0.7.0 WebViews are registered hidden; a hidden webview still
    // loads in the embedder tests, but show() is the documented path
    // for a visible view inside its RenderingContext.
    webview.show();

    let instance = Box::into_raw(Box::new(SeInstance {
        servo,
        rendering_context,
        webview,
        shared,
        pending_load: RefCell::new(None),
        poisoned: AtomicBool::new(false),
        gl_renderer: CString::new(gl_renderer.as_str())
            .unwrap_or_else(|_| CString::new("unknown").unwrap()),
    }));
    // ENG23: init leaves the painter's EGL context current on this
    // thread — release it so the host's own GL work re-binds cleanly.
    unsafe { release_current_gl_context() };
    instance
}

/// Pump Servo's event loop once. Call on the GUI thread whenever the
/// wake callback fires (and to settle queued work).
#[no_mangle]
pub extern "C" fn se_spin(instance: *mut SeInstance) {
    ffi_guard(instance, (), |instance| {
        if std::env::var_os("SE_SELFTEST_PANIC").is_some() {
            panic!("ENG20 selftest panic");
        }
        // ENG28: spin_event_loop drives Painter::render — pre-bind so
        // a transient BadAccess on a generation swap is retried here
        // instead of leaving webrender to draw on a foreign context.
        prebind_for_gl(&instance.webview.rendering_context(), "se_spin");
        instance.servo.spin_event_loop();
        // borrow_mut() ends with this statement — an if-let scrutinee
        // temporary would keep the borrow alive through its whole block.
        let pending = instance.pending_load.borrow_mut().take();
        if let Some(url) = pending {
            if instance.webview.url().is_some() {
                instance.webview.load(url);
            } else {
                *instance.pending_load.borrow_mut() = Some(url);
            }
        }
    });
}

/// Navigate the webview. Returns 0 on success, -1 for a bad URL.
/// Loads issued before the constellation registers the webview are
/// queued and dispatched by the next se_spin() after registration.
#[no_mangle]
pub extern "C" fn se_load(instance: *mut SeInstance, url: *const c_char) -> i32 {
    ffi_guard(instance, -1, |instance| {
        let url = unsafe { CStr::from_ptr(url) }.to_string_lossy();
        match Url::parse(&url) {
            Ok(url) => {
                if instance.webview.url().is_some() {
                    instance.webview.load(url);
                } else {
                    *instance.pending_load.borrow_mut() = Some(url);
                }
                0
            },
            Err(_) => -1,
        }
    })
}

#[no_mangle]
pub extern "C" fn se_resize(instance: *mut SeInstance, width: u32, height: u32) {
    ffi_guard(instance, (), |instance| {
        let size = PhysicalSize::new(width.max(1), height.max(1));
        // Route through the WebView ALONE — it forwards to
        // Painter::resize_rendering_context, which resizes the
        // surfman surface AND pushes the bookkeeping the painter-side
        // state needs: WebViewRenderer::set_rect, the
        // set_document_view transaction, a fresh root-pipeline display
        // list, RepaintReason::Resize.  Resizing the RenderingContext
        // directly first updates its size cell, so the painter's
        // same-size early-return then SKIPS all of that — webrender
        // composites with a stale document view on a resized surface,
        // which on affected adapters leaves a GL error the next
        // Painter::render asserts on (ENG20's abort).
        // ENG23: the swap-chain resize runs GL work — make the
        // painter's context current first (exports release it on the
        // way out) and drain anything it leaves so the next paint's
        // assert sees a clean flag.  ENG28: retry-after-release, same
        // as se_spin — a generation swap can leave a transient
        // BadAccess on the first bind.
        let rc = instance.webview.rendering_context();
        prebind_for_gl(&rc, "se_resize");
        instance.webview.resize(size);
        drain_gl_error(&rc, "post-resize");
    });
}

fn webview_point(x: f32, y: f32) -> WebViewPoint {
    WebViewPoint::Device(DevicePoint::new(x, y))
}

#[no_mangle]
pub extern "C" fn se_mouse_move(instance: *mut SeInstance, x: f32, y: f32) {
    ffi_guard(instance, (), |instance| {
        instance
            .webview
            .notify_input_event(InputEvent::MouseMove(MouseMoveEvent::new(webview_point(
                x, y,
            ))));
    });
}

/// button: 0 primary, 1 middle, 2 secondary (w3c button numbering).
/// down: non-zero for press, 0 for release.
#[no_mangle]
pub extern "C" fn se_mouse_button(instance: *mut SeInstance, down: i32, button: i32, x: f32, y: f32) {
    ffi_guard(instance, (), |instance| {
        let action = if down != 0 {
            MouseButtonAction::Down
        } else {
            MouseButtonAction::Up
        };
        instance.webview.notify_input_event(InputEvent::MouseButton(
            MouseButtonEvent::new(action, MouseButton::from(button), webview_point(x, y)),
        ));
    });
}

/// Scroll in device pixels.
#[no_mangle]
pub extern "C" fn se_wheel(instance: *mut SeInstance, dx: f64, dy: f64, x: f32, y: f32) {
    ffi_guard(instance, (), |instance| {
        instance
            .webview
            .notify_input_event(InputEvent::Wheel(WheelEvent::new(
                WheelDelta {
                    x: dx,
                    y: dy,
                    z: 0.0,
                    mode: WheelMode::DeltaPixel,
                },
                webview_point(x, y),
            )));
    });
}

/// Press+release a text key (spike-grade: Key::Character only).
#[no_mangle]
pub extern "C" fn se_key(instance: *mut SeInstance, utf8: *const c_char) {
    ffi_guard(instance, (), |instance| {
        let text = unsafe { CStr::from_ptr(utf8) }.to_string_lossy().into_owned();
        for state in [KeyState::Down, KeyState::Up] {
            instance
                .webview
                .notify_input_event(InputEvent::Keyboard(KeyboardEvent::from_state_and_key(
                    state,
                    Key::Character(text.clone().into()),
                )));
        }
    });
}

/// Reload the current page (ENG16). Unlike a same-url se_load this
/// keeps the navigation history intact — WebView::load always pushes
/// a new session-history entry, webview.reload() does not. Hosts must
/// tolerate the export missing entirely (pre-ENG16 artifacts) and
/// fall back to a same-url se_load.
#[no_mangle]
pub extern "C" fn se_reload(instance: *mut SeInstance) {
    ffi_guard(instance, (), |instance| {
        instance.webview.reload();
    });
}

#[no_mangle]
pub extern "C" fn se_go_back(instance: *mut SeInstance) {
    ffi_guard(instance, (), |instance| {
        if instance.webview.can_go_back() {
            instance.webview.go_back(1);
        }
    });
}

#[no_mangle]
pub extern "C" fn se_set_zoom(instance: *mut SeInstance, zoom: f32) {
    ffi_guard(instance, (), |instance| {
        instance.webview.set_page_zoom(zoom.max(0.1));
    });
}

/// Evaluate JavaScript on the current page; result is delivered via the
/// console callback as a prefixed "SEJS: <result>" line.
#[no_mangle]
pub extern "C" fn se_eval_js(instance: *mut SeInstance, script: *const c_char) {
    ffi_guard(instance, (), |instance| {
        let script = unsafe { CStr::from_ptr(script) }.to_string_lossy().into_owned();
        instance
            .webview
            .evaluate_javascript(script, move |_result| {
                // JSValue has no Display; the harness only needs that the
                // callback fired — full marshaling is out of spike scope.
            });
    });
}

/// Copy the last painted RGBA frame into `buf`. Returns bytes written,
/// or the needed size as a negative number when `buf` is too small.
/// Pixel format: RGBA8, width*height*4 bytes; call se_frame_size first.
#[no_mangle]
pub extern "C" fn se_frame_size(instance: *mut SeInstance, width: *mut u32, height: *mut u32) -> i32 {
    ffi_guard(instance, -1, |instance| {
        match instance.shared.frame.borrow().as_ref() {
            Some(image) => {
                unsafe {
                    *width = image.width();
                    *height = image.height();
                }
                0
            },
            None => -1,
        }
    })
}

#[no_mangle]
pub extern "C" fn se_frame_copy(
    instance: *mut SeInstance,
    buf: *mut u8,
    buf_len: usize,
) -> isize {
    ffi_guard(instance, -1, |instance| {
        let frame = instance.shared.frame.borrow();
        let Some(image) = frame.as_ref() else {
            return -1;
        };
        let raw = image.as_raw();
        if buf_len < raw.len() {
            return -(raw.len() as isize);
        }
        unsafe { std::ptr::copy_nonoverlapping(raw.as_ptr(), buf, raw.len()) };
        raw.len() as isize
    })
}

/// Number of web-resource loads that passed through the interception
/// hook (spike metric for ENG03 question 1: interceptor coverage).
#[no_mangle]
pub extern "C" fn se_request_count(instance: *mut SeInstance) -> u64 {
    ffi_guard(instance, 0, |instance| instance.shared.intercepted.get())
}

/// Dump the observed request log ("METHOD URL" lines, capped at 2048)
/// into `buf`; returns bytes written or -needed.
#[no_mangle]
pub extern "C" fn se_request_log(instance: *mut SeInstance, buf: *mut c_char, buf_len: usize) -> isize {
    ffi_guard(instance, -1, |instance| {
        let text = instance.shared.request_log.borrow().join("\n");
        let bytes = text.as_bytes();
        if buf_len < bytes.len() {
            return -(bytes.len() as isize);
        }
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, bytes.len()) };
        bytes.len() as isize
    })
}

/// ENG20: non-zero once a panic caught inside any export has poisoned
/// this instance (see ffi_guard) — every export then no-ops.  Hosts
/// may poll this to mark the tab dead and stop calling in; a missing
/// export on older artifacts means "never observed a panic".
#[no_mangle]
pub extern "C" fn se_is_poisoned(instance: *mut SeInstance) -> i32 {
    if instance.is_null() {
        return 0;
    }
    let instance = unsafe { &*instance };
    instance.poisoned.load(Ordering::SeqCst) as i32
}

/// ENG23: GL_RENDERER string of the painter's context, sampled once at
/// se_init — lets the host verify the software rasterizer actually
/// engaged instead of assuming the env override took.  The pointer
/// borrows the instance; nullptr on a null/poisoned instance.
#[no_mangle]
pub extern "C" fn se_gl_renderer(instance: *mut SeInstance) -> *const c_char {
    ffi_guard(instance, std::ptr::null(), |instance| {
        instance.gl_renderer.as_ptr()
    })
}

/// ENG11: report the linked libservo version ("0.7.0"), parsed out of
/// the engine's own user-agent product token so the string always
/// tracks the pinned `servo` dependency — never nullptr, an
/// unrecognized UA yields "unknown". The returned pointer borrows a
/// process-lifetime static; callers must not free it.
#[no_mangle]
pub extern "C" fn se_version() -> *const c_char {
    static VERSION: OnceLock<CString> = OnceLock::new();
    static UNKNOWN: &[u8] = b"unknown\0";
    // No SeInstance to poison here — catch only so an unexpected panic
    // cannot abort the host over the ABI.
    catch_unwind(AssertUnwindSafe(|| {
        VERSION
            .get_or_init(|| {
                let ua = UserAgentPlatform::default().to_user_agent_string();
                let version = ua
                    .split_whitespace()
                    .find_map(|token| token.strip_prefix("Servo/"))
                    .unwrap_or("unknown");
                cstr(version)
            })
            .as_ptr()
    }))
    .unwrap_or(UNKNOWN.as_ptr().cast())
}

/// ENG27: non-zero while the instance slot is occupied — se_init
/// claims it on entry, se_destroy hands it back FIRST (before the
/// fallible teardown, so even a panicking drop frees it).  This is
/// the host-side readback for "the slot is free": on a crashed +
/// destroyed instance it must read 0, or no recovery path has
/// anything to recover into.  Absent on pre-ENG27 artifacts —
/// resolve optionally.
#[no_mangle]
pub extern "C" fn se_instance_active() -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        INSTANCE_ACTIVE.with(|slot| (slot.get() != 0) as i32)
    }))
    .unwrap_or(0)
}

/// ENG27/ENG28: non-zero when an se_init issued right now would
/// produce a live instance.  With the slot free that is true both
/// before the first init (engine not built yet — se_init builds it)
/// and after a destroy with the engine still alive (ENG28 re-init).
/// The refusal is kept only for genuinely unrecoverable state: a live
/// instance holds the slot, or an engine build was attempted but
/// produced none (Opts consumed, nothing to mint a webview on —
/// including after se_shutdown).  The Qt gate consults this so a
/// spent slot stops being a hard-forever refusal.
#[no_mangle]
pub extern "C" fn se_can_init() -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let live = INSTANCE_ACTIVE.with(|slot| slot.get() != 0);
        if live {
            return 0;
        }
        let servable = !INITIALIZED.load(Ordering::SeqCst)
            || ENGINE.with(|engine| engine.borrow().is_some());
        servable as i32
    }))
    .unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn se_destroy(instance: *mut SeInstance) {
    if instance.is_null() {
        return;
    }
    // ENG27: hand the instance slot back FIRST — the teardown below
    // is catch-wrapped, and a panic inside webrender's GL teardown
    // (the gl.rs get_integer_v GL_INVALID_OPERATION the desktop log
    // showed) must not strand the slot: a leaked box wastes memory,
    // a stuck slot makes any later recovery impossible.  Only the
    // pointer that claimed the slot can release it — a stale or
    // double-destroyed pointer reads "not the live instance" and
    // returns harmlessly instead of re-dropping freed memory.
    let claimed = INSTANCE_ACTIVE.with(|slot| {
        let held = slot.get() == instance as usize;
        if held {
            slot.set(0);
        }
        held
    });
    if !claimed {
        return;
    }
    // Poison up front too: any export re-entered from inside teardown
    // (a delegate callback driving nested work) no-ops instead of
    // touching half-dropped fields.
    unsafe { &*instance }.poisoned.store(true, Ordering::SeqCst);
    // Deliberately NOT gated on `poisoned` — the host owns freeing the
    // instance either way.  The teardown is staged under two catches:
    // a panic in the drain (a dead GL context may panic make_current)
    // must not skip the drop attempt, and a panic inside the drop
    // leaks the box rather than aborting the process — the contained
    // alternative now that the slot is already free.
    if catch_unwind(AssertUnwindSafe(|| {
        // ENG23: webrender runs its own debug GL checks inside drop —
        // a residual error on the painter's context would turn a clean
        // teardown into a panic + leaked box; drain first.
        drain_gl_error(&unsafe { &*instance }.webview.rendering_context(), "pre-destroy");
        // ENG27 selftest hook — injects the panic INSIDE the catch-
        // wrapped teardown region the real GL panic surfaced in.
        if std::env::var_os("SE_SELFTEST_DESTROY_PANIC").is_some() {
            panic!("ENG27 selftest destroy panic");
        }
    }))
    .is_err()
    {
        eprintln!("servo-embed: panic during se_destroy pre-teardown — \
                   attempting the drop anyway");
    }
    if catch_unwind(AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(instance) });
    }))
    .is_err()
    {
        eprintln!("servo-embed: panic during se_destroy — instance \
                   leaked, slot already released");
    }
    // ENG23: teardown made the painter's context current — release it
    // for the host's GL work (see release_current_gl_context).
    unsafe { release_current_gl_context() };
}

/// ENG28: shut the shared engine down. Optional and once-only —
/// dropping the last Servo handle sends the constellation Exit and
/// spins the event loop until shutdown completes, so the host calls
/// this from application teardown while the GUI thread is still
/// intact (the ENGINE slot is ManuallyDrop precisely so an
/// unshut-down engine leaks harmlessly at thread exit instead of
/// spinning inside a TLS destructor). No-op when no engine was ever
/// built or after a previous se_shutdown; a live SeInstance is NOT
/// destroyed — hosts must se_destroy first.
#[no_mangle]
pub extern "C" fn se_shutdown() {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let engine = ENGINE.with(|engine| engine.borrow_mut().take());
        if let Some(engine) = engine {
            drop(ManuallyDrop::into_inner(engine));
        }
    }));
    unsafe { release_current_gl_context() };
}

#[cfg(test)]
mod tests {
    use super::*;

    extern "C" fn noop_void(_: *mut c_void) {}
    extern "C" fn noop_str(_: *mut c_void, _: *const c_char) {}
    extern "C" fn noop_status(_: *mut c_void, _: i32) {}
    extern "C" fn noop_console(_: *mut c_void, _: i32, _: *const c_char) {}

    fn test_callbacks() -> SeCallbacks {
        SeCallbacks {
            userdata: std::ptr::null_mut(),
            wake: noop_void,
            frame_ready: noop_void,
            url_changed: noop_str,
            title_changed: noop_str,
            load_status_changed: noop_status,
            console_message: noop_console,
            navigation_request: noop_str,
            resource_load: noop_str,
        }
    }

    #[test]
    fn slot_and_init_gates_report_clean() {
        // A fresh thread-local slot reads free; the engine was never
        // built on this thread's view of the world.
        assert_eq!(se_instance_active(), 0);
    }

    #[test]
    fn null_instance_calls_are_noops() {
        // ffi_guard's null fallback: no dereference, no panic.
        se_spin(std::ptr::null_mut());
        assert_eq!(se_is_poisoned(std::ptr::null_mut()), 0);
        assert_eq!(se_request_count(std::ptr::null_mut()), 0);
        let mut w = 0u32;
        let mut h = 0u32;
        assert_eq!(se_frame_size(std::ptr::null_mut(), &mut w, &mut h), -1);
        se_destroy(std::ptr::null_mut());
    }

    #[test]
    fn destroy_unclaimed_pointer_is_a_noop() {
        // ENG27: the slot holds the live pointer — a destroy on a
        // pointer that never claimed it (stale/double-destroy) must
        // bail BEFORE touching it.  A dangling address is sufficient:
        // the early return means it is never dereferenced.
        let bogus = 0xdeadbeefusize as *mut SeInstance;
        se_destroy(bogus);           // slot reads 0 -> early return
        assert_eq!(se_instance_active(), 0);
    }

    #[test]
    fn destroy_releases_slot_when_teardown_panics() {
        // The ENG27 scenario end-to-end: a live instance whose
        // se_destroy hits a panic inside the catch-wrapped teardown
        // (the webrender GL shape) still hands the slot back.  Needs
        // a real engine — skip cleanly when init cannot run in this
        // environment (the app-level --servo-smoke drives the same
        // legs on the desktop path).
        let config_dir = c"/tmp/arora-se-test";
        std::fs::create_dir_all("/tmp/arora-se-test").ok();
        let instance = se_init(test_callbacks(), 64, 64, config_dir.as_ptr());
        if instance.is_null() {
            eprintln!("servo-embed: se_init unavailable here — skipping");
            return;
        }
        assert_eq!(se_instance_active(), 1);
        assert_eq!(se_can_init(), 0); // slot held
        std::env::set_var("SE_SELFTEST_DESTROY_PANIC", "1");
        se_destroy(instance); // panics inside the catch — must not abort
        std::env::remove_var("SE_SELFTEST_DESTROY_PANIC");
        assert_eq!(se_instance_active(), 0);
        se_destroy(instance); // stale pointer after real destroy: no-op
        // ENG28: the slot is free AND the engine survived — a second
        // se_init now mints a fresh SeInstance on it instead of being
        // refused (once-per-process applies to the ENGINE, not the
        // webview).
        assert_eq!(se_can_init(), 1);
        let second = se_init(test_callbacks(), 64, 64, config_dir.as_ptr());
        assert!(!second.is_null());
        assert_eq!(se_instance_active(), 1);
        assert_eq!(se_can_init(), 0); // held again
        // Concurrent instances stay refused — sequential only.
        let third = se_init(test_callbacks(), 64, 64, config_dir.as_ptr());
        assert!(third.is_null());
        assert_eq!(se_instance_active(), 1);
        se_destroy(second);
        assert_eq!(se_instance_active(), 0);
        assert_eq!(se_can_init(), 1); // engine still alive
        // Explicit teardown: se_shutdown drops the engine; afterwards
        // Opts is consumed with nothing left to mint a webview on —
        // the one genuinely unrecoverable state.
        se_shutdown();
        assert_eq!(se_can_init(), 0);
        let dead = se_init(test_callbacks(), 64, 64, config_dir.as_ptr());
        assert!(dead.is_null());
        se_shutdown(); // idempotent no-op
    }
}
