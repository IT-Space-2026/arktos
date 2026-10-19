/* ENG03 spike: C ABI between the Qt harness and libservo_embed.
 * All functions except the wake callback run on the host GUI thread.
 * The wake callback itself may fire on ANY Servo thread — marshal it
 * to the GUI thread (e.g. QMetaObject::invokeMethod Qt::QueuedConnection)
 * and call se_spin() there.
 */
#ifndef SERVO_EMBED_H
#define SERVO_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *SeInstance;

typedef struct SeCallbacks {
    void *userdata;
    void (*wake)(void *userdata);                 /* any thread */
    void (*frame_ready)(void *userdata);          /* GUI thread */
    void (*url_changed)(void *userdata, const char *url);
    void (*title_changed)(void *userdata, const char *title);
    void (*load_status_changed)(void *userdata, int status); /* 0 Started 1 HeadParsed 2 Complete */
    void (*console_message)(void *userdata, int level, const char *message);
    void (*navigation_request)(void *userdata, const char *url);
    void (*resource_load)(void *userdata, const char *method_and_url);
} SeCallbacks;

/* ENG36 — engine/view split.  The once-per-process engine is built by
 * se_engine_init (idempotent — the engine consumes libservo's
 * process-global Opts and config_dir on the first call only); views
 * are minted and retired independently by se_view_create /
 * se_view_destroy: each gets its own WebView, delegate, software
 * rendering context, callbacks and poison flag, and ANY NUMBER may be
 * live at once (libservo maps WebViewId->WebView on the one Servo —
 * the old single-view limit was this shim's, never the engine's).
 * se_view_destroy tears down only its view; the engine and every
 * other live view survive, and a later se_view_create recovers into
 * the same engine (the ENG28 crash-recovery path, now per-view).
 *
 * se_engine_init returns 0 on success or when the engine already
 * exists, -1 when a previous build attempt failed terminally.  Its
 * wake fn + wake_userdata bind the engine's event-loop waker for the
 * process lifetime (re-installation is impossible), so wake_userdata
 * MUST be a stable engine-level indirection — never a per-view
 * pointer that can die.  A wake means "the shared engine has work":
 * se_spin through ANY live view pumps the whole loop, so routing the
 * wake to any surviving view is sufficient.  A view's own
 * callbacks.userdata is per-view: every delegate callback (and the
 * ENG18 resolver / ENG35 gate consult) is invoked with the OWNING
 * view's userdata, so the host can route callbacks and per-view
 * policy without a second indirection. */
int se_engine_init(void (*wake)(void *userdata), void *wake_userdata,
                   const char *config_dir);
SeInstance se_view_create(SeCallbacks callbacks, unsigned width,
                          unsigned height);
void se_view_destroy(SeInstance instance);
/* ENG36: number of live views — registered by se_view_create,
 * removed by se_view_destroy.  Absent on pre-ENG36 artifacts. */
int se_view_count(void);

/* Legacy one-call entry point (pre-ENG36 hosts): se_engine_init with
 * this view's wake+userdata — on the FIRST call that pair binds the
 * engine's waker, so the host MUST pass a stable indirection cell it
 * keeps pointed at a live view — followed by se_view_create.
 * config_dir is honored on the first engine-init call only. */
SeInstance se_init(SeCallbacks callbacks, unsigned width, unsigned height,
                   const char *config_dir);
void se_spin(SeInstance instance);
int se_load(SeInstance instance, const char *url);
void se_resize(SeInstance instance, unsigned width, unsigned height);
void se_mouse_move(SeInstance instance, float x, float y);
void se_mouse_button(SeInstance instance, int down, int button, float x, float y);
void se_wheel(SeInstance instance, double dx, double dy, float x, float y);
/* ENG32: scroll the area under (x,y) by the same delta convention as
 * se_wheel (wheel motion — positive dy scrolls up) but through the
 * compositor-direct path: the delta lands in the painter's scroll
 * tree on the next update (webrender APZ transaction, no re-layout,
 * damage-limited repaint) instead of round-tripping the script
 * thread for DOM wheel dispatch + preventDefault.  Trade-off: DOM
 * wheel listeners do not fire and preventDefault cannot veto —
 * se_wheel stays the DOM-correct path.  Absent on artifacts
 * predating ENG32 — resolve optionally, fall back to se_wheel. */
void se_scroll(SeInstance instance, double dx, double dy, float x, float y);
void se_key(SeInstance instance, const char *utf8);
void se_go_back(SeInstance instance);
/* ENG16: reload the current page without pushing a history entry.
 * Absent in artifacts predating ENG16 — resolve optionally and fall
 * back to a same-url se_load. */
void se_reload(SeInstance instance);
void se_set_zoom(SeInstance instance, float zoom);
void se_eval_js(SeInstance instance, const char *script);
int se_frame_size(SeInstance instance, unsigned *width, unsigned *height);
long se_frame_copy(SeInstance instance, unsigned char *buf, unsigned long buf_len);
/* ENG11: linked libservo version string ("0.7.0") — process-lifetime
 * static, do not free. Never NULL; "unknown" if unparseable. */
const char *se_version(void);

unsigned long se_request_count(SeInstance instance);
long se_request_log(SeInstance instance, char *buf, unsigned long buf_len);

