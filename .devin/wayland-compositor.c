/*
 * wayland-compositor.c — PACK01 verification harness.
 *
 * A minimal headless Wayland compositor sufficient to run a Qt6/wayland
 * client (incl. QtWebEngine) end-to-end without touching the user's real
 * display session.  Implements just enough of:
 *   wl_compositor, wl_subcompositor, wl_shm, wl_seat (pointer only),
 *   wl_output, wl_data_device_manager, xdg_wm_base
 * Surfaces get a configure + frame-done on commit; buffers are released
 * immediately.  Everything else is a no-op.
 *
 * Build:  wayland-scanner server-header $XDG_SHELL_XML xdg-shell-server.h
 *         wayland-scanner private-code $XDG_SHELL_XML xdg-shell-code.c
 *         cc -O1 -I. -o wayland-compositor wayland-compositor.c \
 *            xdg-shell-code.c $(pkg-config --cflags --libs wayland-server)
 * Run:    XDG_RUNTIME_DIR=/some/dir ./wayland-compositor
 *         (socket name: $WAYLAND_DISPLAY or "arora-smoke")
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server.h>

#include "xdg-shell-server.h"

static uint32_t next_serial = 1;
static struct wl_display *g_display;

/* ---------- correctly-typed no-ops ---------- */

static void noop0(struct wl_client *c, struct wl_resource *r)
{ (void)c; (void)r; }
static void noop1i(struct wl_client *c, struct wl_resource *r, int32_t a)
{ (void)c; (void)r; (void)a; }
static void noop1u(struct wl_client *c, struct wl_resource *r, uint32_t a)
{ (void)c; (void)r; (void)a; }
static void noop1r(struct wl_client *c, struct wl_resource *r,
                   struct wl_resource *a)
{ (void)c; (void)r; (void)a; }
static void noop1s(struct wl_client *c, struct wl_resource *r, const char *s)
{ (void)c; (void)r; (void)s; }
static void noop2i(struct wl_client *c, struct wl_resource *r,
                   int32_t a, int32_t b)
{ (void)c; (void)r; (void)a; (void)b; }
static void noop4i(struct wl_client *c, struct wl_resource *r,
                   int32_t a, int32_t b, int32_t d, int32_t e)
{ (void)c; (void)r; (void)a; (void)b; (void)d; (void)e; }
static void noop_seat_serial(struct wl_client *c, struct wl_resource *r,
                             struct wl_resource *seat, uint32_t serial)
{ (void)c; (void)r; (void)seat; (void)serial; }
static void noop_winmenu(struct wl_client *c, struct wl_resource *r,
                         struct wl_resource *seat, uint32_t serial,
                         int32_t x, int32_t y)
{ (void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y; }
static void noop_resize(struct wl_client *c, struct wl_resource *r,
                        struct wl_resource *seat, uint32_t serial,
                        uint32_t edges)
{ (void)c; (void)r; (void)seat; (void)serial; (void)edges; }
static void noop_cursor(struct wl_client *c, struct wl_resource *r,
                        uint32_t serial, struct wl_resource *surface,
                        int32_t x, int32_t y)
{ (void)c; (void)r; (void)serial; (void)surface; (void)x; (void)y; }
static void noop_start_drag(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *source,
                            struct wl_resource *origin,
                            struct wl_resource *icon, uint32_t serial)
{ (void)c; (void)r; (void)source; (void)origin; (void)icon; (void)serial; }
static void noop_selection(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *source, uint32_t serial)
{ (void)c; (void)r; (void)source; (void)serial; }
static void noop_reposition(struct wl_client *c, struct wl_resource *r,
                            struct wl_resource *positioner, uint32_t token)
{ (void)c; (void)r; (void)positioner; (void)token; }
static void res_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

/* ---------- wl_region ---------- */
static const struct wl_region_interface region_impl = {
    .destroy = res_destroy,
    .add = noop4i,
    .subtract = noop4i,
};

/* ---------- wl_buffer ---------- */
static const struct wl_buffer_interface buffer_impl = {
    .destroy = res_destroy,
};

/* ---------- wl_shm_pool ---------- */
static void pool_create_buffer(struct wl_client *c, struct wl_resource *pool,
                               uint32_t id, int32_t offset,
                               int32_t w, int32_t h, int32_t stride,
                               uint32_t format)
{
    (void)pool; (void)offset; (void)w; (void)h; (void)stride; (void)format;
    struct wl_resource *b = wl_resource_create(c, &wl_buffer_interface,
                                             wl_resource_get_version(pool), id);
    wl_resource_set_implementation(b, &buffer_impl, NULL, NULL);
}
static const struct wl_shm_pool_interface pool_impl = {
    .create_buffer = pool_create_buffer,
    .destroy = res_destroy,
    .resize = noop1i,
};

/* ---------- wl_shm ---------- */
static void shm_create_pool(struct wl_client *c, struct wl_resource *shm,
                            uint32_t id, int32_t fd, int32_t size)
{
    (void)size;
    close(fd);
    struct wl_resource *p = wl_resource_create(c, &wl_shm_pool_interface,
                                             wl_resource_get_version(shm), id);
    wl_resource_set_implementation(p, &pool_impl, NULL, NULL);
}
static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
};
static void shm_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
    (void)d;
    if (ver > 1) ver = 1;
    struct wl_resource *r = wl_resource_create(c, &wl_shm_interface, ver, id);
    wl_resource_set_implementation(r, &shm_impl, NULL, NULL);
    wl_shm_send_format(r, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(r, WL_SHM_FORMAT_XRGB8888);
}

