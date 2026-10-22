/*
 * arktos_core.h — the stable Arktos browser-core facade (RC07).
 *
 * THE single C ABI every engine backend programs against.  It is a
 * thin ownership layer over the RC02–RC06 modules: an AcCore owns
 * profile-bound module handles, and every ac_* operation forwards
 * to the same underlying store the legacy rc_* surface (rustcore.h)
 * reaches — the two spellings can never diverge on disk.
 *
 * Contract (full text in .devin/RUSTCORE-ABI.md):
 *
 *   - Append-only.  New symbols may be added; existing signatures,
 *     struct layouts and enum values are frozen.  Never repurpose an
 *     enum value or renumber AcStatus.
 *   - Versioning.  ac_api_version() reports the library's ABI
 *     version; AcConfig carries struct_size = sizeof(AcConfig) and
 *     the api_version the caller was built against.  ac_init refuses
 *     a config smaller than the v1 struct or an api_version newer
 *     than the library.
 *   - Errors.  Every fallible call returns AcStatus (or NULL/0/-1
 *     where noted); ac_last_error() reads the thread-local message,
 *     freed with ac_string_free.
 *   - Ownership.  Strings/buffers returned are caller-owned (free
 *     with ac_string_free / ac_buffer_free).  Module handles are
 *     minted by ac_* accessors on an AcCore, released individually
 *     with the matching ac_*_close, and all die with ac_shutdown —
 *     a handle may only be used while its AcCore lives.
 *   - Profile binding.  ac_settings binds a scope at open
 *     (NULL/empty profile_id = the global scope); ac_history,
 *     ac_bookmarks and ac_policy bind a profile at open (NULL or a
 *     non-slug id resolves to "default") and re-assert that binding
 *     on every call — no caller-side data-dir switching is needed.
 *   - Panic safety.  No Rust panic crosses the FFI boundary; a panic
 *     degrades to NULL/AC_ERR_INTERNAL with a stashed message.
 *   - Threading.  Thread-safe behind internal mutexes; a bound
 *     handle serializes on its store's lock, so reads/writes from
 *     different threads are safe but not lock-free.  Change
 *     callbacks fire synchronously on the mutating thread and never
 *     while a store lock is held.
 */

#ifndef ARKTOS_CORE_H
#define ARKTOS_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "rustcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The facade ABI version this header was written for.
 * v2 adds the AcSession module (RC13).
 * v3 adds the AcCookies module (RC15).
 * v4 adds the AcIcons module (RC16).
 * v5 adds the AcContainers module (RC19). */
#define AC_API_VERSION 5u

/* ac_capabilities() bits — one per module. */
#define AC_CAP_SETTINGS   0x01ull
#define AC_CAP_PROFILES   0x02ull
#define AC_CAP_HISTORY    0x04ull
#define AC_CAP_BOOKMARKS  0x08ull
#define AC_CAP_POLICY     0x10ull
#define AC_CAP_SESSION    0x20ull
#define AC_CAP_COOKIES    0x40ull
#define AC_CAP_ICONS      0x80ull
#define AC_CAP_CONTAINERS 0x100ull

/* AcStatus and AcBuffer are the RC02–RC06 types verbatim — the
 * facade shares their spelling so mixed ac_/rc_ code compiles
 * without casts. */
typedef RcStatus AcStatus;
typedef RcBuffer AcBuffer;

/* The legacy status values remain reachable under the AcStatus
 * typedef; these aliases give facade-only callers the ac_* names. */
#define AC_OK             RC_OK
#define AC_ERR_GENERIC    RC_ERR_GENERIC
#define AC_ERR_NOT_FOUND  RC_ERR_NOT_FOUND
#define AC_ERR_INVALID    RC_ERR_INVALID
#define AC_ERR_IO         RC_ERR_IO
#define AC_ERR_WRONG_PASSWORD RC_ERR_WRONG_PASSWORD
#define AC_ERR_TAMPERED   RC_ERR_TAMPERED
#define AC_ERR_INTERNAL   RC_ERR_INTERNAL