/* ENG18: host resource resolver. The fetch pipeline consults the
 * webview's load_web_resource delegate for EVERY scheme (the request
 * interceptor runs ahead of scheme dispatch), so a host-registered
 * resolver can answer the schemes libservo cannot fetch — qrc:, the
 * app-internal schemes — without forking Servo.
 *
 * Registered once per process (before or after the first view).
 * Called synchronously from inside se_spin() on the GUI thread for
 * every resource load — invoked with the OWNING view's
 * callbacks.userdata since ENG36 (the registration-time userdata is
 * kept for signature compat only); the host
 * returns non-zero to claim the request and fills `reply`:
 *   status    HTTP status code (0 is treated as 200)
 *   mime      utf8 Content-Type value; "" sends no header
 *   body      borrowed pointer, read only during the call
 *   body_len  byte count (0 = empty body is legal)
 * Return 0 to leave the load on Servo's normal fetch path. */
typedef struct SeHostReply {
    int status;
    char mime[128];
    const unsigned char *body;
    size_t body_len;
} SeHostReply;

typedef int (*SeResourceResolver)(void *userdata, const char *url,
                                  SeHostReply *reply);

void se_set_resource_resolver(void *userdata, SeResourceResolver resolver);

/* ENG35: host load gate — consulted inside the same
 * load_web_resource hook BEFORE the resource resolver, so the host
 * can veto or stub a load Servo would otherwise fetch (adblock-style
 * request blocking).  The observational resource_load callback is
 * untouched: it still reports every load, gated or not.
 *
 * Registered once per process like the resolver.  Called
 * synchronously from inside se_spin() on the GUI thread for EVERY
 * resource load — invoked with the OWNING view's callbacks.userdata
 * since ENG36 (the registration-time userdata is kept for signature
 * compat only), so the host's policy resolves the right tab's
 * profile; the host inspects `info` and returns non-zero to
 * SERVE `reply` verbatim — the request never reaches Servo's fetch
 * path (a deny is reply{status:403, empty body}; a stub is any
 * 200+mime+body pair, the same contract as the resolver).  Return 0
 * to pass the load on to the resolver / normal fetch.  There is no
 * redirect verdict: Servo's interception hook only ever serves a
 * response — a policy wanting to rewrite a URL must carry the
 * replacement payload in `reply` or let the request pass. */
typedef struct SeLoadInfo {
    const char *url;              /* request URL, utf8 */
    /* first-party document URL: the resolved referrer when Servo
     * knows it, else the webview's committed URL for subresources;
     * the request URL itself for main-frame loads; "" when unknown. */
    const char *first_party_url;
    const char *method;           /* HTTP method, utf8 */
    /* Fetch destination string ("","document","script","image",
     * "style","font","iframe","audio","video","worker",...) —
     * servo's content_security_policy::Destination::as_str value. */
    const char *destination;
    int is_for_main_frame;        /* 1 for top-document loads */
    int is_redirect;              /* 1 on a redirect hop */
} SeLoadInfo;

typedef int (*SeLoadGate)(void *userdata, const SeLoadInfo *info,
                          SeHostReply *reply);

/* Absent on artifacts predating ENG35 — resolve optionally; a
 * missing symbol means loads cannot be gated (request policies stay
 * unenforced, the old observable-only behavior). */
void se_set_load_gate(void *userdata, SeLoadGate gate);

/* ENG20: panic containment. Every export catches unwinding panics at
 * the ABI boundary; the first caught panic poisons the instance so all
 * later exports short-circuit to an error/no-op — a panicking webview
 * degrades to a dead tab instead of aborting the host. This export is
 * absent on artifacts predating ENG20 — resolve optionally and treat a
 * missing symbol as "no panic observed" (unverifiable, pre-fix). */
int se_is_poisoned(SeInstance instance);

/* ENG23: GL_RENDERER of the view's painter context, sampled once at
 * view create — the host's handle on "did the software rasterizer actually
 * engage" rather than assuming the env override took. Pointer borrows
 * the instance (never free); NULL on a null/poisoned instance and on
 * artifacts predating ENG23 — resolve optionally. */
const char *se_gl_renderer(SeInstance instance);

/* ENG27/ENG36: lifecycle introspection.
 * se_instance_active — non-zero while at least one view is live
 * (se_view_create registers it; se_view_destroy unregisters it BEFORE
 * the fallible teardown, so even a panicking drop leaves the registry
 * honest).  This is the host-side proof that a crashed+destroyed
 * view released — the state a recovery path (ENG28) needs to find.
 * se_can_init — non-zero when a se_view_create issued right now would
 * produce a live view: true pre-engine AND whenever the engine
 * survives, whether views are live or all destroyed (ENG36 multi-view
 * + per-view re-init).  Stays 0 only for genuinely unrecoverable
 * state — a failed engine build or post-se_shutdown.  Absent on
 * pre-ENG27 artifacts — resolve optionally; a missing symbol reads
 * as "unverifiable / no re-init". */
int se_instance_active(void);
int se_can_init(void);

/* ENG28: explicit engine teardown. Drops the shared engine while the
 * GUI thread is intact (the engine handle is ManuallyDrop so an
 * unshut-down engine leaks harmlessly at thread exit instead of
 * running its shutdown spin inside a TLS destructor). No-op when no
 * engine exists or after a previous call; live views are NOT
 * destroyed — se_view_destroy them first. Absent on pre-ENG28
 * artifacts — resolve optionally; a missing symbol leaves the engine
 * resident until process exit, as before. */
void se_shutdown(void);

/* Legacy name for se_view_destroy (pre-ENG36 hosts).  The destroy
 * unregisters the view even when teardown panics (a webrender GL
 * panic used to leak the box AND strand the registry forever).  Only
 * the pointer that registered unregisters — a stale or
 * double-destroyed pointer is a harmless no-op. */
void se_destroy(SeInstance instance);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_EMBED_H */