/* ---------- wl_surface ---------- */
struct surface_state {
    struct wl_resource *frame;
};

static void surface_attach(struct wl_client *c, struct wl_resource *s,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    (void)c; (void)s; (void)x; (void)y;
    if (buffer)
        wl_buffer_send_release(buffer);
}
static void surface_frame(struct wl_client *c, struct wl_resource *s,
                          uint32_t id)
{
    struct surface_state *st = wl_resource_get_user_data(s);
    if (st->frame)
        wl_resource_destroy(st->frame);
    st->frame = wl_resource_create(c, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(st->frame, NULL, NULL, NULL);
}
static void surface_commit(struct wl_client *c, struct wl_resource *s)
{
    (void)c;
    struct surface_state *st = wl_resource_get_user_data(s);
    if (st->frame) {
        wl_callback_send_done(st->frame, 0);
        wl_resource_destroy(st->frame);
        st->frame = NULL;
    }
}
static void surface_destroy_notify(struct wl_resource *r)
{
    free(wl_resource_get_user_data(r));
}
static const struct wl_surface_interface surface_impl = {
    .destroy = res_destroy,
    .attach = surface_attach,
    .damage = noop4i,
    .frame = surface_frame,
    .set_opaque_region = noop1r,
    .set_input_region = noop1r,
    .commit = surface_commit,
    .set_buffer_transform = noop1i,
    .set_buffer_scale = noop1i,
    .damage_buffer = noop4i,
    .offset = noop2i,
};

/* ---------- wl_compositor ---------- */
static void compositor_create_surface(struct wl_client *c,
                                      struct wl_resource *comp, uint32_t id)
{
    int ver = wl_resource_get_version(comp);
    struct surface_state *st = calloc(1, sizeof(*st));
    struct wl_resource *s = wl_resource_create(c, &wl_surface_interface,
                                             ver < 6 ? ver : 6, id);
    wl_resource_set_implementation(s, &surface_impl, st,
                                   surface_destroy_notify);
}
static void compositor_create_region(struct wl_client *c,
                                     struct wl_resource *comp, uint32_t id)
{
    (void)comp;
    struct wl_resource *r = wl_resource_create(c, &wl_region_interface, 1, id);
    wl_resource_set_implementation(r, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

/* ---------- wl_subcompositor ---------- */
static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = res_destroy,
    .set_position = noop2i,
    .place_above = noop1r,
    .place_below = noop1r,
    .set_sync = noop0,
    .set_desync = noop0,
};
static void subcomp_get_subsurface(struct wl_client *c,
                                   struct wl_resource *subcomp, uint32_t id,
                                   struct wl_resource *surface,
                                   struct wl_resource *parent)
{
    (void)surface; (void)parent;
    struct wl_resource *r = wl_resource_create(c, &wl_subsurface_interface,
                                             wl_resource_get_version(subcomp),
                                             id);
    wl_resource_set_implementation(r, &subsurface_impl, NULL, NULL);
}
static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = res_destroy,
    .get_subsurface = subcomp_get_subsurface,
};

/* ---------- wl_seat (pointer only) ---------- */
static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = noop_cursor,
    .release = res_destroy,
};
static void seat_get_pointer(struct wl_client *c, struct wl_resource *seat,
                             uint32_t id)
{
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface,
                                             wl_resource_get_version(seat), id);
    wl_resource_set_implementation(p, &pointer_impl, NULL, NULL);
}
static void seat_get_keyboard(struct wl_client *c, struct wl_resource *seat,
                              uint32_t id)
{
    /* Keyboard capability is never advertised, but a client may still ask —
     * give it a resource that never emits events. */
    static const struct wl_keyboard_interface kb_impl = {
        .release = res_destroy,
    };
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface,
                                             wl_resource_get_version(seat), id);
    wl_resource_set_implementation(k, &kb_impl, NULL, NULL);
}
static void seat_get_touch(struct wl_client *c, struct wl_resource *seat,
                           uint32_t id)
{
    static const struct wl_touch_interface touch_impl = {
        .release = res_destroy,
    };
    struct wl_resource *t = wl_resource_create(c, &wl_touch_interface,
                                             wl_resource_get_version(seat), id);
    wl_resource_set_implementation(t, &touch_impl, NULL, NULL);
}
static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = res_destroy,
};
static void seat_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
    (void)d;
    if (ver > 8) ver = 8;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, ver, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER);
    if (ver >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(r, "smoke-seat");
}

