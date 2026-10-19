/* WK21 stretch-audit probe — plain GLib, GMainLoop driven.
 *
 *   wk21-probe <workdir>
 *
 * Env contract (set by run-wk21.sh):
 *   WK21_SOCKS_URI   e.g. socks5://localhost:PORT   (localhost on
 *                    purpose: the proxy host's own getaddrinfo in the
 *                    NetworkProcess is the shim-coverage positive
 *                    control — without a RESOLVE line for it from a
 *                    child pid, silence elsewhere would be unproven)
 *   WK21_TARGET      e.g. http://wk21-<tag>.test/   (unresolvable name —
 *                    a load only succeeds through remote-DNS SOCKS)
 *   WK21_EXT         extension fixture dir for webkit_web_extension_new
 *   WK21_DNSLOG      shim log path (dns-shim.c writes RESOLVE/CONNECT/
 *                    SENDTO lines from every process that preloads it)
 *
 * Phases, each printed as [WK21] lines for the run script's report:
 *   selfctl   — the probe calls getaddrinfo itself (shim works here)
 *   socks5    — ephemeral session + CUSTOM socks5 proxy, load TARGET
 *   prefetch  — webkit_network_session_prefetch_dns of a unique name
 *   socks5h   — same load through a socks5h:// URI (scheme accepted?)
 *   noproxy   — DEFAULT-mode session loads TARGET (must FAIL — proves
 *               the name only exists through the proxy)
 *   webrtc    — enable-webrtc default + RTCPeerConnection presence +
 *               host-candidate gather (real IPs vs mDNS vs none)
 *   inspector — enable-developer-extras + toggle_inspector, observe
 *   extension — webkit_web_extension_new manifest parse + a view built
 *               with web-extension-mode=MANIFESTV3 (readback)
 */

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <jsc/jsc.h>
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/wpe-platform.h>

static GMainLoop *loop;
static gboolean timed_out;
static const char *g_run;      /* unique host tag for this run */
static const char *g_socks;    /* socks5://… uri */
static const char *g_target;   /* fixture url */
static const char *g_extdir;

static gboolean quit_cb(gpointer)
{
    timed_out = TRUE;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void pump_seconds(guint secs)
{
    timed_out = FALSE;
    g_timeout_add_seconds(secs, quit_cb, NULL);
    g_main_loop_run(loop);
}

/* ---------- view stack (WK02/WK08 recipe) ---------- */

typedef struct {
    WPEDisplay *display;
    WPEToplevel *toplevel;
    WebKitWebView *webView;
    gboolean finished;
    gboolean failed;
    gchar *failErr;
} Stack;

static void on_load_changed(WebKitWebView *, WebKitLoadEvent ev,
                            Stack *s)
{
    g_print("[WK21] load-changed %d\n", (int)ev);
    if (ev == WEBKIT_LOAD_FINISHED)
        s->finished = TRUE;
}

static gboolean on_load_failed(WebKitWebView *, WebKitLoadEvent ev,
                               const gchar *uri, GError *err, Stack *s)
{
    g_print("[WK21] load-failed ev=%d uri=%s err=%s\n", (int)ev,
            uri ? uri : "?", err ? err->message : "?");
    s->failed = TRUE;
    s->failErr = g_strdup(err ? err->message : "?");
    return FALSE;
}

static gboolean wait_load(Stack *s, guint secs)
{
    guint elapsed = 0;
    while (!s->finished && !s->failed && elapsed < secs) {
        GSource *tick = g_timeout_source_new_seconds(1);
        g_source_set_callback(tick, quit_cb, NULL, NULL);
        timed_out = FALSE;
        g_source_attach(tick, NULL);
        g_main_loop_run(loop);
        g_source_unref(tick);
        ++elapsed;
    }
    return s->finished;
}

static const char *load_verdict(const Stack *s)
{
    /* load-failed can precede a FINISHED for the engine error page —
     * the failure is the truth. */
    if (s->failed)
        return "FAILED";
    return s->finished ? "FINISHED" : "TIMEOUT";
}

static Stack *stack_new(WebKitNetworkSession *session)
{
    Stack *s = g_new0(Stack, 1);
    s->display = wpe_display_headless_new();
    GError *err = NULL;
    if (!s->display || !wpe_display_connect(s->display, &err)) {
        g_print("[WK21] display failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return s;
    }
    s->toplevel = wpe_display_create_toplevel(s->display, 1);
    if (session)
        s->webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "display", s->display,
            "network-session", session, NULL));
    else
        s->webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "display", s->display, NULL));
    if (!s->webView)
        return s;
    WPEView *view = webkit_web_view_get_wpe_view(s->webView);
    if (!view) { g_print("[WK21] no wpe view\n"); return s; }
    wpe_view_set_toplevel(view, s->toplevel);
    wpe_toplevel_resize(s->toplevel, 640, 480);
    wpe_view_set_visible(view, TRUE);
    wpe_view_map(view);
    wpe_view_focus_in(view);
    g_signal_connect(s->webView, "load-changed",
                     G_CALLBACK(on_load_changed), s);
    g_signal_connect(s->webView, "load-failed",
                     G_CALLBACK(on_load_failed), s);
    return s;
}

