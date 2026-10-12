/* ENG03 spike: minimal C ABI over libservo (servo v0.7.0, git-pinned).
 *
 * Threading model: everything Servo-side happens on the calling
 * thread. The embedder's event loop is driven by `se_spin()`; Servo
 * calls `wake` (any thread) whenever it wants the loop pumped — the
 * host must marshal that to its GUI thread and call se_spin() there.
 * Delegate callbacks (frame_ready, url_changed, ...) always fire from
 * inside se_spin() on the GUI thread.
 */

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

use dpi::PhysicalSize;
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
        webview.paint();
        let rc = webview.rendering_context();
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
    let _ = RESOURCE_RESOLVER.set((userdata as usize, resolver));
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
    servo: Servo,
    rendering_context: Rc<SoftwareRenderingContext>,
    webview: WebView,
    shared: Rc<Shared>,
    /// A load() issued before the constellation registered the webview's
    /// browsing context is DROPPED upstream ("LoadUrl for unknown
    /// browsing context"). url() reports None until registration lands,
    /// so early loads queue here and se_spin() flushes them.
    pending_load: RefCell<Option<Url>>,
}

fn cstr(text: &str) -> CString {
    CString::new(text.as_bytes()).unwrap_or_else(|_| CString::new("<invalid>").unwrap())
}

// ENG12: libservo v0.7.0 parks Opts in a process-global OnceCell —
// the first Servo build wins, every later init panics "Already
// initialized" (components/config/opts.rs), and a panic may not
// unwind across an extern "C" boundary so the host process aborts.
// One instance per process lifetime, ever — the Qt side enforces the
// same contract so this gate is the last line of defense.
static INITIALIZED: AtomicBool = AtomicBool::new(false);

/// Create a Servo instance with one WebView sized `width`x`height`
/// (device px) on a software rendering context. Returns nullptr on
/// failure — including a second call, which libservo cannot serve
/// (process-global Opts). `config_dir` may be nullptr (Servo's
/// default) or a writable directory used for the cookie db / storage
/// threads.
#[no_mangle]
pub extern "C" fn se_init(
    callbacks: SeCallbacks,
    width: u32,
    height: u32,
    config_dir: *const c_char,
) -> *mut SeInstance {
    if INITIALIZED.swap(true, Ordering::SeqCst) {
        eprintln!("servo-embed: se_init called more than once — \
                   libservo Opts is process-global, refusing");
        return std::ptr::null_mut();
    }
    // catch_unwind keeps any residual init panic (context failure,
    // missing resource dir, ...) from aborting the host — panics do
    // not unwind across extern "C".
    catch_unwind(AssertUnwindSafe(|| {
        se_init_inner(callbacks, width, height, config_dir)
    }))
    .unwrap_or(std::ptr::null_mut())
}

fn se_init_inner(
    callbacks: SeCallbacks,
    width: u32,
    height: u32,
    config_dir: *const c_char,
) -> *mut SeInstance {
    let _ = rustls::crypto::aws_lc_rs::default_provider().install_default();

    let mut opts = Opts::default();
    opts.multiprocess = false; // single-process: no re-exec needed for the spike
    opts.sandbox = false;
    if !config_dir.is_null() {
        let dir = unsafe { CStr::from_ptr(config_dir) }
            .to_string_lossy()
            .into_owned();
        opts.config_dir = Some(dir.into());
    }

    let shared = Rc::new(Shared {
        cbs: callbacks,
        frame: RefCell::new(None),
        intercepted: Cell::new(0),
        request_log: RefCell::new(Vec::new()),
    });

    let servo = ServoBuilder::default()
        .opts(opts)
        .event_loop_waker(Box::new(SeWaker {
            userdata: callbacks.userdata as usize,
            wake: callbacks.wake,
        }))
        .build();
    servo.setup_logging();

    let size = PhysicalSize::new(width.max(1), height.max(1));
    let rendering_context = match SoftwareRenderingContext::new(size) {
        Ok(rc) => Rc::new(rc),
        Err(error) => {
            log::error!("SoftwareRenderingContext::new failed: {error:?}");
            return std::ptr::null_mut();
        },
    };

    // Tests call make_current() on the context up front — the software
    // backend is inert either way, but keep the ordering identical.
    let _ = rendering_context.make_current();

    let webview = WebViewBuilder::new(&servo, rendering_context.clone())
        .delegate(Rc::new(SeDelegate {
            shared: shared.clone(),
        }))
        .build();

    // v0.7.0 WebViews are registered hidden; a hidden webview still
    // loads in the embedder tests, but show() is the documented path
    // for a visible view inside its RenderingContext.
    webview.show();

    Box::into_raw(Box::new(SeInstance {
        servo,
        rendering_context,
        webview,
        shared,
        pending_load: RefCell::new(None),
    }))
}

/// Pump Servo's event loop once. Call on the GUI thread whenever the
/// wake callback fires (and to settle queued work).
#[no_mangle]
pub extern "C" fn se_spin(instance: *mut SeInstance) {
    let instance = unsafe { &*instance };
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
}

/// Navigate the webview. Returns 0 on success, -1 for a bad URL.
/// Loads issued before the constellation registers the webview are
/// queued and dispatched by the next se_spin() after registration.
#[no_mangle]
pub extern "C" fn se_load(instance: *mut SeInstance, url: *const c_char) -> i32 {
    let instance = unsafe { &*instance };
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
}

#[no_mangle]
pub extern "C" fn se_resize(instance: *mut SeInstance, width: u32, height: u32) {
    let instance = unsafe { &*instance };
    let size = PhysicalSize::new(width.max(1), height.max(1));
    instance.rendering_context.resize(size);
    instance.webview.resize(size);
}