/* Opaque core + module handles.  AcSettings and AcProfiles are the
 * RC02/RC03 handle types — legacy rc_settings_* / rc_profiles_*
 * calls on facade-minted handles stay legal until callers migrate. */
typedef struct AcCore   AcCore;
typedef struct AcHistory    AcHistory;
typedef struct AcBookmarks  AcBookmarks;
typedef struct AcPolicy     AcPolicy;
typedef struct AcSession    AcSession;
typedef struct AcCookies    AcCookies;
typedef struct AcIcons      AcIcons;
typedef struct AcContainers AcContainers;
typedef RcSettings AcSettings;
typedef RcProfiles AcProfiles;

/* Change notification, one per AcCore.  `module` is a stable topic
 * ("settings", "profiles", "history", "bookmarks", "sitedec",
 * "policy", "uaspoof", "cookies", "icons", "containers");
 * `profile_id` is the bound
 * profile that
 * changed or NULL for global/unscoped mutations.  Both pointers are
 * borrowed — valid only for the duration of the call.  Fires
 * synchronously on the mutating thread outside every store lock;
 * consumers that need another thread must hop themselves. */
typedef void (*AcChangeCallback)(const char *module,
                                 const char *profile_id,
                                 void *userdata);

/* v1 configuration — append-only; always pass
 * struct_size = sizeof(AcConfig). */
typedef struct AcConfig {
    uint32_t struct_size;      /* sizeof(AcConfig) — required */
    uint32_t api_version;      /* AC_API_VERSION the caller expects; 0 = any */
    const char *data_root;     /* app data root (required) */
    const char *app_name;      /* informational; may be NULL */
    const char *default_profile_name; /* display-name seed for the
                                         synthesized "default" row;
                                         may be NULL */
} AcConfig;

/* ---- housekeeping -------------------------------------------------- */

uint32_t     ac_api_version(void);
uint64_t     ac_capabilities(void);
AcCore      *ac_init(const AcConfig *cfg);
void         ac_shutdown(AcCore *core);
char        *ac_last_error(void);
void         ac_string_free(char *s);
void         ac_buffer_free(AcBuffer buf);
void         ac_set_change_callback(AcCore *core,
                                  AcChangeCallback cb,
                                  void *userdata);

/* ---- module accessors -------------------------------------------------
 *
 * Each mints a profile-bound handle owned by `core`; release with
 * the matching close (or let ac_shutdown reclaim it).  NULL return
 * = failure with a stashed ac_last_error().
 */

AcSettings  *ac_settings(AcCore *core, const char *profile_id);
AcProfiles  *ac_profiles(AcCore *core);
AcHistory   *ac_history(AcCore *core, const char *profile_id);
int          ac_history_exists(AcCore *core, const char *profile_id);
AcBookmarks *ac_bookmarks(AcCore *core, const char *profile_id);
AcPolicy    *ac_policy(AcCore *core, const char *profile_id);
AcSession   *ac_session(AcCore *core, const char *profile_id);
AcCookies   *ac_cookies(AcCore *core, const char *profile_id);
AcIcons     *ac_icons(AcCore *core, const char *profile_id);
AcContainers *ac_containers(AcCore *core, const char *profile_id);

void ac_settings_close(AcSettings *h);
void ac_profiles_close(AcProfiles *h);
void ac_hist_close(AcHistory *h);
void ac_bm_close(AcBookmarks *h);
void ac_policy_close(AcPolicy *h);
void ac_session_close(AcSession *h);
void ac_cookies_close(AcCookies *h);
void ac_icons_close(AcIcons *h);
void ac_containers_close(AcContainers *h);

/* ---- settings (RC02) --------------------------------------------------- */