/* ---------- wl_output ---------- */
static const struct wl_output_interface output_impl = {
    .release = res_destroy,
};
static void output_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
    (void)d;
    if (ver > 4) ver = 4;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, ver, id);
    wl_resource_set_implementation(r, &output_impl, NULL, NULL);
    wl_output_send_geometry(r, 0, 0, 400, 300,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "smoke", "smoke-output",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 60000);
    if (ver >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(r, 1);
    if (ver >= WL_OUTPUT_NAME_SINCE_VERSION) {
        wl_output_send_name(r, "smoke-0");
        wl_output_send_description(r, "headless smoke output");
    }
    if (ver >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(r);
}

/* ---------- wl_data_device_manager ---------- */
static const struct wl_data_source_interface data_source_impl = {
    .offer = noop1s,
    .destroy = res_destroy,
    .set_actions = noop1u,
};
static const struct wl_data_device_interface data_device_impl = {
    .start_drag = noop_start_drag,
    .set_selection = noop_selection,
    .release = res_destroy,
};
static void ddm_create_data_source(struct wl_client *c,
                                   struct wl_resource *ddm, uint32_t id)
{
    struct wl_resource *s = wl_resource_create(c, &wl_data_source_interface,
                                             wl_resource_get_version(ddm), id);
    wl_resource_set_implementation(s, &data_source_impl, NULL, NULL);
}
static void ddm_get_data_device(struct wl_client *c,
                                struct wl_resource *ddm, uint32_t id,
                                struct wl_resource *seat)
{
    (void)seat;
    struct wl_resource *d = wl_resource_create(c, &wl_data_device_interface,
                                             wl_resource_get_version(ddm), id);
    wl_resource_set_implementation(d, &data_device_impl, NULL, NULL);
}
static const struct wl_data_device_manager_interface ddm_impl = {
    .create_data_source = ddm_create_data_source,
    .get_data_device = ddm_get_data_device,
};

/* ---------- xdg_positioner ---------- */
static const struct xdg_positioner_interface positioner_impl = {
    .destroy = res_destroy,
    .set_size = noop2i,
    .set_anchor_rect = noop4i,
    .set_anchor = noop1u,
    .set_gravity = noop1u,
    .set_constraint_adjustment = noop1u,
    .set_offset = noop2i,
    .set_reactive = noop0,
    .set_parent_size = noop2i,
    .set_parent_configure = noop1u,
};

/* ---------- xdg_toplevel ---------- */
static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = res_destroy,
    .set_parent = noop1r,
    .set_title = noop1s,
    .set_app_id = noop1s,
    .show_window_menu = noop_winmenu,
    .move = noop_seat_serial,
    .resize = noop_resize,
    .set_max_size = noop2i,
    .set_min_size = noop2i,
    .set_maximized = noop0,
    .unset_maximized = noop0,
    .set_fullscreen = noop1r,
    .unset_fullscreen = noop0,
    .set_minimized = noop0,
};

/* ---------- xdg_popup ---------- */
static const struct xdg_popup_interface popup_impl = {
    .destroy = res_destroy,
    .grab = noop_seat_serial,
    .reposition = noop_reposition,
};