static void stack_free(Stack *s)
{
    g_clear_object(&s->webView);
    g_clear_object(&s->toplevel);
    g_clear_object(&s->display);
    g_free(s->failErr);
    g_free(s);
}

/* ---------- async JS eval helper (sync-pumped) ---------- */

typedef struct {
    gboolean done;
    JSCValue *value;
    GError *error;
} EvalResult;

static void eval_done(WebKitWebView *v, GAsyncResult *res, EvalResult *r)
{
    r->value = webkit_web_view_evaluate_javascript_finish(
        v, res, &r->error);
    r->done = TRUE;
    g_main_loop_quit(loop);
}

static EvalResult eval_js(WebKitWebView *v, const char *js)
{
    EvalResult r = {FALSE, NULL, NULL};
    webkit_web_view_evaluate_javascript(v, js, -1, NULL, NULL, NULL,
        (GAsyncReadyCallback)eval_done, &r);
    guint guard = 0;
    while (!r.done && guard < 100) {
        timed_out = FALSE;
        g_timeout_add(100, quit_cb, NULL);
        g_main_loop_run(loop);
        ++guard;
    }
    return r;
}

/* ---------- phases ---------- */

int main(int argc, char **argv)
{
    g_run = getenv("WK21_RUN");
    g_socks = getenv("WK21_SOCKS_URI");
    g_target = getenv("WK21_TARGET");
    g_extdir = getenv("WK21_EXT");
    if (!g_run || !g_socks || !g_target) {
        fprintf(stderr, "need WK21_RUN/WK21_SOCKS_URI/WK21_TARGET\n");
        return 2;
    }
    loop = g_main_loop_new(NULL, FALSE);

    /* Phase 0 — shim self-control: a RESOLVE line for this name must
     * appear in $WK21_DNSLOG from THIS process. */
    {
        struct addrinfo *res = NULL;
        char ctl[128];
        snprintf(ctl, sizeof(ctl), "wk21selfctl-%s.invalid", g_run);
        getaddrinfo(ctl, NULL, NULL, &res);
        if (res)
            freeaddrinfo(res);
        g_print("[WK21] phase=selfctl lookup %s issued (expect RESOLVE "
                "line from comm=wk21-probe)\n", ctl);
    }

    /* Phase 1 — socks5 proxied load on an ephemeral session. */
    WebKitNetworkSession *session =
        webkit_network_session_new_ephemeral();
    WebKitNetworkProxySettings *ps =
        webkit_network_proxy_settings_new(g_socks, NULL);
    webkit_network_session_set_proxy_settings(
        session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
    g_print("[WK21] phase=socks5 proxy=%s target=%s\n", g_socks,
            g_target);
    Stack *s1 = stack_new(session);
    if (s1->webView) {
        webkit_web_view_load_uri(s1->webView, g_target);
        wait_load(s1, 30);
        g_print("[WK21] socks5-load result=%s%s\n",
                load_verdict(s1),
                s1->failErr ? s1->failErr : "");
        gchar *title = NULL;
        g_object_get(s1->webView, "title", &title, NULL);
        g_print("[WK21] socks5-load title=%s\n", title ? title : "?");
        g_free(title);
    }

    /* Phase 2 — explicit prefetch under the proxied session: a RESOLVE
     * line for this unique name = prefetch resolved locally anyway. */
    {
        char pf[128];
        snprintf(pf, sizeof(pf), "wk21pf-%s.invalid", g_run);
        g_print("[WK21] phase=prefetch host=%s\n", pf);
        webkit_network_session_prefetch_dns(session, pf);
        pump_seconds(3);
    }

    /* Phase 2b — page-driven dns-prefetch: the fixture page carries
     * <link rel="dns-prefetch" href="//wk21pf-page-RUN.invalid">.
     * Report the setting's default, force it ON, reload, and let the
     * shim log say whether page hints resolve locally under proxy. */
    if (s1->webView) {
        WebKitSettings *set = webkit_web_view_get_settings(s1->webView);
        g_print("[WK21] dns-prefetching default=%d\n",
                webkit_settings_get_enable_dns_prefetching(set));
        webkit_settings_set_enable_dns_prefetching(set, TRUE);
        s1->finished = FALSE;
        s1->failed = FALSE;
        g_clear_pointer(&s1->failErr, g_free);
        g_print("[WK21] phase=prefetch-page reload with hint\n");
        webkit_web_view_reload(s1->webView);
        wait_load(s1, 20);
        g_print("[WK21] prefetch-page reload result=%s\n",
                load_verdict(s1));
        pump_seconds(3); /* prefetch happens post-load */
    }

    /* Phase 2c — WebSocket: ws:// rides the same libsoup proxy path
     * in theory — verify empirically: a CONNECT for the ws host in
     * socks.log = proxied+remote DNS; a local RESOLVE + direct connect
     * = the classic bypass. */
    if (s1->webView) {
        char *js = g_strdup_printf(
            "try{new WebSocket('ws://wk21ws-%s.test:%s/');'sent'}"
            "catch(e){'THROW:'+e}",
            g_run, getenv("WK21_SOCKS_PORT") ? getenv("WK21_SOCKS_PORT") : "80");
        g_print("[WK21] phase=websocket target=ws://wk21ws-%s.test\n",
                g_run);
        EvalResult r = eval_js(s1->webView, js);
        if (r.value && jsc_value_is_string(r.value)) {
            gchar *st = jsc_value_to_string(r.value);
            g_print("[WK21] websocket eval=%s\n", st);
            g_free(st);
        }
        g_free(js);
        pump_seconds(4); /* let the attempt reach the proxy log */
    }

    /* Phase 3 — socks5h:// scheme accepted by the proxy layer? */
    {
        char *h = g_strdup(g_socks);
        if (g_str_has_prefix(h, "socks5://"))
            h = g_strconcat("socks5h://", h + strlen("socks5://"), NULL);
        WebKitNetworkSession *hs =
            webkit_network_session_new_ephemeral();
        WebKitNetworkProxySettings *hps =
            webkit_network_proxy_settings_new(h, NULL);
        webkit_network_session_set_proxy_settings(
            hs, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, hps);
        char *t = g_strdup_printf("http://wk21h-%s.test/", g_run);
        g_print("[WK21] phase=socks5h proxy=%s target=%s\n", h, t);
        Stack *s3 = stack_new(hs);
        if (s3->webView) {
            webkit_web_view_load_uri(s3->webView, t);
            wait_load(s3, 20);
            g_print("[WK21] socks5h-load result=%s%s\n",
                    load_verdict(s3),
                    s3->failErr ? s3->failErr : "");
        }
        stack_free(s3);
        webkit_network_proxy_settings_free(hps);
        g_object_unref(hs);
        g_free(h);
        g_free(t);
    }

    /* Phase 4 — no-proxy control: the .test name must fail without the
     * fixture mapping; any success here means a proxy bypass path. */
    {
        WebKitNetworkSession *ns =
            webkit_network_session_new_ephemeral();
        webkit_network_session_set_proxy_settings(
            ns, WEBKIT_NETWORK_PROXY_MODE_NO_PROXY, NULL);
        char *t = g_strdup_printf("http://wk21np-%s.test/", g_run);
        g_print("[WK21] phase=noproxy target=%s (must fail)\n", t);
        Stack *s4 = stack_new(ns);
        if (s4->webView) {
            webkit_web_view_load_uri(s4->webView, t);
            wait_load(s4, 15);
            g_print("[WK21] noproxy-load result=%s%s\n",
                    load_verdict(s4),
                    s4->failErr ? s4->failErr : "");
        }
        stack_free(s4);
        g_object_unref(ns);
        g_free(t);
    }

    /* Phase 5 — WebRTC. enable-webrtc exists in the build (ABI says
     * so); report the default, force it on, then probe the page. */
    if (s1->webView) {
        WebKitSettings *set = webkit_web_view_get_settings(s1->webView);
        gboolean we = webkit_settings_get_enable_webrtc(set);
        g_print("[WK21] phase=webrtc enable-webrtc default=%s\n",
                we ? "true" : "false");
        webkit_settings_set_enable_webrtc(set, TRUE);
        EvalResult r = eval_js(s1->webView,
            "(function(){"
            "window.__wk21rtc='PENDING';"
            "if(typeof RTCPeerConnection==='undefined'){"
            "  window.__wk21rtc='NO-RTC';return 'NO-RTC';}"
            "try{"
            " var pc=new RTCPeerConnection({iceServers:[]});"
            " var c=[];"
            " pc.onicecandidate=function(e){"
            "   c.push(e.candidate?e.candidate.candidate:'<null-end>');};"
            " pc.onicegatheringstatechange=function(){"
            "   if(pc.iceGatheringState==='complete')"
            "     window.__wk21rtc='DONE:'+JSON.stringify(c);};"
            " pc.createDataChannel('x');"
            " pc.createOffer().then(function(o){"
            "   return pc.setLocalDescription(o);})"
            "  .catch(function(e){window.__wk21rtc='ERR:'+e;});"
            " setTimeout(function(){"
            "   if(window.__wk21rtc==='PENDING')"
            "     window.__wk21rtc='TIMEOUT:'+JSON.stringify(c);},4000);"
            " return 'ARMED';"
            "}catch(e){window.__wk21rtc='THROW:'+e;return 'THROW';}}"
            ")()");
        if (r.error)
            g_print("[WK21] webrtc eval error: %s\n", r.error->message);
        g_print("[WK21] webrtc gather armed\n");
        for (int i = 0; i < 14; ++i) {
            pump_seconds(1);
            EvalResult q = eval_js(s1->webView,
                "window.__wk21rtc");
            if (q.value && jsc_value_is_string(q.value)) {
                gchar *str = jsc_value_to_string(q.value);
                if (g_strcmp0(str, "PENDING") != 0) {
                    g_print("[WK21] webrtc result=%s\n", str);
                    g_free(str);
                    break;
                }
                g_free(str);
            }
            if (i == 13)
                g_print("[WK21] webrtc result=STILL-PENDING\n");
        }
    }

    /* Phase 6 — inspector: toggle on the loaded page, watch what
     * happens (crash / message / nothing).  No embedder API exists —
     * this observes whether the built-in inspector does anything at
     * all under the headless platform. */
    if (s1->webView) {
        WebKitSettings *set = webkit_web_view_get_settings(s1->webView);
        webkit_settings_set_enable_developer_extras(set, TRUE);
        g_print("[WK21] phase=inspector toggling\n");
        webkit_web_view_toggle_inspector(s1->webView);
        pump_seconds(3);
        g_print("[WK21] inspector toggled, process alive, "
                "is-loading=%d\n",
                webkit_web_view_is_loading(s1->webView));
        webkit_web_view_toggle_inspector(s1->webView);
        pump_seconds(1);
        g_print("[WK21] inspector re-toggled (close), alive\n");
    }

    /* Phase 7 — WebExtension manifest parse + extension-mode view. */
    {
        GError *err = NULL;
        WebKitWebExtension *ext =
            webkit_web_extension_new(g_extdir, &err);
        if (!ext) {
            g_print("[WK21] phase=extension manifest-parse FAILED: %s\n",
                    err ? err->message : "?");
            g_clear_error(&err);
        } else {
            g_print("[WK21] extension parsed: mv=%.0f name=%s ver=%s "
                    "supportsV2=%d supportsV3=%d perms=%u bg=%d "
                    "inj=%d opts=%d\n",
                    webkit_web_extension_get_manifest_version(ext),
                    webkit_web_extension_get_display_name(ext),
                    webkit_web_extension_get_display_version(ext),
                    webkit_web_extension_supports_manifest_version(
                        ext, 2.0),
                    webkit_web_extension_supports_manifest_version(
                        ext, 3.0),
                    g_strv_length((gchar **)
                        webkit_web_extension_get_requested_permissions(
                            ext)),
                    webkit_web_extension_get_has_background_content(ext),
                    webkit_web_extension_get_has_injected_content(ext),
                    webkit_web_extension_get_has_options_page(ext));
            g_object_unref(ext);
        }
        /* The web-extension-mode construct property — can a view be
         * put in extension mode at all without a manager? */
        GObjectClass *k = G_OBJECT_CLASS(
            g_type_class_ref(WEBKIT_TYPE_WEB_VIEW));
        const GParamSpec *p =
            g_object_class_find_property(k, "web-extension-mode");
        g_type_class_unref(k);
        g_print("[WK21] web-extension-mode prop=%s flags=0x%x\n",
                p ? "present" : "ABSENT",
                p ? (unsigned)p->flags : 0);
        if (p && (p->flags & G_PARAM_WRITABLE)) {
            Stack *se = stack_new(session);
            if (se->webView) {
                /* reconstruct with the mode set */
                g_clear_object(&se->webView);
                se->webView = WEBKIT_WEB_VIEW(g_object_new(
                    WEBKIT_TYPE_WEB_VIEW,
                    "display", se->display,
                    "network-session", session,
                    "web-extension-mode",
                        WEBKIT_WEB_EXTENSION_MODE_MANIFESTV3,
                    NULL));
                g_print("[WK21] ext-mode view mode=%d (want %d)\n",
                        webkit_web_view_get_web_extension_mode(
                            se->webView),
                        WEBKIT_WEB_EXTENSION_MODE_MANIFESTV3);
            }
            stack_free(se);
        }
    }

    stack_free(s1);
    webkit_network_proxy_settings_free(ps);
    g_object_unref(session);
    g_print("[WK21] probe done\n");
    /* WebKit teardown self-aborts on the plain-C path (WK02 trap b) —
     * _exit once the answer is printed. */
    fflush(stdout);
    _exit(0);
}