AcStatus ac_settings_import_ini(AcSettings *h, const char *path);
int      ac_settings_contains(AcSettings *h, const char *key);
AcStatus ac_settings_get(AcSettings *h, const char *key, AcBuffer *out);
AcStatus ac_settings_get_string(AcSettings *h, const char *key, AcBuffer *out);
AcStatus ac_settings_get_bool(AcSettings *h, const char *key, int *out);
AcStatus ac_settings_get_int(AcSettings *h, const char *key, int64_t *out);
AcStatus ac_settings_get_double(AcSettings *h, const char *key, double *out);
AcStatus ac_settings_get_bytes(AcSettings *h, const char *key, AcBuffer *out);
AcStatus ac_settings_get_list(AcSettings *h, const char *key, AcBuffer *out);
AcStatus ac_settings_set_string(AcSettings *h, const char *key, const char *value);
AcStatus ac_settings_set_bool(AcSettings *h, const char *key, int value);
AcStatus ac_settings_set_int(AcSettings *h, const char *key, int64_t value);
AcStatus ac_settings_set_double(AcSettings *h, const char *key, double value);
AcStatus ac_settings_set_bytes(AcSettings *h, const char *key,
                               const uint8_t *value, size_t len);
AcStatus ac_settings_set_list(AcSettings *h, const char *key,
                              const uint8_t *json, size_t len);
AcStatus ac_settings_remove(AcSettings *h, const char *key);
char    *ac_settings_list(AcSettings *h, const char *prefix);
AcStatus ac_settings_sync(AcSettings *h);

/* ---- profiles (RC03) ---------------------------------------------------- */

AcStatus ac_profiles_reload(AcProfiles *h);
char    *ac_profiles_list(AcProfiles *h);
char    *ac_profiles_get(AcProfiles *h, const char *id);
char    *ac_profiles_create(AcProfiles *h, const char *display_name);
AcStatus ac_profiles_rename(AcProfiles *h, const char *id, const char *name);
AcStatus ac_profiles_set_avatar(AcProfiles *h, const char *id,
                                const char *name_or_null);
AcStatus ac_profiles_delete(AcProfiles *h, const char *id);
char    *ac_profiles_active(AcProfiles *h);
AcStatus ac_profiles_set_active(AcProfiles *h, const char *id);
char    *ac_profiles_dir(AcProfiles *h, const char *id);
char    *ac_profiles_cache_dir(AcProfiles *h, const char *id);
char    *ac_profiles_engine_storage_dir(AcProfiles *h, const char *id);
char    *ac_profiles_engine_cache_dir(AcProfiles *h, const char *id);
char    *ac_profiles_settings_dir(AcProfiles *h, const char *id);
char    *ac_profiles_avatar_path(AcProfiles *h, const char *id);
char    *ac_profiles_file(AcProfiles *h);
char    *ac_profiles_slugify(const char *display_name);

/* ---- history (RC04) ------------------------------------------------------ */

int64_t  ac_hist_count(const AcHistory *h);
char    *ac_hist_entry_at(const AcHistory *h, int64_t row);
AcStatus ac_hist_add(const AcHistory *h, const char *url,
                     const char *title, int64_t ts_ms);
AcStatus ac_hist_update_title(const AcHistory *h, const char *url,
                              const char *title);
AcStatus ac_hist_remove(const AcHistory *h, const char *url,
                        const char *title, int64_t ts_ms);
AcStatus ac_hist_clear(const AcHistory *h);
AcStatus ac_hist_clear_since(const AcHistory *h, int64_t since_ms);
AcStatus ac_hist_expire_older_than(const AcHistory *h, int64_t cutoff_ms,
                                   int64_t *out_removed);
char    *ac_hist_query(const AcHistory *h, const char *spec_json);
AcStatus ac_hist_icon_set(const AcHistory *h, const char *host,
                          const uint8_t *png, size_t len);
AcStatus ac_hist_icon_get(const AcHistory *h, const char *host,
                          AcBuffer *out);
