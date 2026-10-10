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
void se_set_zoom(SeInstance instance, float zoom);
void se_eval_js(SeInstance instance, const char *script);
int se_frame_size(SeInstance instance, unsigned *width, unsigned *height);
long se_frame_copy(SeInstance instance, unsigned char *buf, unsigned long buf_len);
unsigned long se_request_count(SeInstance instance);
long se_request_log(SeInstance instance, char *buf, unsigned long buf_len);
void se_destroy(SeInstance instance);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_EMBED_H */
