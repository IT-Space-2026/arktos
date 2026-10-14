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

SeInstance se_init(SeCallbacks callbacks, unsigned width, unsigned height,
                   const char *config_dir);
void se_spin(SeInstance instance);
int se_load(SeInstance instance, const char *url);
void se_resize(SeInstance instance, unsigned width, unsigned height);
void se_mouse_move(SeInstance instance, float x, float y);
void se_mouse_button(SeInstance instance, int down, int button, float x, float y);
void se_wheel(SeInstance instance, double dx, double dy, float x, float y);
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
 * Registered once per process (before or after se_init — only one
 * SeInstance can ever exist anyway). Called synchronously from inside
 * se_spin() on the GUI thread for every resource load; the host
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

/* ENG20: panic containment. Every export catches unwinding panics at
 * the ABI boundary; the first caught panic poisons the instance so all
 * later exports short-circuit to an error/no-op — a panicking webview
 * degrades to a dead tab instead of aborting the host. This export is
 * absent on artifacts predating ENG20 — resolve optionally and treat a
 * missing symbol as "no panic observed" (unverifiable, pre-fix). */
int se_is_poisoned(SeInstance instance);

/* ENG23: GL_RENDERER of the painter's context, sampled once at
 * se_init — the host's handle on "did the software rasterizer actually
 * engage" rather than assuming the env override took. Pointer borrows
 * the instance (never free); NULL on a null/poisoned instance and on
 * artifacts predating ENG23 — resolve optionally. */
const char *se_gl_renderer(SeInstance instance);

/* ENG27: crash-recovery introspection.
 * se_instance_active — non-zero while the instance slot is occupied
 * (se_init claims it; se_destroy hands it back BEFORE the fallible
 * teardown, so even a panicking drop leaves it free).  This is the
 * host-side proof that a crashed+destroyed instance released its
 * slot — the state a recovery path (ENG28) needs to find.
 * se_can_init — non-zero when a se_init issued right now would
 * produce a live instance: true pre-init, false once the once-per-
 * process engine is consumed.  Absent on pre-ENG27 artifacts —
 * resolve optionally; a missing symbol reads as "unverifiable /
 * no re-init". */
int se_instance_active(void);
int se_can_init(void);

/* ENG27: se_destroy releases the instance slot even when teardown
 * panics (a webrender GL panic used to leak the box AND strand the
 * slot forever).  Only the pointer that claimed the slot releases
 * it — a stale or double-destroyed pointer is a harmless no-op. */
void se_destroy(SeInstance instance);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_EMBED_H */