AcStatus ac_hist_icon_clear(const AcHistory *h);
char    *ac_hist_suggest(const AcHistory *h, const char *term, int64_t limit);

/* ---- bookmarks (RC05) ------------------------------------------------------ */

uint64_t ac_bm_root(const AcBookmarks *h);
AcStatus ac_bm_reload(const AcBookmarks *h);
AcStatus ac_bm_load_mem(const AcBookmarks *h, const uint8_t *xbel,
                        size_t len);
AcStatus ac_bm_save(const AcBookmarks *h, const char *path_or_null);
int64_t  ac_bm_child_count(const AcBookmarks *h, uint64_t node);
uint64_t ac_bm_child_at(const AcBookmarks *h, uint64_t node, int64_t row);
uint64_t ac_bm_parent(const AcBookmarks *h, uint64_t node);
char    *ac_bm_get(const AcBookmarks *h, uint64_t node);
uint64_t ac_bm_create(const AcBookmarks *h, uint64_t parent, int64_t row,
                      const char *json);
AcStatus ac_bm_attach(const AcBookmarks *h, uint64_t parent, int64_t row,
                      uint64_t node);
AcStatus ac_bm_detach(const AcBookmarks *h, uint64_t node);
AcStatus ac_bm_destroy(const AcBookmarks *h, uint64_t node);
AcStatus ac_bm_set_title(const AcBookmarks *h, uint64_t node,
                         const char *value);
AcStatus ac_bm_set_url(const AcBookmarks *h, uint64_t node,
                       const char *value);
AcStatus ac_bm_set_desc(const AcBookmarks *h, uint64_t node,
                        const char *value);
AcStatus ac_bm_set_expanded(const AcBookmarks *h, uint64_t node,
                            int expanded);
AcStatus ac_bm_set_tags(const AcBookmarks *h, uint64_t node,
                        const char *json);
uint64_t ac_bm_find(const AcBookmarks *h, const char *url);
char    *ac_bm_import_html(const AcBookmarks *h, uint64_t parent,
                           const uint8_t *html, size_t len, int flags);
AcStatus ac_bm_export_html(const AcBookmarks *h, const char *path);

/* ---- policy: site decisions + verdicts (RC06) ------------------------------ */

AcStatus ac_sitedec_get(const AcPolicy *h, const char *kind,
                        const char *key, AcBuffer *out);
AcStatus ac_sitedec_set(const AcPolicy *h, const char *kind,
                        const char *key, const char *value);
AcStatus ac_sitedec_remove(const AcPolicy *h, const char *kind,
                           const char *key);
AcStatus ac_sitedec_clear(const AcPolicy *h, const char *kind);
AcStatus ac_sitedec_replace(const AcPolicy *h, const char *kind,
                            const uint8_t *json, size_t len);
char    *ac_sitedec_list(const AcPolicy *h, const char *kind);
char    *ac_sitedec_snapshot(const AcPolicy *h);
char    *ac_sitedec_lookup(const AcPolicy *h, const char *kind,
                           const char *host);
int      ac_sitedec_store_present(const AcPolicy *h);
AcStatus ac_sitedec_reload(const AcPolicy *h);
AcStatus ac_sitedec_reset(const AcPolicy *h);

AcStatus ac_policy_load_snapshot(const AcPolicy *h,
                                 const uint8_t *json, size_t len);
char    *ac_policy_evaluate(const AcPolicy *h,
                            const uint8_t *json, size_t len);
int      ac_policy_cookie_filter(const AcPolicy *h,
                                 const uint8_t *json, size_t len);
char    *ac_policy_call(const AcPolicy *h,
                        const uint8_t *json, size_t len);
uint64_t ac_policy_eval_generation(void);

int      ac_blocklist_check(const AcPolicy *h, const char *host);
AcStatus ac_blocklist_load(const AcPolicy *h, const uint8_t *text,
                           size_t len);
