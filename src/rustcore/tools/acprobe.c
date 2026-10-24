/*
 * acprobe.c — RC07 smoke probe for the arktos_core.h facade.
 *
 * A freestanding C consumer (no Qt, no Rust toolchain) that links
 * the static crate and exercises every module accessor plus the
 * lifecycle/notification surface — the "the header really is the
 * ABI" proof the task asks for.  It only ever writes under the
 * scratch root passed as argv[1] (or a fresh mkdtemp dir).
 *
 *   cc -I../include acprobe.c ../target/release/libarktos_rustcore.a \
 *      -ldl -lpthread -lm -o acprobe
 *   ./acprobe /tmp/scratch-root
 */

#include <arktos_core.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures;

/* AcBuffer is {data,len}, not NUL-terminated — substring check over
 * the raw span (portable memmem). */
static int buf_contains(const void *hay, size_t hlen, const char *needle)
{
    const size_t nlen = strlen(needle);
    const unsigned char *h = hay;
    size_t i;
    if (!hay || nlen == 0 || hlen < nlen)
        return 0;
    for (i = 0; i + nlen <= hlen; ++i)
        if (memcmp(h + i, needle, nlen) == 0)
            return 1;
    return 0;
}

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    msg);                                               \
            failures++;                                                 \
        }                                                               \
    } while (0)

static char *last_error_dup(void)
{
    char *e = ac_last_error();
    if (!e)
        return NULL;
    char *copy = strdup(e);
    ac_string_free(e);
    return copy;
}

/* (module, profile) change-callback accounting — the facade lane
 * fires on every store mutation. */
static int cb_hits;
static char cb_last_module[64];
static char cb_last_profile[64]; /* "" = NULL profile seen */

static void on_change(const char *module, const char *profile_id,
                      void *userdata)
{
    (void)userdata;
    cb_hits++;
    snprintf(cb_last_module, sizeof(cb_last_module), "%s",
             module ? module : "");
    snprintf(cb_last_profile, sizeof(cb_last_profile), "%s",
             profile_id ? profile_id : "<null>");
}

static void join_path(char *out, size_t n, const char *a, const char *b,
                      const char *c)
{
    snprintf(out, n, "%s/%s/%s", a, b, c);
}

/* ---- MB05 bridge leg --------------------------------------------------
 *
 * The --bridge mode: exercises the middleware end-to-end — the MB02
 * envelope (one query + one command dispatched per domain), the MB03
 * namespaced event lane (subscribe, provoke, verify, unsubscribe),
 * and the MB04 capability matrix.  Envelope framing is MW-ARCH §3 v1:
 * a 20-byte little-endian header + UTF-8 JSON payload.
 */

#define MW_TY_QUERY   1
#define MW_TY_COMMAND 2
#define MW_TY_OK      3
#define MW_TY_ERROR   4
#define MW_HEADER     20

static size_t mw_frame(unsigned char *buf, unsigned char type,
                       unsigned long long corr, unsigned char domain,
                       unsigned short op, const char *payload)
{
    const size_t plen = payload ? strlen(payload) : 0;
    int i;
    memcpy(buf, "AM", 2);
    buf[2] = 1; /* envelope schema version */
    buf[3] = type;
    for (i = 0; i < 8; ++i)
        buf[4 + i] = (unsigned char)((corr >> (8 * i)) & 0xff);
    buf[12] = domain;
    buf[13] = 0; /* flags */
    buf[14] = (unsigned char)(op & 0xff);
    buf[15] = (unsigned char)((op >> 8) & 0xff);
    for (i = 0; i < 4; ++i)
        buf[16 + i] = (unsigned char)((plen >> (8 * i)) & 0xff);
    if (plen)
        memcpy(buf + MW_HEADER, payload, plen);
    return MW_HEADER + plen;
}

typedef struct MwResp {
    int type;              /* MW_TY_OK / MW_TY_ERROR */
    unsigned long long corr;
    int domain;
    int op;
    AcBuffer body;         /* JSON; free via ac_buffer_free */
} MwResp;

/* One dispatch round-trip: builds the request frame, calls
 * ac_mw_dispatch, parses the response header.  Returns 0 on a parsed
 * envelope (either type), -1 on a bare transport failure. */
static int mw_call(AcCore *core, unsigned char type,
                   unsigned long long corr, unsigned char domain,
                   unsigned short op, const char *payload, MwResp *r)
{
    unsigned char req[8192];
    size_t n = mw_frame(req, type, corr, domain, op, payload);
    AcBuffer out = {NULL, 0};
    memset(r, 0, sizeof(*r));
    if (ac_mw_dispatch(core, req, n, &out) != AC_OK)
        return -1;
    CHECK(out.len >= MW_HEADER && memcmp(out.data, "AM", 2) == 0,
          "response is not an envelope");
    r->type = out.data[3];
    r->corr = 0;
    {
        int i;
        for (i = 0; i < 8; ++i)
            r->corr |= (unsigned long long)out.data[4 + i] << (8 * i);
    }
    r->domain = out.data[12];
    r->op = out.data[14] | (out.data[15] << 8);
    /* Move the payload span into its own buffer. */
    r->body.len = out.len - MW_HEADER;
    r->body.data = malloc(r->body.len ? r->body.len : 1);
    if (r->body.len)
        memcpy(r->body.data, (const unsigned char *)out.data + MW_HEADER,
               r->body.len);
    ac_buffer_free(out);
    return 0;
}

static void mw_resp_free(MwResp *r)
{
    free(r->body.data);
    r->body.data = NULL;
    r->body.len = 0;
}

static int resp_ok(const MwResp *r)
{
    return r->type == MW_TY_OK;
}

static int resp_err_code_is(const MwResp *r, const char *code)
{
    char needle[80];
    snprintf(needle, sizeof(needle), "\"code\":\"%s\"", code);
    return r->type == MW_TY_ERROR &&
           buf_contains(r->body.data, r->body.len, needle);
}

/* Namespaced-event callback accounting (ac_events_subscribe lane). */
static int ev_hits;
static char ev_last_topic[64];
static char ev_last_payload[512];

static void on_event(const char *topic, const char *payload_json,
                     void *userdata)
{
    (void)userdata;
    ev_hits++;
    snprintf(ev_last_topic, sizeof(ev_last_topic), "%s",
             topic ? topic : "");
    snprintf(ev_last_payload, sizeof(ev_last_payload), "%s",
             payload_json ? payload_json : "");
}

/* One table row per dispatched op — MB05's "at least one query +
 * one command per domain" spelled out, with the payload substring the
 * ok response must carry.  opname feeds the describe-advertised check. */
struct bridge_step {
    int domain;
    int op;
    int ty;
    const char *domain_name;
    const char *op_name;
    const char *payload;
    const char *expect;      /* substring in an ok body; NULL = just ok */
    const char *err_code;    /* expected error code; NULL = expect ok */
};

#define MAX_PAYLOAD 2048

