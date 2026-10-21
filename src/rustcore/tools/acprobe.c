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

int main(int argc, char **argv)
{
    char root[512];
    if (argc > 1) {
        snprintf(root, sizeof(root), "%s", argv[1]);
        mkdir(root, 0700);
    } else {
        snprintf(root, sizeof(root), "/tmp/acprobe-XXXXXX");
        if (!mkdtemp(root)) {
            perror("mkdtemp");
            return 2;
        }
    }
    fprintf(stderr, "acprobe: data root %s\n", root);

    /* ---- housekeeping -------------------------------------------- */
    CHECK(ac_api_version() == AC_API_VERSION, "api version mismatch");
    CHECK(ac_capabilities() ==
              (AC_CAP_SETTINGS | AC_CAP_PROFILES | AC_CAP_HISTORY |
               AC_CAP_BOOKMARKS | AC_CAP_POLICY),
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
    ac_shutdown(core);
    ac_shutdown(NULL);

    if (failures == 0)
        fprintf(stderr, "acprobe: OK (callback saw %d events)\n",
                cb_hits);
    else
        fprintf(stderr, "acprobe: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