AcStatus ac_blocklist_reload(const AcPolicy *h);
size_t   ac_blocklist_count(const AcPolicy *h);

uint32_t ac_tranco_rank(const AcPolicy *h, const char *host);
AcStatus ac_tranco_load(const AcPolicy *h, const uint8_t *text,
                        size_t len);
AcStatus ac_tranco_reload(const AcPolicy *h);
size_t   ac_tranco_count(const AcPolicy *h);
size_t   ac_tranco_file_entries(const AcPolicy *h, const char *name);

char    *ac_sitetrust_lookup(const AcPolicy *h, const char *host,
                             int is_https);
char    *ac_sitetrust_lookup_opts(const AcPolicy *h, const char *host,
                                  int is_https, uint32_t probes);
AcStatus ac_sitetrust_reload(const AcPolicy *h);
char    *ac_lookalike_check(const AcPolicy *h, const char *host);

char    *ac_ua_spoof(const AcPolicy *h, const char *host);
AcStatus ac_ua_spoof_set(const AcPolicy *h, const char *host,
                         const char *ua);
AcStatus ac_ua_spoof_remove(const AcPolicy *h, const char *host);
char    *ac_ua_spoof_list(const AcPolicy *h);

/* ---- sessions: per-profile window/tab store + lifecycle (RC13) ------
 * The bound profile's profiles/<id>/session.dat — the same versioned
 * ARSS blob and JSON manifest the rc_session_* surface carries (see
 * rustcore.h for the manifest grammar, the "private" strip markers
 * and the "scroll" field).  These ops are path-bound: they run on the
 * handle's own file with no global data-dir switching.
 *
 * The lifecycle ops drive profiles/<id>/session.state — the
 * clean-shutdown flag + crash-dirty marker + restore-loop guard, so
 * "restore previous session?" works identically on every engine.
 * Session ops fire no change callbacks (per-run scratch state, not
 * shared-observable data). */

AcStatus ac_session_save(AcSession *h, const uint8_t *json, size_t len);
AcStatus ac_session_load(AcSession *h, AcBuffer *out);
int      ac_session_exists(const AcSession *h);
AcStatus ac_session_clear(AcSession *h);

int      ac_session_mark_start(AcSession *h);   /* 1 = prev ran dirty */
AcStatus ac_session_mark_exit(AcSession *h, int32_t code);
AcStatus ac_session_mark_terminated(AcSession *h, int32_t signal);
int      ac_session_dirty(const AcSession *h);
int      ac_session_crash_count(const AcSession *h);
AcStatus ac_session_last_exit(AcSession *h, AcBuffer *out);
AcStatus ac_session_set_restoring(AcSession *h, int on);
int      ac_session_restoring(const AcSession *h);

/* ---- cookies: per-profile cookie database (RC15) --------------------
 *
 * The bound profile's profiles/<id>/cookies.db — SQLite/WAL like
 * history.db, container-partitioned rows.  This is the Servo/WPE
 * jar's persistent home (declare AC_COOKIES_AUTHORITATIVE) AND
 * Chromium's durable mirror (Chromium's QWebEngineCookieStore stays
 * engine-owned — declare AC_COOKIES_MIRROR and this db is its
 * on-disk shadow for dialogs/sync).  The ops are identical under
 * either role; the flag is the honesty contract — capability
 * reporting must say which mode the backend actually runs.
 *
 * Records are JSON — {"container":"","name":"","value":"",
 * "domain":"","path":"/","expires":ms|null,"secure":b,
 * "httpOnly":b,"sameSite":0..3,"hostOnly":b,"created":ms}.  Domains
 * normalize (lowercase, no leading dot); hostOnly carries the
 * Qt-verbatim "leading-dot Domain = domain cookie, no-dot or absent
 * = host-exact" distinction.  Mutations emit the "cookies" change
 * topic stamped with the bound profile.
 */