/* ---------- xdg_surface ---------- */
static void xsurface_get_toplevel(struct wl_client *c,
                                  struct wl_resource *xs, uint32_t id)
{
    int ver = wl_resource_get_version(xs);
    struct wl_resource *t = wl_resource_create(c, &xdg_toplevel_interface,
                                             ver < 6 ? ver : 6, id);
    wl_resource_set_implementation(t, &toplevel_impl, NULL, NULL);
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(t, 0, 0, &states);
    wl_array_release(&states);
    if (wl_resource_get_version(t) >=
            XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
        xdg_toplevel_send_configure_bounds(t, 0, 0);
    xdg_surface_send_configure(xs, next_serial++);
}
static void xsurface_get_popup(struct wl_client *c, struct wl_resource *xs,
                               uint32_t id, struct wl_resource *parent,
                               struct wl_resource *positioner)
{
    (void)parent; (void)positioner;
    struct wl_resource *p = wl_resource_create(c, &xdg_popup_interface, 6, id);
    wl_resource_set_implementation(p, &popup_impl, NULL, NULL);
    xdg_popup_send_configure(p, 0, 0, 0, 0);
    xdg_surface_send_configure(xs, next_serial++);
}
static const struct xdg_surface_interface xsurface_impl = {
    .destroy = res_destroy,
    .get_toplevel = xsurface_get_toplevel,
    .get_popup = xsurface_get_popup,
    .set_window_geometry = noop4i,
    .ack_configure = noop1u,
};

/* ---------- xdg_wm_base ---------- */
static void wm_base_create_positioner(struct wl_client *c,
                                    struct wl_resource *wm, uint32_t id)
{
    (void)wm;
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface, 6,
                                             id);
    wl_resource_set_implementation(p, &positioner_impl, NULL, NULL);
}
static void wm_base_get_xdg_surface(struct wl_client *c,
                                  struct wl_resource *wm, uint32_t id,
                                  struct wl_resource *surface)
{
    (void)surface;
    struct wl_resource *xs = wl_resource_create(c, &xdg_surface_interface,
                                              wl_resource_get_version(wm), id);
    wl_resource_set_implementation(xs, &xsurface_impl, NULL, NULL);
}
static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = res_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = noop1u,
};

/* ---------- global bind boilerplate ---------- */
struct simple_global {
    const struct wl_interface *iface;
    const void *impl;
    int ver;
};

static void simple_bind(struct wl_client *c, void *d, uint32_t ver, uint32_t id)
{
    const struct simple_global *g = d;
    uint32_t v = ver < (uint32_t)g->ver ? ver : (uint32_t)g->ver;
    struct wl_resource *r = wl_resource_create(c, g->iface, v, id);
    wl_resource_set_implementation(r, g->impl, NULL, NULL);
}

static struct simple_global g_compositor    = { &wl_compositor_interface,
                                                &compositor_impl, 6 };
static struct simple_global g_subcompositor = { &wl_subcompositor_interface,
                                                &subcompositor_impl, 1 };
static struct simple_global g_ddm           = { &wl_data_device_manager_interface,
                                                &ddm_impl, 3 };
static struct simple_global g_wm_base       = { &xdg_wm_base_interface,
                                                &wm_base_impl, 6 };

/* ---------- main ---------- */
static void on_signal(int sig)
{
    (void)sig;
    wl_display_terminate(g_display);
}

static void client_created(struct wl_listener *l, void *data)
{
    (void)l;
    fprintf(stderr, "compositor: client %p connected\n", wl_client_get_display((struct wl_client *)data));
}
static struct wl_listener client_created_listener = { .notify = client_created };

int main(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
        return 1;
    }

    g_display = wl_display_create();
    if (!g_display) {
        fprintf(stderr, "wl_display_create failed\n");
        return 1;
    }
    wl_display_add_client_created_listener(g_display, &client_created_listener);

    /* Fixed socket name — the harness gives us a dedicated XDG_RUNTIME_DIR. */
    const char *sock = getenv("WAYLAND_DISPLAY");
    if (!sock || !*sock)
        sock = "arora-smoke";
    if (wl_display_add_socket(g_display, sock) < 0) {
        fprintf(stderr, "wl_display_add_socket(%s) failed\n", sock);
        return 1;
    }

    wl_global_create(g_display, &wl_compositor_interface, 6,
                     &g_compositor, simple_bind);
    wl_global_create(g_display, &wl_subcompositor_interface, 1,
                     &g_subcompositor, simple_bind);
    wl_global_create(g_display, &wl_shm_interface, 1, NULL, shm_bind);
    wl_global_create(g_display, &wl_seat_interface, 8, NULL, seat_bind);
    wl_global_create(g_display, &wl_output_interface, 4, NULL, output_bind);
    wl_global_create(g_display, &wl_data_device_manager_interface, 3,
                     &g_ddm, simple_bind);
    wl_global_create(g_display, &xdg_wm_base_interface, 6,
                     &g_wm_base, simple_bind);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGCHLD, SIG_IGN);

    printf("WAYLAND_DISPLAY=%s\n", sock);
    fflush(stdout);
    wl_display_run(g_display);
    wl_display_destroy(g_display);
    return 0;
}