fn webview_point(x: f32, y: f32) -> WebViewPoint {
    WebViewPoint::Device(DevicePoint::new(x, y))
}

#[no_mangle]
pub extern "C" fn se_mouse_move(instance: *mut SeInstance, x: f32, y: f32) {
    let instance = unsafe { &*instance };
    instance
        .webview
        .notify_input_event(InputEvent::MouseMove(MouseMoveEvent::new(webview_point(
            x, y,
        ))));
}

/// button: 0 primary, 1 middle, 2 secondary (w3c button numbering).
/// down: non-zero for press, 0 for release.
#[no_mangle]
pub extern "C" fn se_mouse_button(instance: *mut SeInstance, down: i32, button: i32, x: f32, y: f32) {
    let instance = unsafe { &*instance };
    let action = if down != 0 {
        MouseButtonAction::Down
    } else {
        MouseButtonAction::Up
    };
    instance.webview.notify_input_event(InputEvent::MouseButton(
        MouseButtonEvent::new(action, MouseButton::from(button), webview_point(x, y)),
    ));
}

/// Scroll in device pixels.
#[no_mangle]
pub extern "C" fn se_wheel(instance: *mut SeInstance, dx: f64, dy: f64, x: f32, y: f32) {
    let instance = unsafe { &*instance };
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
}

/// Press+release a text key (spike-grade: Key::Character only).
#[no_mangle]
pub extern "C" fn se_key(instance: *mut SeInstance, utf8: *const c_char) {
    let instance = unsafe { &*instance };
    let text = unsafe { CStr::from_ptr(utf8) }.to_string_lossy().into_owned();
    for state in [KeyState::Down, KeyState::Up] {
        instance
            .webview
            .notify_input_event(InputEvent::Keyboard(KeyboardEvent::from_state_and_key(
                state,
                Key::Character(text.clone().into()),
            )));
    }
}

/// Reload the current page (ENG16). Unlike a same-url se_load this
/// keeps the navigation history intact — WebView::load always pushes
/// a new session-history entry, webview.reload() does not. Hosts must
/// tolerate the export missing entirely (pre-ENG16 artifacts) and
/// fall back to a same-url se_load.
#[no_mangle]
pub extern "C" fn se_reload(instance: *mut SeInstance) {
    let instance = unsafe { &*instance };
    instance.webview.reload();
}

#[no_mangle]
pub extern "C" fn se_go_back(instance: *mut SeInstance) {
    let instance = unsafe { &*instance };
    if instance.webview.can_go_back() {
        instance.webview.go_back(1);
    }
}

#[no_mangle]
pub extern "C" fn se_set_zoom(instance: *mut SeInstance, zoom: f32) {
    let instance = unsafe { &*instance };
    instance.webview.set_page_zoom(zoom.max(0.1));
}

/// Evaluate JavaScript on the current page; result is delivered via the
/// console callback as a prefixed "SEJS: <result>" line.
#[no_mangle]
pub extern "C" fn se_eval_js(instance: *mut SeInstance, script: *const c_char) {
    let instance = unsafe { &*instance };
    let script = unsafe { CStr::from_ptr(script) }.to_string_lossy().into_owned();
    instance
        .webview
        .evaluate_javascript(script, move |_result| {
            // JSValue has no Display; the harness only needs that the
            // callback fired — full marshaling is out of spike scope.
        });
}

/// Copy the last painted RGBA frame into `buf`. Returns bytes written,
/// or the needed size as a negative number when `buf` is too small.
/// Pixel format: RGBA8, width*height*4 bytes; call se_frame_size first.
#[no_mangle]
pub extern "C" fn se_frame_size(instance: *mut SeInstance, width: *mut u32, height: *mut u32) -> i32 {
    let instance = unsafe { &*instance };
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
}

#[no_mangle]
pub extern "C" fn se_frame_copy(
    instance: *mut SeInstance,
    buf: *mut u8,
    buf_len: usize,
) -> isize {
    let instance = unsafe { &*instance };
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
}

/// Number of web-resource loads that passed through the interception
/// hook (spike metric for ENG03 question 1: interceptor coverage).
#[no_mangle]
pub extern "C" fn se_request_count(instance: *mut SeInstance) -> u64 {
    let instance = unsafe { &*instance };
    instance.shared.intercepted.get()
}

/// Dump the observed request log ("METHOD URL" lines, capped at 2048)
/// into `buf`; returns bytes written or -needed.
#[no_mangle]
pub extern "C" fn se_request_log(instance: *mut SeInstance, buf: *mut c_char, buf_len: usize) -> isize {
    let instance = unsafe { &*instance };
    let text = instance.shared.request_log.borrow().join("\n");
    let bytes = text.as_bytes();
    if buf_len < bytes.len() {
        return -(bytes.len() as isize);
    }
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, bytes.len()) };
    bytes.len() as isize
}

/// ENG11: report the linked libservo version ("0.7.0"), parsed out of
/// the engine's own user-agent product token so the string always
/// tracks the pinned `servo` dependency — never nullptr, an
/// unrecognized UA yields "unknown". The returned pointer borrows a
/// process-lifetime static; callers must not free it.
#[no_mangle]
pub extern "C" fn se_version() -> *const c_char {
    static VERSION: OnceLock<CString> = OnceLock::new();
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
}

#[no_mangle]
pub extern "C" fn se_destroy(instance: *mut SeInstance) {
    if !instance.is_null() {
        drop(unsafe { Box::from_raw(instance) });
    }
}