#define AC_COOKIES_AUTHORITATIVE 0ll /* this file IS the jar */
#define AC_COOKIES_MIRROR        1ll /* the engine owns the jar */

int64_t  ac_cookies_role(const AcCookies *h);      /* -1 on error */
AcStatus ac_cookies_set_role(AcCookies *h, int64_t role);

/* Upsert one record JSON; a past expiry deletes the row (the
 * Set-Cookie eviction rule); an identical rewrite is a no-op. */
AcStatus ac_cookies_set(AcCookies *h, const uint8_t *json, size_t len);
/* One cookie by key — AC_ERR_NOT_FOUND on a miss; container NULL =
 * the default partition. */
AcStatus ac_cookies_get(const AcCookies *h, const char *domain,
                        const char *name, const char *path,
                        const char *container_or_null, AcBuffer *out);
AcStatus ac_cookies_remove(const AcCookies *h, const char *domain,
                           const char *name, const char *path,
                           const char *container_or_null);
/* A partition's rows (NULL container = all) as a JSON array —
 * caller frees with ac_string_free. */
char    *ac_cookies_list(const AcCookies *h,
                         const char *container_or_null);
/* The send-match — request JSON {"url":"https://h/p",
 * "container":"","first_party":"h"|null,"now_ms":0}; first_party
 * engages the SameSite cross-site filter.  Returns a JSON array in
 * RFC6265 §5.4 send order — caller frees. */
char    *ac_cookies_for_url(const AcCookies *h,
                            const uint8_t *request, size_t len);
/* Parse + store one Set-Cookie header value against `url` — the
 * authoritative backend's ingest.  Verdict JSON
 * {"stored":bool,"reason":...} — caller frees. */
char    *ac_cookies_store_header(AcCookies *h, const char *url,
                                 const char *header,
                                 const char *container_or_null);
/* The cookie-policy gate — consults the profile's sitedecisions.json
 * "cookie" kind (the file CookieJar's exceptions persist through)
 * plus the caller's policy snapshot; see rc_cookies_gate in
 * rustcore.h for the input/output shape.  Caller frees. */
char    *ac_cookies_gate(const AcCookies *h,
                         const uint8_t *input, size_t len);
/* Import/export the QNetworkCookie::toRawForm file shape — one raw
 * line per cookie (the vendored NetworkCookieJar's saveState
 * format).  Import skips lines without a Domain attribute and
 * returns {"imported":N,"skipped":M} — caller frees. */
char    *ac_cookies_import(AcCookies *h, const char *path,
                           const char *container_or_null);
AcStatus ac_cookies_export(const AcCookies *h, const char *path,
                           const char *container_or_null);
AcStatus ac_cookies_clear(AcCookies *h, const char *container_or_null);
/* The "clear site data" sweep — CookieJar::removeCookiesForHost
 * verbatim (rows scoped to the host/subdomains, plus rows on parent
 * domains the site still receives). */
AcStatus ac_cookies_remove_for_host(AcCookies *h, const char *host,
                                    const char *container_or_null);
int64_t  ac_cookies_count(const AcCookies *h,
                          const char *container_or_null); /* -1 err */
AcStatus ac_cookies_purge_expired(AcCookies *h, int64_t now_ms);
AcStatus ac_cookies_purge_session(AcCookies *h);

/* ---- icons: per-profile favicon/site-icon cache (RC16) ------------
 *
 * The bound profile's profiles/<id>/cache/icons/ — the disposable
 * lane (icon files live under the cache root, never the data root;
 * the vault's "cache" exclusion already holds them aside unsealed).
 * Records are keyed by page-url/origin and dedup by host: every page
 * of a host shares one PNG file.  Fetch stays engine-side — the
 * backend already has the bytes (QWebEnginePage::iconChanged); the
 * store owns the index, expiry and file lifecycle.  Path-bound like
 * AcSession (no process-global anchor), and a profile's first touch
 * folds the legacy stores in once — the <profile>/icons/ png files
 * and the history.db icons table — gated by a stamp that survives an
 * explicit clear.  Mutations emit the "icons" change topic stamped
 * with the bound profile.
 */