static int run_bridge(const char *root)
{
    AcCore *core;
    AcConfig cfg;
    MwResp r;
    AcBuffer caps = {NULL, 0};
    unsigned long long corr = 0x1000;
    size_t i;

    /* Vault needs a staged source dir under the scratch root. */
    char vault_src[600], vault_out[600], vault_file[640];
    char payload[MAX_PAYLOAD];
    snprintf(vault_src, sizeof(vault_src), "%s/vault-src", root);
    snprintf(vault_out, sizeof(vault_out), "%s/vault.rv", root);
    snprintf(vault_file, sizeof(vault_file), "%s/f.txt", vault_src);
    mkdir(vault_src, 0700);
    {
        FILE *f = fopen(vault_file, "wb");
        if (f) {
            fputs("payload", f);
            fclose(f);
        }
    }
    snprintf(payload, sizeof(payload),
             "{\"dir\":\"%s\",\"out\":\"%s\","
             "\"pass_b64\":\"cHJvYmUtdmF1bHQtcGFzcw==\",\"algo\":1}",
             vault_src, vault_out);
    char vault_seal[MAX_PAYLOAD];
    snprintf(vault_seal, sizeof(vault_seal), "%s", payload);
    snprintf(payload, sizeof(payload),
             "{\"path\":\"%s\"}", vault_out);
    char vault_inspect[MAX_PAYLOAD];
    snprintf(vault_inspect, sizeof(vault_inspect), "%s", payload);
    snprintf(payload, sizeof(payload),
             "{\"path\":\"%s\",\"pass_b64\":\"cHJvYmUtdmF1bHQtcGFzcw==\"}",
             vault_out);
    char vault_verify[MAX_PAYLOAD];
    snprintf(vault_verify, sizeof(vault_verify), "%s", payload);

    /* One query + one command per domain.  Domains with no envelope
     * surface BY DESIGN (tor, bidi — MW-ARCH §2.3 keeps the control
     * link and the BiDi client flat) get probe ops that must answer
     * unknown_op envelopes, not crashes or Ok.  Query-only extverify
     * gets its command demand as a class-mismatch check. */
    const struct bridge_step steps[] = {
        /* mw — the meta domain itself. */
        {0, 0x0001, MW_TY_QUERY, "mw", "describe", "",
         "\"envelope_version\":1", NULL},
        {0, 0x0002, MW_TY_QUERY, "mw", "version", "",
         "\"api_version\":", NULL},
        {0, 0x0003, MW_TY_QUERY, "mw", "ping", "{\"x\":1}",
         "\"pong\":true", NULL},
        /* emit is also the second event provoke (history/ filter). */
        {0, 0x0004, MW_TY_COMMAND, "mw", "emit",
         "{\"topic\":\"history/changed\",\"fields\":{\"op\":\"visit\"}}",
         "\"ok\":true", NULL},
        /* settings — the proving-slice domain. */
        {1, 0x0004, MW_TY_COMMAND, "settings", "set",
         "{\"key\":\"MWBridge/probe\",\"value\":{\"t\":\"s\",\"v\":\"x\"}}",
         "\"ok\":true", NULL},
        {1, 0x0001, MW_TY_QUERY, "settings", "get",
         "{\"key\":\"MWBridge/probe\"}", "\"v\":\"x\"", NULL},
        /* profiles — the persona registry. */
        {2, 0x0001, MW_TY_QUERY, "profiles", "list", "",
         "\"default\"", NULL},
        {2, 0x0002, MW_TY_COMMAND, "profiles", "reload", "",
         "\"ok\":true", NULL},
        /* history — the add also provokes history/changed. */
        {3, 0x0001, MW_TY_QUERY, "history", "count",
         "{\"profile\":\"probe\"}", "\"count\":0", NULL},
        {3, 0x0002, MW_TY_COMMAND, "history", "add",
         "{\"profile\":\"probe\",\"url\":\"https://probe/\","
         "\"title\":\"P\",\"ts_ms\":1}",
         "\"ok\":true", NULL},
        {3, 0x0003, MW_TY_QUERY, "history", "query",
         "{\"profile\":\"probe\",\"spec\":{\"limit\":10}}",
         "https://probe/", NULL},
        /* bookmarks. */
        {4, 0x0001, MW_TY_QUERY, "bookmarks", "get",
         "{\"profile\":\"probe\"}", "\"root\":", NULL},
        {4, 0x0002, MW_TY_COMMAND, "bookmarks", "create",
         "{\"profile\":\"probe\",\"node\":{\"type\":2,"
         "\"title\":\"P\",\"url\":\"https://p/\"}}",
         "\"id\":", NULL},
        /* policy — the sitedecisions slice. */
        {5, 0x0002, MW_TY_COMMAND, "policy", "sitedec_set",
         "{\"profile\":\"probe\",\"kind\":\"zoom\","
         "\"key\":\"example.com\",\"value\":\"125\"}",
         "\"changed\":true", NULL},
        {5, 0x0001, MW_TY_QUERY, "policy", "sitedec_get",
         "{\"profile\":\"probe\",\"kind\":\"zoom\","
         "\"key\":\"example.com\"}",
         "\"value\":\"125\"", NULL},
        /* session. */
        {6, 0x0003, MW_TY_COMMAND, "session", "save",
         "{\"profile\":\"probe\",\"session\":{\"version\":1,"
         "\"windows\":[{\"shell\":\"\",\"current\":-1,"
         "\"engine\":\"webengine\",\"tabs\":[]}]}}",
         "\"ok\":true", NULL},
        {6, 0x0001, MW_TY_QUERY, "session", "exists",
         "{\"profile\":\"probe\"}", "\"exists\":true", NULL},
        /* cookies. */
        {7, 0x0003, MW_TY_COMMAND, "cookies", "set",
         "{\"profile\":\"probe\",\"record\":{\"name\":\"s\","
         "\"value\":\"v\",\"domain\":\"example.com\",\"path\":\"/\"}}",
         "\"stored\"", NULL},
        {7, 0x0001, MW_TY_QUERY, "cookies", "count",
         "{\"profile\":\"probe\"}", "\"count\":1", NULL},
        /* icons — bytes cross the envelope base64'd. */
        {8, 0x0002, MW_TY_COMMAND, "icons", "put",
         "{\"profile\":\"probe\",\"url\":\"https://a/\","
         "\"png_b64\":\"iVBORy1mYWtl\"}",
         "\"changed\":true", NULL},
        {8, 0x0001, MW_TY_QUERY, "icons", "count",
         "{\"profile\":\"probe\"}", "\"count\":1", NULL},
        /* containers. */
        {9, 0x0002, MW_TY_COMMAND, "containers", "create",
         "{\"profile\":\"probe\",\"name\":\"Work\",\"color\":\"blue\"}",
         "\"id\":", NULL},
        {9, 0x0001, MW_TY_QUERY, "containers", "list",
         "{\"profile\":\"probe\"}", "\"Work\"", NULL},
        /* notes. */
        {10, 0x0003, MW_TY_COMMAND, "notes", "insert",
         "{\"profile\":\"probe\",\"row\":-1,\"title\":\"T\",\"body\":\"B\"}",
         "\"ok\":true", NULL},
        {10, 0x0001, MW_TY_QUERY, "notes", "count",
         "{\"profile\":\"probe\"}", "\"count\":1", NULL},
        /* custody — file-key credential map, no passphrase needed. */
        {11, 0x0001, MW_TY_QUERY, "custody", "state",
         "{\"profile\":\"probe\"}", "\"available\":", NULL},
        {11, 0x0003, MW_TY_COMMAND, "custody", "cred_put",
         "{\"profile\":\"probe\",\"name\":\"net/pw\","
         "\"value_b64\":\"c2Vrcml0\"}",
         "\"changed\":true", NULL},
        /* opensearch — the data-root-global engine registry. */
        {12, 0x0002, MW_TY_COMMAND, "opensearch", "put",
         "{\"engine\":{\"name\":\"ProbeSE\",\"search\":"
         "{\"template\":\"https://se/?q={searchTerms}\"}}}",
         "\"changed\":true", NULL},
        {12, 0x0001, MW_TY_QUERY, "opensearch", "list", "",
         "\"ProbeSE\"", NULL},
        /* updates — global-scope policy state. */
        {13, 0x0002, MW_TY_COMMAND, "updates", "record_check",
         "{\"now_ms\":1234}", "\"ok\":true", NULL},
        {13, 0x0001, MW_TY_QUERY, "updates", "state", "",
         "\"last_check_ms\":1234", NULL},
        /* services — classify (query) + the live-channel tls_probe
         * (command; a refused port still answers an Ok verdict). */
        {14, 0x0001, MW_TY_QUERY, "services", "classify_input",
         "{\"input\":\"https://x/\"}", "\"navigate\"", NULL},
        {14, 0x0002, MW_TY_COMMAND, "services", "tls_probe",
         "{\"host\":\"127.0.0.1\",\"port\":1}", NULL, NULL},
        /* extverify — query-only by design; the command demand must
         * come back as a class-mismatch envelope. */
        {15, 0x0001, MW_TY_QUERY, "extverify", "manifest_check",
         "{\"data_b64\":"
         "\"eyJtYW5pZmVzdF92ZXJzaW9uIjozLCJuYW1lIjoicHJvYmUiLCJ2ZXJzaW9uIjoiMS4wIn0=\"}",
         "\"manifest_version\":3", NULL},
        {15, 0x0001, MW_TY_COMMAND, "extverify", "manifest_check",
         "{\"data_b64\":\"AA==\"}", NULL, "malformed"},
        /* vault — explicit paths, sealed under the probe passphrase. */
        {16, 0x0002, MW_TY_COMMAND, "vault", "seal", NULL,
         "\"ok\":true", NULL},
        {16, 0x0001, MW_TY_QUERY, "vault", "inspect", NULL,
         "\"format\":\"ARV1\"", NULL},
        {16, 0x0003, MW_TY_QUERY, "vault", "verify", NULL,
         "\"ok\":true", NULL},
        /* tor + bidi — keep-flat domains: probes must answer
         * unknown_op envelopes (the registered-name boundary). */
        {17, 0x0001, MW_TY_QUERY, "tor", "", "", NULL, "unknown_op"},
        {17, 0x0001, MW_TY_COMMAND, "tor", "", "", NULL, "unknown_op"},
        {18, 0x0001, MW_TY_QUERY, "bidi", "", "", NULL, "unknown_op"},
        {18, 0x0001, MW_TY_COMMAND, "bidi", "", "", NULL, "unknown_op"},
    };
    const char *step_payloads[3] = {vault_seal, vault_inspect,
                                    vault_verify};
    int vault_step = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.api_version = AC_API_VERSION;
    cfg.data_root = root;
    core = ac_init(&cfg);
    CHECK(core != NULL, "ac_init for bridge leg");
    if (!core)
        return ++failures;

    CHECK(ac_mw_version() == 1, "ac_mw_version == 1");

    /* MB03: subscribe on the namespaced lane BEFORE the sweep — the
     * mw.emit and history.add steps below are the provokes. */
    {
        uint64_t sub = ac_events_subscribe(core, "history/", on_event,
                                           NULL);
        CHECK(sub != 0, "ac_events_subscribe history/");

        ev_hits = 0;
        ev_last_topic[0] = 0;
        ev_last_payload[0] = 0;

        for (i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
            const struct bridge_step *s = &steps[i];
            const char *pl = s->payload;
            char tag[96];
            ++corr;
            if (!pl && vault_step < 3)
                pl = step_payloads[vault_step++];
            snprintf(tag, sizeof(tag), "%s.%s", s->domain_name,
                     s->op_name);
            if (mw_call(core, (unsigned char)s->ty, corr,
                        (unsigned char)s->domain,
                        (unsigned short)s->op, pl, &r) != 0) {
                fprintf(stderr, "FAIL: %s transport failure\n", tag);
                failures++;
                continue;
            }
            CHECK(r.corr == corr, "correlation id not echoed");
            CHECK(r.domain == s->domain && r.op == s->op,
                  "response domain/op not echoed");
            if (s->err_code) {
                CHECK(resp_err_code_is(&r, s->err_code),
                      tag);
            } else {
                if (!resp_ok(&r)) {
                    fprintf(stderr,
                            "FAIL: %s answered error: %.*s\n", tag,
                            (int)r.body.len, (const char *)r.body.data);
                    failures++;
                }
                if (s->expect &&
                    !buf_contains(r.body.data, r.body.len, s->expect)) {
                    fprintf(stderr,
                            "FAIL: %s missing %s in %.*s\n", tag,
                            s->expect, (int)r.body.len,
                            (const char *)r.body.data);
                    failures++;
                }
            }
            /* The describe response is also the advertised-ops
             * cross-check, captured when the mw.describe step runs. */
            if (s->domain == 0 && s->op == 0x0001 && resp_ok(&r)) {
                size_t k;
                for (k = 0;
                     k < sizeof(steps) / sizeof(steps[0]); ++k) {
                    char needle[96];
                    if (!steps[k].op_name[0])
                        continue;
                    snprintf(needle, sizeof(needle),
                             "\"name\":\"%s\"", steps[k].op_name);
                    if (!buf_contains(r.body.data, r.body.len, needle)) {
                        fprintf(stderr,
                                "FAIL: describe omits %s.%s\n",
                                steps[k].domain_name,
                                steps[k].op_name);
                        failures++;
                    }
                }
            }
            mw_resp_free(&r);
        }

        /* The two provokes landed: mw.emit then history.add — both
         * under the "history/" namespace filter. */
        CHECK(ev_hits == 2, "expected 2 namespaced events");
        CHECK(strcmp(ev_last_topic, "history/changed") == 0,
              "event topic is history/changed");
        CHECK(strstr(ev_last_payload, "\"topic\":\"history/changed\"") &&
                  strstr(ev_last_payload, "\"profile\":\"probe\"") &&
                  strstr(ev_last_payload, "\"op\":\"visit\""),
              "history/changed payload carries v+topic+profile+op");

        /* The topic registry reports produced+seen honestly. */
        {
            char *topics = ac_events_topics();
            CHECK(topics != NULL, "ac_events_topics");
            if (topics) {
                CHECK(strstr(topics, "\"topic\":\"history/changed\"") &&
                          strstr(topics, "\"produced\":true") &&
                          strstr(topics, "\"seen\":true"),
                      "topic registry marks history/changed seen");
                ac_string_free(topics);
            }
        }

        /* Unsubscribe, then re-provoke — the lane must go quiet. */
        CHECK(ac_events_unsubscribe(core, sub) == AC_OK,
              "ac_events_unsubscribe");
        CHECK(ac_events_unsubscribe(core, sub) == AC_ERR_NOT_FOUND,
              "double unsubscribe is NotFound");
        {
            int hits_before = ev_hits;
            if (mw_call(core, MW_TY_COMMAND, ++corr, 0, 0x0004,
                        "{\"topic\":\"history/changed\",\"fields\":{\"op\":\"visit\"}}",
                        &r) == 0) {
                CHECK(resp_ok(&r), "post-unsubscribe emit ok");
                mw_resp_free(&r);
            }
            CHECK(ev_hits == hits_before,
                  "no events after unsubscribe");
        }
    }

    /* Envelope fault lanes — a malformed frame and an unregistered
     * domain still answer in-band envelopes with corr echoes. */
    {
        unsigned char bad[32];
        memset(bad, 0, sizeof(bad));
        memcpy(bad, "XX", 2); /* bad magic */
        {
            AcBuffer out = {NULL, 0};
            CHECK(ac_mw_dispatch(core, bad, sizeof(bad), &out) == AC_OK,
                  "bad-magic frame still returns an envelope");
            if (out.data) {
                CHECK(out.data[3] == MW_TY_ERROR &&
                          buf_contains(out.data, out.len, "\"malformed\""),
                      "bad magic is a malformed envelope");
                ac_buffer_free(out);
            }
        }
        if (mw_call(core, MW_TY_QUERY, ++corr, 99, 0x0001, "", &r) == 0) {
            CHECK(r.corr == corr, "unknown_domain echoes corr");
            CHECK(resp_err_code_is(&r, "unknown_domain"),
                  "domain 99 is unknown_domain");
            mw_resp_free(&r);
        }
    }

    /* MB04: the capability matrix, the alias, and a refused id. */
    CHECK(ac_engine_caps(NULL, &caps) == AC_OK, "caps matrix");
    CHECK(buf_contains(caps.data, caps.len, "\"qtwebengine\"") &&
              buf_contains(caps.data, caps.len, "\"servo\"") &&
              buf_contains(caps.data, caps.len, "\"webkit\"") &&
              buf_contains(caps.data, caps.len, "\"features\"") &&
              buf_contains(caps.data, caps.len, "\"state\""),
          "matrix covers all three engines");
    ac_buffer_free(caps);
    caps.data = NULL;
    caps.len = 0;
    /* The Qt backend spelling resolves as the qtwebengine alias. */
    CHECK(ac_engine_caps("webengine", &caps) == AC_OK &&
              buf_contains(caps.data, caps.len, "\"qtwebengine\""),
          "webengine alias resolves");
    ac_buffer_free(caps);
    caps.data = NULL;
    caps.len = 0;
    CHECK(ac_engine_caps("nonexistent", &caps) == AC_ERR_INVALID,
          "unknown engine id refused");

    ac_shutdown(core);
    return failures;
}