/* Record the favicon PNG for `url`'s icon key (lowercase host, or
 * the whole url when hostless).  An identical-bytes rewrite is a
 * no-op. */
AcStatus ac_icons_put(const AcIcons *h, const char *url,
                      const uint8_t *png, size_t len);
/* The stored PNG into *out (ac_buffer_free); AC_ERR_NOT_FOUND on a
 * miss. */
AcStatus ac_icons_get(const AcIcons *h, const char *url,
                      AcBuffer *out);
AcStatus ac_icons_remove(const AcIcons *h, const char *url);
AcStatus ac_icons_clear(const AcIcons *h);
/* Drop records not refreshed at-or-before cutoff_ms; out_removed may
 * be NULL. */
AcStatus ac_icons_sweep(const AcIcons *h, int64_t cutoff_ms,
                        int64_t *out_removed);
int64_t  ac_icons_count(const AcIcons *h);               /* -1 on err */
/* The derived icon key for `url` — caller frees. */
char    *ac_icons_key(const AcIcons *h, const char *url);
/* The index rows as a JSON array — caller frees. */
char    *ac_icons_list(const AcIcons *h);

/* ---- containers: registry + storage-namespace map (RC19) ----------
 *
 * The bound profile's container registry — the "containers" group of
 * its settings.json, so this module is a typed specialization over
 * the settings store (rc_containers_* in rustcore.h is the same
 * surface on a caller-held AcSettings; ac_containers_* keeps the
 * profile binding inside the handle — every op opens the bound
 * scope's store fresh, so a registry the C++ mirror wrote mid-process
 * is honored).  Rows are {"id","name","color","icon"}; ids are
 * library-minted v4 uuids in the C++ hex-with-dashes shape.
 *
 * ac_containers_namespace is the storage-namespace map engines
 * partition by: the same storage root / VSEC02 cache tree / Qt
 * residue dir / storage name ContainerManager computes for a
 * QWebEngineProfile, plus the cookie-partition key the ac_cookies_*
 * lane takes as `container`.  The default ("") container has no
 * namespace — its storage is the persona profile itself.  "Always
 * open in <container>" host rules live in ac_sitedec_*'s "container"
 * kind, not here.  Mutations emit "containers" stamped with the
 * bound profile.
 */

/* The registry as a JSON array in creation order — caller frees. */
char    *ac_containers_list(const AcContainers *h);
/* One row — NULL when the id is unregistered (a miss is not an
 * error).  Caller frees. */
char    *ac_containers_get(const AcContainers *h, const char *id);
/* Create from JSON {"name","color","icon"?} — returns the row JSON
 * (caller frees) or NULL + stashed error. */
char    *ac_containers_create(const AcContainers *h,
                              const uint8_t *json, size_t len);
/* Patch from JSON {"name"?,"color"?,"icon"?} — "icon":"" or null
 * clears it; AC_ERR_NOT_FOUND on an unregistered id. */
AcStatus ac_containers_update(const AcContainers *h, const char *id,
                              const uint8_t *json, size_t len);
/* The store half of deleteContainer — group + "order" entry;
 * AC_ERR_NOT_FOUND on an unregistered id.  The on-disk storage trees
 * are the caller's job — ac_containers_namespace derives them. */
AcStatus ac_containers_remove(const AcContainers *h, const char *id);
/* The storage namespace JSON {"storage_name","storage_path",
 * "cache_path","default_data_path","cookie_partition"} — caller
 * frees; NULL + stashed error on a bad id. */
char    *ac_containers_namespace(const AcContainers *h,
                                 const char *container_id);

#ifdef __cplusplus
}
#endif

#endif /* ARKTOS_CORE_H */