int main(int argc, char **argv)
{
    char root[512];
#ifdef ACPROBE_BRIDGE_DEFAULT
    /* The autotest build runs the bridge leg without a flag — its
     * runner only passes Qt-test args like -silent. */
    int bridge = 1;
#else
    int bridge = 0;
#endif
    const char *arg_root = NULL;
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bridge") == 0)
            bridge = 1;
        else if (argv[i][0] != '-' && !arg_root)
            arg_root = argv[i];
        /* Unknown -flags are ignored: test runners pass their own. */
    }
    if (arg_root) {
        snprintf(root, sizeof(root), "%s", arg_root);
        mkdir(root, 0700);
    } else {
        snprintf(root, sizeof(root), "/tmp/acprobe-XXXXXX");
        if (!mkdtemp(root)) {
            perror("mkdtemp");
            return 2;
        }
    }
    fprintf(stderr, "acprobe: data root %s\n", root);

    /* --bridge runs only the middleware leg — the flat-facade sweep
     * below stays the default mode. */
    if (bridge) {
        int f = run_bridge(root);
        if (f == 0)
            fprintf(stderr, "acprobe --bridge: OK\n");
        else
            fprintf(stderr, "acprobe --bridge: %d FAILURES\n", f);
        return f ? 1 : 0;
    }

    /* ---- housekeeping -------------------------------------------- */
    CHECK(ac_api_version() == AC_API_VERSION, "api version mismatch");
    CHECK(ac_capabilities() ==
              (AC_CAP_SETTINGS | AC_CAP_PROFILES | AC_CAP_HISTORY |
               AC_CAP_BOOKMARKS | AC_CAP_POLICY | AC_CAP_SESSION |
               AC_CAP_COOKIES | AC_CAP_ICONS | AC_CAP_CONTAINERS |
               AC_CAP_NOTES | AC_CAP_DLPOLICY | AC_CAP_PRIVACY |
               AC_CAP_URLCLEAN),
          "capabilities bitmask incomplete");

    CHECK(ac_init(NULL) == NULL, "ac_init(NULL) must fail");
    {
        char *err = last_error_dup();
        CHECK(err && err[0], "NULL config must stash an error");
        free(err);
    }
    AcConfig bad = {0};
    bad.struct_size = 4; /* shorter than v1 */
    bad.api_version = AC_API_VERSION;
    bad.data_root = root;
    CHECK(ac_init(&bad) == NULL, "undersized AcConfig must fail");

    AcConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg);
    cfg.api_version = AC_API_VERSION;
    cfg.data_root = root;
    cfg.app_name = "acprobe";
    cfg.default_profile_name = "Probe Person";
    AcCore *core = ac_init(&cfg);
    CHECK(core != NULL, "ac_init failed");
    if (!core)
        return 2;
    ac_set_change_callback(core, on_change, NULL);

    /* ---- engine capability matrix (MB04) -------------------------
     * Static registry — needs no core.  Each engine row prints; the
     * checks pin the schema and the honesty grades that must never
     * silently upgrade (Bitwarden-class extension support stays
     * "partial", servo's devtools stay off). */
    {
        static const char *ids[] = {
            "qtwebengine", "servo", "webkit"
        };
        size_t i;
        for (i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
            AcBuffer out = {0};
            CHECK(ac_engine_caps(ids[i], &out) == AC_OK,
                  "ac_engine_caps known id");
            if (out.data) {
                fprintf(stderr, "caps %s: %.*s\n", ids[i],
                        (int)out.len, (const char *)out.data);
                CHECK(buf_contains(out.data, out.len,
                                   "\"devtools\"") &&
                          buf_contains(out.data, out.len,
                                       "\"autofill\""),
                      "row carries the required feature keys");
                ac_buffer_free(out);
            }
        }
        /* backend->id() spelling resolves the same row. */
        {
            AcBuffer a = {0}, b = {0};
            CHECK(ac_engine_caps("webengine", &a) == AC_OK,
                  "webengine alias");
            CHECK(ac_engine_caps("qtwebengine", &b) == AC_OK,
                  "qtwebengine canonical");
            CHECK(a.len == b.len &&
                      memcmp(a.data, b.data, a.len) == 0,
                  "alias resolves the same document");
            ac_buffer_free(a);
            ac_buffer_free(b);
        }
        /* Honesty pins — these grades must never fake parity.  The
         * servo devtools note is the fingerprint that the row stays
         * graded off with the reason attached. */
        {
            AcBuffer out = {0};
            CHECK(ac_engine_caps("servo", &out) == AC_OK &&
                      buf_contains(out.data, out.len, "devtools") &&
                      buf_contains(out.data, out.len,
                                   "Firefox remote protocol"),
                  "servo devtools honestly unsupported");
            if (out.data)
                ac_buffer_free(out);
        }
        /* NULL emits the whole matrix; unknown ids fail clean. */
        {
            AcBuffer out = {0};
            CHECK(ac_engine_caps(NULL, &out) == AC_OK &&
                      buf_contains(out.data, out.len,
                                   "\"qtwebengine\"") &&
                      buf_contains(out.data, out.len, "\"servo\"") &&
                      buf_contains(out.data, out.len, "\"webkit\""),
                  "matrix covers every engine");
            if (out.data)
                ac_buffer_free(out);
            CHECK(ac_engine_caps("gecko", &out) == AC_ERR_INVALID,
                  "unknown engine id refused");
            CHECK(out.data == NULL, "failure leaves no buffer");
            CHECK(ac_engine_caps("qtwebengine", NULL) ==
                      AC_ERR_INVALID,
                  "NULL out refused");
        }
    }

    /* ---- settings (RC02) ----------------------------------------- */
    AcSettings *gs = ac_settings(core, NULL);
    AcSettings *ps = ac_settings(core, "work");
    CHECK(gs && ps, "settings accessors failed");
    CHECK(ac_settings_set_string(ps, "probe/key", "value") == AC_OK,
          "ac_settings_set_string");
    {
        AcBuffer out = {0};
        CHECK(ac_settings_get_string(ps, "probe/key", &out) == AC_OK,
              "ac_settings_get_string");
        /* AcBuffer is {data,len}, not NUL-terminated — get_string
         * unwraps the typed envelope to the raw bytes. */
        CHECK(out.len == 5 && memcmp(out.data, "value", 5) == 0,
              "settings value roundtrip");
        ac_buffer_free(out);
    }
    CHECK(ac_settings_set_bool(ps, "probe/flag", 1) == AC_OK,
          "ac_settings_set_bool");
    CHECK(ac_settings_contains(ps, "probe/flag") == 1,
          "ac_settings_contains");
    {
        char *keys = ac_settings_list(ps, "probe/");
        CHECK(keys && strstr(keys, "probe/key"), "ac_settings_list");
        ac_string_free(keys);
    }
    CHECK(ac_settings_remove(ps, "probe/flag") == AC_OK,
          "ac_settings_remove");
    CHECK(ac_settings_sync(ps) == AC_OK, "ac_settings_sync");
    /* global scope is a different store */
    CHECK(ac_settings_contains(gs, "probe/key") == 0,
          "global scope must not see profile keys");

    /* ---- profiles (RC03) ------------------------------------------ */
    AcProfiles *pr = ac_profiles(core);
    CHECK(pr != NULL, "ac_profiles failed");
    {
        char *list = ac_profiles_list(pr);
        CHECK(list && strstr(list, "\"id\":\"default\""),
              "profiles list must carry the default row");
        if (list)
            ac_string_free(list);
        char *id = ac_profiles_slugify("Probe Person");
        CHECK(id && strcmp(id, "probe-person") == 0,
              "ac_profiles_slugify");
        if (id)
            ac_string_free(id);
        char *dir = ac_profiles_dir(pr, "work");
        char expect[1024];
        snprintf(expect, sizeof(expect), "%s/profiles/work", root);
        CHECK(dir && strcmp(dir, expect) == 0, "ac_profiles_dir");
        if (dir)
            ac_string_free(dir);
    }

    /* ---- history (RC04) -------------------------------------------- */
    AcHistory *h = ac_history(core, "work");
    CHECK(h != NULL, "ac_history failed");
    CHECK(ac_history_exists(core, "work") == 1,
          "ac_history_exists after open");
    CHECK(ac_history_exists(core, "nobody") == 0,
          "ac_history_exists for unopened profile");
    CHECK(ac_hist_add(h, "https://probe.example/", "Probe", 1000) ==
              AC_OK,
          "ac_hist_add");
    CHECK(ac_hist_count(h) == 1, "ac_hist_count");
    {
        char *row = ac_hist_entry_at(h, 0);
        CHECK(row && strstr(row, "probe.example"), "ac_hist_entry_at");
        if (row)
            ac_string_free(row);
        char *sug = ac_hist_suggest(h, "probe", 5);
        CHECK(sug && strstr(sug, "probe.example"), "ac_hist_suggest");
        if (sug)
            ac_string_free(sug);
        char *q = ac_hist_query(h, "{}");
        CHECK(q && strstr(q, "probe.example"), "ac_hist_query");
        if (q)
            ac_string_free(q);
    }
    CHECK(cb_hits > 0 && strcmp(cb_last_profile, "work") == 0,
          "facade callback must stamp the bound profile");

    /* ---- bookmarks (RC05) ------------------------------------------- */
    AcBookmarks *b = ac_bookmarks(core, "work");
    CHECK(b != NULL, "ac_bookmarks failed");
    {
        uint64_t r = ac_bm_root(b);
        CHECK(r != 0, "ac_bm_root");
        uint64_t n = ac_bm_create(
            b, r, -1,
            "{\"title\":\"P\",\"url\":\"https://bm.example/\"}");
        CHECK(n != 0, "ac_bm_create");
        CHECK(ac_bm_find(b, "https://bm.example/") == n, "ac_bm_find");
        CHECK(ac_bm_child_count(b, r) == 1, "ac_bm_child_count");
        CHECK(ac_bm_child_at(b, r, 0) == n, "ac_bm_child_at");
        CHECK(ac_bm_parent(b, n) == r, "ac_bm_parent");
        char *g = ac_bm_get(b, n);
        CHECK(g && strstr(g, "bm.example"), "ac_bm_get");
        if (g)
            ac_string_free(g);
        CHECK(ac_bm_set_desc(b, n, "probe") == AC_OK, "ac_bm_set_desc");
        CHECK(ac_bm_save(b, NULL) == AC_OK, "ac_bm_save bound file");
        {
            char xbel[512];
            struct stat st;
            join_path(xbel, sizeof(xbel), root, "profiles/work",
                      "bookmarks.xbel");
            CHECK(stat(xbel, &st) == 0 && st.st_size > 0,
                  "bookmarks.xbel written");
        }
        CHECK(ac_bm_reload(b) == AC_OK, "ac_bm_reload");
        CHECK(ac_bm_find(b, "https://bm.example/") != 0,
              "reloaded tree keeps the node");
    }

    /* ---- policy (RC06) ----------------------------------------------- */
    AcPolicy *p = ac_policy(core, "work");
    CHECK(p != NULL, "ac_policy failed");
    CHECK(ac_sitedec_set(p, "cookie", "probe.example", "block") ==
              AC_OK,
          "ac_sitedec_set");
    {
        AcBuffer out = {0};
        CHECK(ac_sitedec_get(p, "cookie", "probe.example", &out) ==
                  AC_OK,
              "ac_sitedec_get");
        if (out.data)
            ac_buffer_free(out);
        char *look = ac_sitedec_lookup(p, "cookie", "probe.example");
        CHECK(look && strstr(look, "block"), "ac_sitedec_lookup");
        if (look)
            ac_string_free(look);
        char *snap = ac_sitedec_snapshot(p);
        CHECK(snap && strstr(snap, "probe.example"),
              "ac_sitedec_snapshot");
        if (snap)
            ac_string_free(snap);
    }
    CHECK(ac_policy_eval_generation() > 0, "ac_policy_eval_generation");
    CHECK(ac_blocklist_check(p, "unrelated.example") == 0,
          "ac_blocklist_check");
    (void)ac_blocklist_count(p); /* builtin set size varies */
    (void)ac_tranco_count(p);
    CHECK(ac_ua_spoof_set(p, "ua.example", "ProbeUA/1.0") == AC_OK,
          "ac_ua_spoof_set");
    {
        char *ua = ac_ua_spoof(p, "ua.example");
        CHECK(ua && strcmp(ua, "ProbeUA/1.0") == 0, "ac_ua_spoof");
        if (ua)
            ac_string_free(ua);
        CHECK(ac_ua_spoof_remove(p, "ua.example") == AC_OK,
              "ac_ua_spoof_remove");
    }
    {
        char *st = ac_sitetrust_lookup(p, "probe.example", 1);
        CHECK(st && strstr(st, "\"level\""), "ac_sitetrust_lookup");
        if (st)
            ac_string_free(st);
    }

    /* ---- navpolicy popup gate (MD09) ------------------------------------
     * ac_nav_call forwards rc_nav_call under the policy handle's
     * profile anchor; the verdict JSON is the gate's contract:
     * default deny on an unknown http(s) opener, session allow,
     * clear back to deny, persistent allow via the anchored
     * sitedecisions "popup" rows, remove back to deny. */
    {
        static const char decide[] =
            "{\"op\":\"popup_decide\",\"url\":"
            "\"https://probe-pop.example/\",\"shaped\":true,"
            "\"enabled\":true}";
        static const char allow_s[] =
            "{\"op\":\"popup_allow\",\"host\":\"probe-pop.example\","
            "\"persistent\":false}";
        static const char allow_p[] =
            "{\"op\":\"popup_allow\",\"host\":\"probe-pop.example\","
            "\"persistent\":true}";
        static const char is_allowed[] =
            "{\"op\":\"popup_is_allowed\",\"host\":\"probe-pop.example\"}";
        static const char clear_s[] = "{\"op\":\"popup_clear_session\"}";
        static const char remove_p[] =
            "{\"op\":\"popup_remove\",\"host\":\"probe-pop.example\"}";
        char *v = ac_nav_call(p, (const uint8_t *)decide,
                              sizeof(decide) - 1);
        CHECK(v && strstr(v, "\"block\""), "popup default-block");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)allow_s, sizeof(allow_s) - 1);
        CHECK(v != NULL, "popup_allow session");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)is_allowed,
                        sizeof(is_allowed) - 1);
        CHECK(v && strstr(v, "\"allowed\":true"),
              "popup session-allowed");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)decide, sizeof(decide) - 1);
        CHECK(v && strstr(v, "\"allow\""), "popup session allow");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)clear_s, sizeof(clear_s) - 1);
        CHECK(v != NULL, "popup_clear_session");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)decide, sizeof(decide) - 1);
        CHECK(v && strstr(v, "\"block\""),
              "popup clear restores deny");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)allow_p, sizeof(allow_p) - 1);
        CHECK(v != NULL, "popup_allow persistent");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)decide, sizeof(decide) - 1);
        CHECK(v && strstr(v, "\"allow\""), "popup persisted allow");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)remove_p, sizeof(remove_p) - 1);
        CHECK(v != NULL, "popup_remove");
        if (v)
            ac_string_free(v);
        v = ac_nav_call(p, (const uint8_t *)decide, sizeof(decide) - 1);
        CHECK(v && strstr(v, "\"block\""),
              "popup remove restores deny");
        if (v)
            ac_string_free(v);
    }

    /* ---- session (RC13) ------------------------------------------------ */
    AcSession *sess = ac_session(core, "work");
    AcSession *sess_default = ac_session(core, NULL);
    CHECK(sess && sess_default, "ac_session accessors failed");
    {
        static const char manifest[] =
            "{\"version\":1,\"windows\":[{\"shell\":\"AAAA\","
            "\"current\":0,\"profile\":\"work\",\"tabs\":[{\"url\":"
            "\"https://probe.example/\",\"engine\":\"webengine\","
            "\"state\":\"\",\"scroll\":{\"x\":0,\"y\":640}}]}]}";
        CHECK(ac_session_exists(sess) == 0, "no session.dat yet");
        CHECK(ac_session_save(sess, (const uint8_t *)manifest,
                              sizeof(manifest) - 1) == AC_OK,
              "ac_session_save");
        CHECK(ac_session_exists(sess) == 1, "session.dat exists");
        /* path-bound isolation: the default profile must not see
         * the work profile's session. */
        CHECK(ac_session_exists(sess_default) == 0,
              "default session must not see work's file");
        {
            AcBuffer out = {0};
            CHECK(ac_session_load(sess, &out) == AC_OK,
                  "ac_session_load");
            /* load() re-emits the canonical manifest — the content
             * round-trips, byte shape is the codec's own. */
            CHECK(out.data && out.len &&
                      buf_contains(out.data, out.len, "probe.example") &&
                      buf_contains(out.data, out.len, "\"webengine\"") &&
                      buf_contains(out.data, out.len, "\"scroll\""),
                  "session manifest roundtrip");
            if (out.data)
                ac_buffer_free(out);
            struct stat st;
            char file[512];
            join_path(file, sizeof(file), root, "profiles/work",
                      "session.dat");
            CHECK(stat(file, &st) == 0 && st.st_size > 0,
                  "session.dat written under profiles/work");
        }
        /* lifecycle: fresh dir reads clean; mark_start arms;
         * mark_terminated leaves the signal breadcrumb. */
        CHECK(ac_session_dirty(sess) == 0, "fresh dir reads clean");
        CHECK(ac_session_mark_start(sess) == 0,
              "first mark_start not dirty");
        CHECK(ac_session_dirty(sess) == 1, "marker armed");
        CHECK(ac_session_mark_terminated(sess, 15) == AC_OK,
              "ac_session_mark_terminated");
        {
            AcBuffer out = {0};
            CHECK(ac_session_last_exit(sess, &out) == AC_OK,
                  "ac_session_last_exit");
            /* AcBuffer is {data,len}, not NUL-terminated. */
            char crumb[128];
            const size_t clen =
                out.len < sizeof(crumb) - 1 ? out.len
                                            : sizeof(crumb) - 1;
            if (out.data)
                memcpy(crumb, out.data, clen);
            crumb[clen] = '\0';
            CHECK(strstr(crumb, "SIGTERM") != NULL,
                  "signal breadcrumb recorded");
            if (out.data)
                ac_buffer_free(out);
        }
        CHECK(ac_session_mark_start(sess) == 0,
              "cleaned marker not dirty on next start");
        CHECK(ac_session_crash_count(sess) == 0,
              "clean exit cleared crash count");
        CHECK(ac_session_set_restoring(sess, 1) == AC_OK &&
                  ac_session_restoring(sess) == 1,
              "restoring flag sets");
        CHECK(ac_session_set_restoring(sess, 0) == AC_OK &&
                  ac_session_restoring(sess) == 0,
              "restoring flag clears");
        CHECK(ac_session_clear(sess) == AC_OK, "ac_session_clear");
        CHECK(ac_session_exists(sess) == 0, "session.dat gone");
    }
    ac_session_close(sess);
    ac_session_close(sess_default);

    /* ---- cookies (RC15) ------------------------------------------------ */
    AcCookies *ck = ac_cookies(core, "work");
    AcCookies *ck_other = ac_cookies(core, "other");
    CHECK(ck && ck_other, "ac_cookies accessors failed");
    CHECK(ac_cookies_role(ck) == AC_COOKIES_AUTHORITATIVE,
          "fresh db declares the authoritative role");
    CHECK(ac_cookies_set_role(ck, AC_COOKIES_MIRROR) == AC_OK,
          "ac_cookies_set_role");
    CHECK(ac_cookies_role(ck) == AC_COOKIES_MIRROR,
          "role round-trips");
    {
        /* CRUD over the JSON record lane. */
        static const char rec[] =
            "{\"name\":\"sid\",\"value\":\"v1\","
            "\"domain\":\".probe.example\",\"path\":\"/\","
            "\"expires\":2000000000000,\"secure\":true,"
            "\"sameSite\":2}";
        CHECK(ac_cookies_set(ck, (const uint8_t *)rec,
                             sizeof(rec) - 1) == AC_OK,
              "ac_cookies_set");
        CHECK(ac_cookies_count(ck, NULL) == 1, "ac_cookies_count");
        AcBuffer out = {0};
        CHECK(ac_cookies_get(ck, "probe.example", "sid", "/", NULL,
                             &out) == AC_OK,
              "ac_cookies_get");
        CHECK(out.data &&
                  buf_contains(out.data, out.len, "\"value\":\"v1\""),
              "record roundtrip");
        if (out.data)
            ac_buffer_free(out);
        /* Send-match: https carries the secure cookie, http drops
         * it; a foreign domain sees nothing. */
        static const char req_https[] =
            "{\"url\":\"https://sub.probe.example/\"}";
        char *m = ac_cookies_for_url(
            ck, (const uint8_t *)req_https, sizeof(req_https) - 1);
        CHECK(m && strstr(m, "\"name\":\"sid\""),
              "secure cookie rides https");
        if (m)
            ac_string_free(m);
        static const char req_http[] =
            "{\"url\":\"http://sub.probe.example/\"}";
        m = ac_cookies_for_url(
            ck, (const uint8_t *)req_http, sizeof(req_http) - 1);
        CHECK(m && !strstr(m, "\"name\":\"sid\""),
              "secure cookie must not ride http");
        if (m)
            ac_string_free(m);
        static const char req_foreign[] =
            "{\"url\":\"https://elsewhere.invalid/\"}";
        m = ac_cookies_for_url(
            ck, (const uint8_t *)req_foreign,
            sizeof(req_foreign) - 1);
        CHECK(m && !strstr(m, "\"name\":\"sid\""),
              "foreign domain sees no rows");
        if (m)
            ac_string_free(m);
        /* Set-Cookie ingest + a list of the partition. */
        char *v = ac_cookies_store_header(
            ck, "https://probe.example/app/x",
            "tok=abc; Max-Age=3600; SameSite=Strict", NULL);
        CHECK(v && strstr(v, "\"stored\":true"),
              "ac_cookies_store_header");
        if (v)
            ac_string_free(v);
        char *list = ac_cookies_list(ck, NULL);
        CHECK(list && strstr(list, "\"name\":\"tok\""),
              "ac_cookies_list sees the header-stored row");
        if (list)
            ac_string_free(list);
        /* The gate consults the profile's sitedecisions.json — the
         * block rule the policy leg wrote above applies. */
        static const char gate_blocked[] = "{\"host\":\"probe.example\"}";
        char *g = ac_cookies_gate(
            ck, (const uint8_t *)gate_blocked,
            sizeof(gate_blocked) - 1);
        CHECK(g && strstr(g, "\"accept\":false"),
              "gate honors the block rule");
        if (g)
            ac_string_free(g);
        static const char gate_open[] = "{\"host\":\"open.example\"}";
        g = ac_cookies_gate(ck, (const uint8_t *)gate_open,
                            sizeof(gate_open) - 1);
        CHECK(g && strstr(g, "\"accept\":true"),
              "gate accepts an unruled host");
        if (g)
            ac_string_free(g);
        /* toRawForm export -> file -> import round-trip. */
        char exp_file[512], imp_file[512];
        join_path(exp_file, sizeof(exp_file), root, "", "cookies.txt");
        join_path(imp_file, sizeof(imp_file), root, "",
                  "cookies-in.txt");
        CHECK(ac_cookies_export(ck, exp_file, NULL) == AC_OK,
              "ac_cookies_export");
        {
            struct stat st;
            CHECK(stat(exp_file, &st) == 0 && st.st_size > 0,
                  "export file written");
            FILE *f = fopen(imp_file, "w");
            CHECK(f != NULL, "import fixture write");
            if (f) {
                fputs("imp=1; expires=Tue, 01-Jan-2030 00:00:00 GMT;"
                      " domain=.imp.example; path=/\n"
                      "noattr=1; path=/\n",
                      f);
                fclose(f);
            }
        }
        CHECK(ac_cookies_clear(ck_other, NULL) == AC_OK,
              "ac_cookies_clear other partition");
        {
            char *rep =
                ac_cookies_import(ck, imp_file, NULL);
            /* one Domain-carrying line lands; the no-domain line is
             * skipped (no request host to key it on). */
            CHECK(rep && strstr(rep, "\"imported\":1") &&
                      strstr(rep, "\"skipped\":1"),
                  "ac_cookies_import report");
            if (rep)
                ac_string_free(rep);
        }
        CHECK(ac_cookies_remove(ck, "probe.example", "sid", "/",
                                NULL) == AC_OK,
              "ac_cookies_remove");
        CHECK(ac_cookies_remove_for_host(ck, "probe.example",
                                         NULL) == AC_OK,
              "ac_cookies_remove_for_host");
        CHECK(ac_cookies_purge_expired(ck, 9999999999999LL) == AC_OK,
              "ac_cookies_purge_expired");
        CHECK(ac_cookies_purge_session(ck) == AC_OK,
              "ac_cookies_purge_session");
        CHECK(ac_cookies_clear(ck, NULL) == AC_OK, "ac_cookies_clear");
        CHECK(ac_cookies_count(ck, NULL) == 0, "store emptied");
    }
    /* profile isolation: work's rows never reach other's db. */
    CHECK(ac_cookies_count(ck_other, NULL) == 0,
          "other profile stays empty");
    ac_cookies_close(ck);
    ac_cookies_close(ck_other);

    /* ---- icons (RC16) -------------------------------------------------- */
    AcIcons *ic = ac_icons(core, "work");
    AcIcons *ic_other = ac_icons(core, "other");
    CHECK(ic && ic_other, "ac_icons accessors failed");
    {
        static const uint8_t png_a[] = "\x89PNG-probe-a";
        static const uint8_t png_b[] = "\x89PNG-probe-b";
        char *key = ac_icons_key(ic, "https://ICON.example/x");
        CHECK(key && strcmp(key, "icon.example") == 0,
              "icon key is the lowercase host");
        if (key)
            ac_string_free(key);
        CHECK(ac_icons_put(ic, "https://icon.example/a", png_a,
                           sizeof(png_a) - 1) == AC_OK,
              "ac_icons_put");
        /* dedup-by-host: a second page of the same host rewrites the
         * one record. */
        CHECK(ac_icons_put(ic, "https://icon.example/b", png_b,
                           sizeof(png_b) - 1) == AC_OK,
              "dedup put");
        CHECK(ac_icons_count(ic) == 1, "host dedup keeps one row");
        {
            AcBuffer out = {0};
            CHECK(ac_icons_get(ic, "https://icon.example/a",
                               &out) == AC_OK,
                  "ac_icons_get");
            CHECK(out.data && out.len == sizeof(png_b) - 1 &&
                      memcmp(out.data, png_b, out.len) == 0,
                  "latest write wins for the host");
            if (out.data)
                ac_buffer_free(out);
        }
        {
            /* Files land under profiles/<id>/cache/icons — the
             * disposable lane — and never the data root's icons/. */
            char p[512];
            struct stat st;
            join_path(p, sizeof(p), root, "profiles/work",
                      "cache/icons/icons.json");
            CHECK(stat(p, &st) == 0 && st.st_size > 0,
                  "index lives under the cache root");
            join_path(p, sizeof(p), root, "profiles/work",
                      "icons");
            CHECK(stat(p, &st) != 0,
                  "no icons/ dir under the data root");
            char *list = ac_icons_list(ic);
            CHECK(list && strstr(list, "\"key\":\"icon.example\""),
                  "ac_icons_list");
            if (list)
                ac_string_free(list);
        }
        /* The expiry sweep at a boundary newer than the record
         * drops it; 0 keeps it. */
        {
            int64_t removed = -1;
            CHECK(ac_icons_sweep(ic, 0, &removed) == AC_OK &&
                      removed == 0,
                  "sweep at epoch keeps the row");
            CHECK(ac_icons_sweep(ic, 9999999999999LL, &removed) ==
                          AC_OK &&
                      removed == 1,
                  "sweep drops the aged row");
            CHECK(ac_icons_count(ic) == 0, "swept store is empty");
        }
        CHECK(ac_icons_put(ic, "https://icon.example/a", png_a,
                           sizeof(png_a) - 1) == AC_OK,
              "re-put after sweep");
        CHECK(ac_icons_remove(ic, "https://icon.example/") == AC_OK,
              "ac_icons_remove");
        CHECK(ac_icons_count(ic) == 0, "remove empties the row");
        /* clear is the clearIconDatabase equivalent. */
        CHECK(ac_icons_put(ic, "https://icon.example/", png_a,
                           sizeof(png_a) - 1) == AC_OK,
              "put before clear");
        CHECK(ac_icons_clear(ic) == AC_OK, "ac_icons_clear");
        {
            /* The wipe removes the store wholesale — checked before
             * the count below, which lazily rebuilds the (empty)
             * index. */
            char p[512];
            struct stat st;
            join_path(p, sizeof(p), root, "profiles/work",
                      "cache/icons");
            CHECK(stat(p, &st) != 0, "clear removed the icons dir");
        }
        CHECK(ac_icons_count(ic) == 0, "clear emptied the cache");
    }
    /* profile isolation: work's icons never reach other's cache. */
    CHECK(ac_icons_count(ic_other) == 0, "other profile stays empty");
    ac_icons_close(ic);
    ac_icons_close(ic_other);

    /* ---- containers (RC19) ------------------------------------------
     * The registry lives in the bound profile's settings scope;
     * the namespace map is the storage-derivation engines consume. */
    AcContainers *cm = ac_containers(core, "work");
    CHECK(cm != NULL, "ac_containers failed");
    {
        static const char spec[] =
            "{\"name\":\"Probe Box\",\"color\":\"#112233\"}";
        char *row = ac_containers_create(
            cm, (const uint8_t *)spec, sizeof(spec) - 1);
        CHECK(row && strstr(row, "\"name\":\"Probe Box\""),
              "ac_containers_create");
        char id[64] = {0};
        if (row) {
            /* fish the minted uuid out of the row JSON */
            const char *k = strstr(row, "\"id\":\"");
            if (k)
                snprintf(id, sizeof(id), "%s", k + 6);
            char *e = strchr(id, '"');
            if (e)
                *e = '\0';
            ac_string_free(row);
        }
        CHECK(id[0], "create returns a minted id");
        char *list = ac_containers_list(cm);
        CHECK(list && strstr(list, "\"Probe Box\"") &&
                  strstr(list, "\"color\":\"#112233\""),
              "ac_containers_list");
        if (list)
            ac_string_free(list);
        char *ns = ac_containers_namespace(cm, id);
        CHECK(ns && strstr(ns, "arktos-container-work-") &&
                  strstr(ns, "\"cookie_partition\""),
              "ac_containers_namespace");
        if (ns)
            ac_string_free(ns);
        static const char patch[] = "{\"name\":\"Renamed\"}";
        CHECK(ac_containers_update(
                  cm, id, (const uint8_t *)patch,
                  sizeof(patch) - 1) == AC_OK,
              "ac_containers_update");
        char *got = ac_containers_get(cm, id);
        CHECK(got && strstr(got, "\"Renamed\""), "ac_containers_get");
        if (got)
            ac_string_free(got);
        CHECK(ac_containers_get(cm, "ghost") == NULL,
              "unregistered id is a clean miss");
        CHECK(ac_containers_remove(cm, id) == AC_OK,
              "ac_containers_remove");
        CHECK(ac_containers_remove(cm, id) == AC_ERR_NOT_FOUND,
              "second remove is NotFound");
    }
    ac_containers_close(cm);

    /* ---- notes (RC21) -------------------------------------------------
     * profiles/<id>/notes.json — the sidebar's ordered list, the
     * dirty+sync debounce contract carried over the ABI. */
    AcNotes *nt = ac_notes(core, "work");
    AcNotes *nt_other = ac_notes(core, "other");
    CHECK(nt && nt_other, "ac_notes accessors failed");
    CHECK(ac_notes_is_persistent(nt) == 1, "bound store persists");
    {
        CHECK(ac_notes_insert(nt, 9, "Probe note",
                              "first body") == 0,
              "ac_notes_insert clamps the row");
        CHECK(ac_notes_insert(nt, 0, "Lead", NULL) == 0,
              "insert at 0 + NULL body reads empty");
        CHECK(ac_notes_count(nt) == 2, "ac_notes_count");
        int changed = -1;
        CHECK(ac_notes_set_title(nt, 0, "Lead note", &changed) ==
                  AC_OK && changed == 1,
              "ac_notes_set_title changed");
        CHECK(ac_notes_set_title(nt, 0, "Lead note", &changed) ==
                  AC_OK && changed == 0,
              "identical title is a no-op (setData false)");
        CHECK(ac_notes_set_markdown(nt, 1, 1, &changed) == AC_OK &&
                  changed == 1,
              "ac_notes_set_markdown");
        CHECK(ac_notes_set_title(nt, 9, "x", &changed) ==
                  AC_ERR_NOT_FOUND,
              "out-of-range set is NotFound");
        {
            char *g = ac_notes_get(nt, 1);
            CHECK(g && strstr(g, "\"title\":\"Probe note\"") &&
                      strstr(g, "\"markdown\":true"),
                  "ac_notes_get");
            if (g)
                ac_string_free(g);
            char *l = ac_notes_list(nt);
            CHECK(l && strstr(l, "\"Lead note\""),
                  "ac_notes_list keeps the order");
            if (l)
                ac_string_free(l);
        }
        /* dirty until the debounced commit — then the file exists
         * under the bound profile's dir. */
        CHECK(ac_notes_is_dirty(nt) == 1, "mutations mark dirty");
        CHECK(ac_notes_sync(nt) == AC_OK, "ac_notes_sync");
        CHECK(ac_notes_is_dirty(nt) == 0, "sync clears dirty");
        {
            char p[512];
            struct stat st;
            join_path(p, sizeof(p), root, "profiles/work",
                      "notes.json");
            CHECK(stat(p, &st) == 0 && st.st_size > 0,
                  "notes.json written under profiles/work");
        }
        CHECK(ac_notes_remove(nt, 0, 1) == AC_OK,
              "ac_notes_remove");
        CHECK(ac_notes_remove(nt, -1, 1) == AC_ERR_INVALID,
              "refused range is Invalid");
        CHECK(ac_notes_count(nt) == 1, "count after remove");
    }
    /* profile isolation: work's notes never reach other's file. */
    CHECK(ac_notes_count(nt_other) == 0, "other profile stays empty");
    ac_notes_close(nt);
    ac_notes_close(nt_other);
    CHECK(strcmp(cb_last_module, "notes") == 0 &&
              strcmp(cb_last_profile, "work") == 0,
          "notes mutations stamp the bound profile");

    /* ---- dlwpolicy (MD04) -------------------------------------------
     * The download-interception decision layer: pure ops need no
     * store, the bound profile's settings scope supplies the legacy
     * downloadmanager/* keys, and the facade-authored rules +
     * records live in profiles/<id>/downloads.json. */
    AcDlwpolicy *dw = ac_dlwpolicy(core, "work");
    AcDlwpolicy *dw_other = ac_dlwpolicy(core, "other");
    CHECK(dw && dw_other, "ac_dlwpolicy accessors failed");
    {
        /* Pure op over the facade handle. */
        static const char sn[] =
            "{\"op\":\"sanitize_name\",\"suggested\":\"a/b/c.zip\"}";
        char *r = ac_dlw_call(dw, (const uint8_t *)sn,
                              sizeof(sn) - 1);
        CHECK(r && strstr(r, "\"name\":\"c.zip\""),
              "dlw sanitize_name");
        if (r)
            ac_string_free(r);
        /* The threat table: a dangerous suffix warns. */
        static const char th[] =
            "{\"op\":\"threat\",\"file_name\":\"setup.exe\","
            "\"mime\":\"text/plain\"}";
        r = ac_dlw_call(dw, (const uint8_t *)th, sizeof(th) - 1);
        CHECK(r && strstr(r, "\"warn\":true") &&
                  strstr(r, "dangerous-extension"),
              "dlw threat");
        if (r)
            ac_string_free(r);
        /* A blocked-extension rule flips the verdict to block. */
        static const char rs[] =
            "{\"op\":\"rule_set\",\"key\":\"blocked_extensions\","
            "\"value\":[\"scr\",\"bat\"]}";
        r = ac_dlw_call(dw, (const uint8_t *)rs, sizeof(rs) - 1);
        CHECK(r && strstr(r, "\"set\":true"), "dlw rule_set");
        if (r)
            ac_string_free(r);
        static const char vd[] =
            "{\"op\":\"verdict\",\"url\":\"https://h.invalid/f.scr\","
            "\"mime\":\"application/octet-stream\","
            "\"context\":\"download_request\"}";
        r = ac_dlw_call(dw, (const uint8_t *)vd, sizeof(vd) - 1);
        CHECK(r && strstr(r, "\"action\":\"block\""),
              "dlw verdict blocks a listed extension");
        if (r)
            ac_string_free(r);
        /* The other persona's empty store never blocks. */
        r = ac_dlw_call(dw_other, (const uint8_t *)vd,
                        sizeof(vd) - 1);
        CHECK(r && strstr(r, "\"action\":\"download\""),
              "dlw verdict is per-profile");
        if (r)
            ac_string_free(r);
        /* Records round-trip through downloads.json. */
        static const char recs[] =
            "{\"op\":\"records_save\",\"records\":[{"
            "\"url\":\"https://h.invalid/a\",\"location\":\"/tmp/a\","
            "\"done\":true,\"size\":7}]}";
        r = ac_dlw_call(dw, (const uint8_t *)recs, sizeof(recs) - 1);
        CHECK(r && strstr(r, "\"saved\":1"), "dlw records_save");
        if (r)
            ac_string_free(r);
        static const char rl[] = "{\"op\":\"records_list\"}";
        r = ac_dlw_call(dw, (const uint8_t *)rl, sizeof(rl) - 1);
        CHECK(r && strstr(r, "h.invalid/a") && strstr(r, "\"size\":7"),
              "dlw records_list");
        if (r)
            ac_string_free(r);
        {
            char p[512];
            struct stat st;
            join_path(p, sizeof(p), root, "profiles/work",
                      "downloads.json");
            CHECK(stat(p, &st) == 0 && st.st_size > 0,
                  "downloads.json written under profiles/work");
        }
        r = ac_dlw_call(dw_other, (const uint8_t *)rl,
                        sizeof(rl) - 1);
        CHECK(r && strstr(r, "\"records\":[]"),
              "other profile sees no records");
        if (r)
            ac_string_free(r);
        static const char rc_[] = "{\"op\":\"records_clear\"}";
        r = ac_dlw_call(dw, (const uint8_t *)rc_, sizeof(rc_) - 1);
        CHECK(r && strstr(r, "\"cleared\":true"),
              "dlw records_clear");
        if (r)
            ac_string_free(r);
    }
    ac_dlwpolicy_close(dw);
    ac_dlwpolicy_close(dw_other);
    CHECK(strcmp(cb_last_module, "dlpolicy") == 0 &&
              strcmp(cb_last_profile, "work") == 0,
          "dlwpolicy mutations stamp the bound profile");

    /* ---- privacy (MD05) ----------------------------------------------
     * The per-persona privacy/* store view: the Sec-GPC decision and
     * the unified {gpc,strip,adblock} verdict read the bound
     * profile's settings scope; read-only, no topic emitted. */
    AcPrivacy *pv = ac_privacy(core, "work");
    AcPrivacy *pv_other = ac_privacy(core, "other");
    CHECK(pv && pv_other, "ac_privacy accessors failed");
    {
        /* A pristine persona sends Sec-GPC; tor_mode sends
         * unconditionally. */
        CHECK(ac_privacy_gpc(pv, 0) == 1, "privacy gpc default on");
        CHECK(ac_privacy_gpc(pv, 1) == 1, "privacy gpc tor constant");
        /* The granular endpoint answers the flag set. */
        static const char fl[] = "{\"op\":\"flags\"}";
        char *r = ac_privacy_call(pv, (const uint8_t *)fl,
                                  sizeof(fl) - 1);
        CHECK(r && strstr(r, "\"gpc_signal\":true") &&
                  strstr(r, "\"adblock_enabled\":true"),
              "privacy flags");
        if (r)
            ac_string_free(r);
        /* A persona write flips only that persona's answer. */
        AcSettings *ws = ac_settings(core, "work");
        CHECK(ws != NULL, "ac_settings for privacy leg");
        CHECK(ac_settings_set_bool(ws, "privacy/gpcSignal", 0)
                  == AC_OK,
              "privacy write via settings lane");
        CHECK(ac_privacy_gpc(pv, 0) == 0,
              "privacy gpc follows the bound persona's row");
        CHECK(ac_privacy_gpc(pv_other, 0) == 1,
              "privacy gpc is per-profile");
        static const char vd5[] = "{\"op\":\"verdict\"}";
        r = ac_privacy_call(pv, (const uint8_t *)vd5,
                            sizeof(vd5) - 1);
        CHECK(r && strstr(r, "\"gpc\":false") &&
                  strstr(r, "\"strip\":true"),
              "privacy verdict folds strip+gpc");
        if (r)
            ac_string_free(r);
        /* The pure defaults op needs no store. */
        static const char df[] = "{\"op\":\"defaults\"}";
        r = ac_privacy_call(NULL, (const uint8_t *)df,
                            sizeof(df) - 1);
        CHECK(r == NULL, "privacy NULL handle degrades");
        r = ac_privacy_call(pv, (const uint8_t *)df,
                            sizeof(df) - 1);
        CHECK(r && strstr(r, "\"gpc_signal\":true"),
              "privacy defaults");
        if (r)
            ac_string_free(r);
        ac_settings_close(ws);
    }
    ac_privacy_close(pv);
    ac_privacy_close(pv_other);

    /* ---- urlclean (MD06) ---------------------------------------------
     * The per-persona URL-sanitize policy: the copy/nav toggles, the
     * editable profile ruleset (profiles/<id>/urlclean.json), the
     * legacy urlcleaner/extraParams merge and the clean verdict. */
    AcUrlclean *uc = ac_urlclean(core, "work");
    AcUrlclean *uc_other = ac_urlclean(core, "other");
    CHECK(uc && uc_other, "ac_urlclean accessors failed");
    {
        /* A pristine persona cleans with the vendored ruleset. */
        static const char cl[] =
            "{\"op\":\"clean\",\"url\":\"https://h.invalid/?"
            "utm_source=x&id=1\",\"purpose\":\"copy\"}";
        char *r = ac_urlclean_call(uc, (const uint8_t *)cl,
                                   sizeof(cl) - 1);
        CHECK(r && strstr(r, "\"changed\":true") &&
                  strstr(r, "https://h.invalid/?id=1"),
              "urlclean strips a tracker param");
        if (r)
            ac_string_free(r);
        /* The legacy extraParams key merges into the strip set. */
        AcSettings *ws = ac_settings(core, "work");
        CHECK(ws != NULL, "ac_settings for urlclean leg");
        CHECK(ac_settings_set_list(
                  ws, "urlcleaner/extraParams",
                  (const uint8_t *)"[\"partner\"]",
                  sizeof("[\"partner\"]") - 1) == AC_OK,
              "urlclean extraParams via settings lane");
        static const char cl2[] =
            "{\"op\":\"clean\",\"url\":\"https://h.invalid/?"
            "partner=p&id=1\",\"purpose\":\"copy\"}";
        r = ac_urlclean_call(uc, (const uint8_t *)cl2,
                             sizeof(cl2) - 1);
        CHECK(r && strstr(r, "https://h.invalid/?id=1"),
              "urlclean merges extraParams");
        if (r)
            ac_string_free(r);
        /* A profile ruleset replaces the global list. */
        static const char rs[] =
            "{\"op\":\"ruleset_set\",\"ruleset\":{\"version\":1,"
            "\"params\":[\"only_this\"]}}";
        r = ac_urlclean_call(uc, (const uint8_t *)rs, sizeof(rs) - 1);
        CHECK(r && strstr(r, "\"set\":true"), "urlclean ruleset_set");
        if (r)
            ac_string_free(r);
        r = ac_urlclean_call(uc, (const uint8_t *)cl, sizeof(cl) - 1);
        CHECK(r && strstr(r, "\"changed\":false"),
              "urlclean profile ruleset overrides global");
        if (r)
            ac_string_free(r);
        {
            char p[512];
            struct stat st;
            join_path(p, sizeof(p), root, "profiles/work",
                      "urlclean.json");
            CHECK(stat(p, &st) == 0 && st.st_size > 0,
                  "urlclean.json written under profiles/work");
        }
        /* The other persona still strips with the global set. */
        r = ac_urlclean_call(uc_other, (const uint8_t *)cl,
                             sizeof(cl) - 1);
        CHECK(r && strstr(r, "\"changed\":true"),
              "urlclean ruleset is per-profile");
        if (r)
            ac_string_free(r);
        /* The snapshot-facing composite carries the merged doc. */
        static const char cp[] = "{\"op\":\"clean_policy\"}";
        r = ac_urlclean_call(uc, (const uint8_t *)cp, sizeof(cp) - 1);
        CHECK(r && strstr(r, "\"source\":\"profile\"") &&
                  strstr(r, "only_this") && strstr(r, "partner"),
              "urlclean clean_policy merges ruleset + extras");
        if (r)
            ac_string_free(r);
        /* The copy toggle is facade-authored; the nav toggle stays
         * Qt-owned and refuses the write. */
        static const char se[] =
            "{\"op\":\"set_enabled\",\"purpose\":\"copy\","
            "\"value\":false}";
        r = ac_urlclean_call(uc, (const uint8_t *)se, sizeof(se) - 1);
        CHECK(r && strstr(r, "\"set\":true"), "urlclean set_enabled");
        if (r)
            ac_string_free(r);
        r = ac_urlclean_call(uc, (const uint8_t *)cl2,
                             sizeof(cl2) - 1);
        CHECK(r && strstr(r, "\"enabled\":false"),
              "urlclean copy toggle gates the verdict");
        if (r)
            ac_string_free(r);
        static const char sne[] =
            "{\"op\":\"set_enabled\",\"purpose\":\"nav\","
            "\"value\":false}";
        r = ac_urlclean_call(uc, (const uint8_t *)sne,
                             sizeof(sne) - 1);
        CHECK(r == NULL,
              "urlclean refuses to write the Qt-owned nav toggle");
        static const char rr[] = "{\"op\":\"ruleset_reset\"}";
        r = ac_urlclean_call(uc, (const uint8_t *)rr, sizeof(rr) - 1);
        CHECK(r && strstr(r, "\"reset\":true"),
              "urlclean ruleset_reset");
        if (r)
            ac_string_free(r);
        ac_settings_close(ws);
    }
    ac_urlclean_close(uc);
    ac_urlclean_close(uc_other);
    CHECK(strcmp(cb_last_module, "urlclean") == 0 &&
              strcmp(cb_last_profile, "work") == 0,
          "urlclean mutations stamp the bound profile");

    /* ---- close-out ---------------------------------------------------- */
    ac_settings_close(gs);
    ac_settings_close(ps);
    ac_profiles_close(pr);
    ac_hist_close(h);
    ac_bm_close(b);
    ac_policy_close(p);
    /* NULL closes are legal. */
    ac_hist_close(NULL);
    ac_bm_close(NULL);
    ac_policy_close(NULL);
    ac_settings_close(NULL);
    ac_profiles_close(NULL);
    ac_session_close(NULL);
    ac_cookies_close(NULL);
    ac_icons_close(NULL);
    ac_containers_close(NULL);
    ac_notes_close(NULL);
    ac_dlwpolicy_close(NULL);
    ac_privacy_close(NULL);
    ac_urlclean_close(NULL);
    ac_shutdown(core);
    ac_shutdown(NULL);

    if (failures == 0)
        fprintf(stderr, "acprobe: OK (callback saw %d events)\n",
                cb_hits);
    else
        fprintf(stderr, "acprobe: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
