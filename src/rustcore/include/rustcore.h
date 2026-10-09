/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef ARORA_RUSTCORE_H
#define ARORA_RUSTCORE_H

/*
 * C ABI for libarora_rustcore — the shared, engine-neutral Rust core
 * (RCORE01).  Stable rules for every component landing here:
 *
 *  - Fallible functions return RcStatus; RC_OK is the only success.
 *    On failure, rc_last_error_message() describes it on that thread.
 *  - Byte results arrive via RcBuffer (free with rc_buffer_free);
 *    string results via returned char* (free with rc_string_free).
 *  - Mutations that matter announce a topic string through the
 *    rc_set_change_callback hook; RustCoreBridge re-emits them as
 *    queued Qt signals (the callback -> Qt-signal bridge).
 *
 * Credential-store file names (relative to the data dir handed to
 * rc_set_data_dir): "securestore.key", "securestore.kdf",
 * RC_CREDENTIALS_FILE.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RcStatus {
    RC_OK = 0,
    RC_INVALID_ARGUMENT = 1,
    RC_IO = 2,
    RC_LOCKED = 3,              /* passphrase mode, store locked */
    RC_WRONG_PASSPHRASE = 4,    /* verifier rejected the derived key */
    RC_CRYPTO = 5,              /* AEAD auth failure / panic fallback */
    RC_NOT_INITIALIZED = 6,     /* rc_set_data_dir not called */
    RC_CORRUPT = 7,             /* malformed on-disk artifact */
    RC_ALREADY_ENABLED = 8,
    RC_NOT_ENABLED = 9,
    RC_UNAVAILABLE = 10,        /* RNG or platform facility missing */
    RC_NOT_FOUND = 11,          /* no such credential name */
    RC_EMPTY_PASSPHRASE = 12
} RcStatus;

typedef struct RcBuffer {
    uint8_t *data;
    size_t len;
} RcBuffer;

#define RC_CREDENTIALS_FILE "credentials.dat"

/* Change topics emitted so far: "credentials" (rc_cred_* map). */
typedef void (*RcChangeCallback)(const char *topicUtf8, void *userdata);

/* --- housekeeping ------------------------------------------------- */

/* Point the core at the app data dir.  Idempotent, cheap — call it
 * before every operation if the dir can change (test-mode switches). */
RcStatus rc_set_data_dir(const char *utf8Path);
char *rc_last_error_message(void);  /* "" when nothing failed */
void rc_string_free(char *s);
void rc_buffer_free(RcBuffer buf);
void rc_set_change_callback(RcChangeCallback cb, void *userdata);

/* --- custody ------------------------------------------------------ */

int rc_is_available(void);              /* crypto + custody usable */
int rc_is_sealed(const uint8_t *blob, size_t len);   /* ARSEC1 magic */
int rc_passphrase_enabled(void);        /* securestore.kdf exists */
int rc_is_unlocked(void);

/* Seal/open under the current custody key. */
RcStatus rc_seal(const uint8_t *plain, size_t len, RcBuffer *out);
RcStatus rc_open(const uint8_t *blob, size_t len, RcBuffer *out);

/* Explicit-key variants for custody transitions: the caller holds the
 * old and new keys while re-sealing its consumer stores. */
RcStatus rc_seal_with_key(const uint8_t *key32,
                          const uint8_t *plain, size_t len,
                          RcBuffer *out);
RcStatus rc_open_with_key(const uint8_t *key32,
                          const uint8_t *blob, size_t len,
                          RcBuffer *out);

RcStatus rc_unlock(const uint8_t *passUtf8, size_t len);
void rc_lock(void);

/* Staged custody operations (the Qt shim interleaves its own consumer
 * re-sealing between these, preserving the crash-safe ordering the
 * C++ implementation established):
 *   enable:  rc_key_file_copy(old) -> rc_kdf_create(pass)
 *            -> reseal consumers -> rc_key_file_delete()
 *   disable: rc_derived_key_copy(old) -> rc_key_file_create()
 *            -> rc_key_file_copy(new) -> reseal consumers
 *            -> rc_kdf_file_delete()
 *   change:  rc_derived_key_copy(old) -> rc_kdf_create(new)
 *            -> reseal consumers
 */
RcStatus rc_kdf_create(const uint8_t *passUtf8, size_t len);
RcStatus rc_key_file_create(void);
RcStatus rc_key_file_delete(void);
RcStatus rc_kdf_file_delete(void);
RcStatus rc_key_file_copy(uint8_t *out32);
RcStatus rc_derived_key_copy(uint8_t *out32);

/* --- named credential store (credentials.dat) ---------------------- */

RcStatus rc_cred_get(const char *nameUtf8, RcBuffer *out);
RcStatus rc_cred_put(const char *nameUtf8,
                     const uint8_t *value, size_t len);
RcStatus rc_cred_remove(const char *nameUtf8);
char *rc_cred_list(void);       /* JSON array; rc_string_free() */
/* Whole-op rotation for Rust-only consumers (covers credentials.dat;
 * the Qt shim uses the staged calls above for its own consumers). */
RcStatus rc_cred_change_passphrase(const uint8_t *passUtf8, size_t len);
/* Re-seal the credential file between explicit keys mid-transition. */
RcStatus rc_cred_reseal(const uint8_t *from32, const uint8_t *to32);

/* --- URL cleaning (SEC17) --------------------------------------------
 * ClearURLs-style tracking-parameter stripping.  The ruleset is a
 * vendored JSON file compiled into the crate, overridable by
 * "<data dir>/urlstrip-rules.json" — an update drops that file in like
 * a filter-list refresh and calls rc_urlstrip_reload() to activate it.
 *
 * rc_urlstrip returns the URL with tracking query parameters removed,
 * or a copy of the input when nothing matched (only http(s) URLs can
 * match).  NULL on error (rc_last_error_message).  Free the result
 * with rc_string_free(). */
char *rc_urlstrip(const char *urlUtf8);

/* Swap in a caller-supplied JSON ruleset (update/test seam).
 * RC_CORRUPT on malformed input — the previous ruleset stays active. */
RcStatus rc_urlstrip_load_rules(const uint8_t *jsonUtf8, size_t len);

/* Re-read the data-dir override (or the vendored set when absent).
 * RC_CORRUPT when an override exists but does not parse — the
 * previous ruleset stays active. */
RcStatus rc_urlstrip_reload(void);

/* --- domain blocklist (SEC18) ----------------------------------------
 * Local anti-phishing/malware domain list — a lookup never leaves the
 * machine.  The active set is the vendored seed (compiled in) unioned
 * with "<data dir>/blocklist-domains.txt", which the Qt-side
 * DomainBlocklist updater writes from remote feeds before calling
 * rc_blocklist_reload().  Matching is exact + suffix: a listed
 * "dom.ain" blocks "dom.ain" and every "*.dom.ain".
 *
 * rc_blocklist_check returns 1 when host is listed, 0 otherwise
 * (bad pointers and malformed hosts read as "not listed" — the check
 * never blocks on an FFI hiccup). */
int rc_blocklist_check(const char *hostUtf8);

/* Swap in a caller-supplied list body — plain domains, hostfile rows
 * and URL rows all parse (update/test seam).  RC_CORRUPT when the
 * body yields no usable entries; the previous list stays active. */
RcStatus rc_blocklist_load(const uint8_t *textUtf8, size_t len);

/* Re-run the seed-union-override merge.  RC_CORRUPT when an override
 * exists but yields no usable entries — previous list stays active. */
RcStatus rc_blocklist_reload(void);
size_t rc_blocklist_count(void);

/* --- navigation policy (ENG02) ----------------------------------------
 * The engine-agnostic request-policy core: every branching decision the
 * interceptors and the cookie gate used to make in C++ lives here as
 * pure verdict functions over a compact JSON manifest.  The Qt side
 * only marshals context in and applies verdicts out — no policy
 * branching remains on the adapter.  Everything fails OPEN on errors:
 * a NULL verdict reads as "allow", never as a block.
 *
 * rc_policy_load_snapshot: pushes the privacy snapshot (toggles +
 *   https-only exceptions) from the GUI thread.
 *   JSON {"https_first","https_only","referer_policy","security_level",
 *   "block_pings","block_remote_fonts","block_prefetch",
 *   "block_third_party_ws","strip_tracking_params","domain_blocklist",
 *   "https_only_exceptions":[...]}
 *
 * rc_policy_evaluate: request manifest -> verdict JSON.
 *   in  {"url","first_party_url","resource_type","method",
 *        "headers":[["k","v"],...],"scope","tor_mode",
 *        "script_allowed","min_referer_level"}
 *   out {"action":"pass"}
 *     | {"action":"block","reason":...}
 *     | {"action":"redirect","url":...,"reason":...}
 *     | {"action":"allow","referer":{"op":"keep"|"set","value":...}}
 *   NULL on malformed input — treat as "allow".  rc_string_free().
 *
 * rc_policy_cookie_filter: cookie-gate manifest -> 1 accept / 0 reject
 *   / -1 malformed (fail open).  in {"host","third_party","block_3p",
 *   "accept_policy","block":[],"allow":[],"allow_session":[]}
 *
 * rc_policy_call: generic granular endpoint {"op":..., params} ->
 *   JSON result.  Ops: flag, referrer_meta, rewritten_referer,
 *   referer_apply, upgrade_candidate, warn_http, warn_form_post,
 *   is_http_allowed, allow_http, clear_http_allowance, http_exceptions,
 *   is_downgraded, mark_downgraded, clear_downgraded,
 *   clear_all_downgraded, ttl_get, ttl_set,
 *   failure_implies_downgrade, note_nav_failure, record_blocked_nav,
 *   take_blocked_nav, block_domain, is_domain_blocked,
 *   is_blocked_domain_allowed, allow_blocked_domain,
 *   clear_blocked_domain_allowance, clear_all_blocked_domain,
 *   is_ping, block_resource, block_ws, block_script, private_or_local.
 *   NULL on error — rc_string_free() the result. */
RcStatus rc_policy_load_snapshot(const uint8_t *jsonUtf8, size_t len);
char *rc_policy_evaluate(const uint8_t *jsonUtf8, size_t len);
int rc_policy_cookie_filter(const uint8_t *jsonUtf8, size_t len);
char *rc_policy_call(const uint8_t *jsonUtf8, size_t len);

/* --- untrusted-document parsers (SEC19) ------------------------------
 * The Qt side passes the raw attacker-controlled bytes and receives
 * a JSON document it maps onto its existing types — Qt's XML/JSON
 * parsers only ever see output the crate produced.  All inputs are
 * bounded on both sides; RC_CORRUPT means the document was rejected
 * (rc_last_error_message carries the reader-style reason).
 *
 * rc_opensearch_parse: OpenSearch 1.1 descriptor -> JSON field map
 *   {"name","description","imageUrl",
 *    "search"|"suggestions"|"image":
 *        {"template","method","params":[["k","v"],...]}}
 *   A slot key is absent when no matching <Url> was accepted.
 *
 * rc_updatemanifest_parse: gupdate manifest ->
 *   {"offers":[[appid,status,codebase,version],...]}
 *
 * rc_suggest_parse: OpenSearch suggestions reply -> JSON string array.
 *   RC_CORRUPT when the reply is not the [term, [...]] shape.
 *
 * rc_xbel_check: structural gate for XBEL bookmark documents —
 *   RC_OK means Qt may parse, RC_CORRUPT refuses (wrong root, wrong
 *   version, malformed, oversized, nested beyond the reader bound). */
RcStatus rc_opensearch_parse(const uint8_t *xml, size_t len,
                             RcBuffer *outJson);
RcStatus rc_updatemanifest_parse(const uint8_t *xml, size_t len,
                                 RcBuffer *outJson);
RcStatus rc_xbel_check(const uint8_t *xml, size_t len);
RcStatus rc_suggest_parse(const uint8_t *jsonUtf8, size_t len,
                          RcBuffer *outJson);

/* --- bookmark store (RCORE02a) ---------------------------------------
 * The canonical bookmark tree.  Nodes are addressed by uint64
 * handles; 0 is the null handle and rc_bm_root() is the root.
 * Children are enumerated one node per call — there is deliberately
 * no bulk tree marshal, so models stay O(touched-rows).
 *
 * Node JSON (rc_bm_get / rc_bm_create):
 *   {"type":0..3,"title","url","desc","expanded":bool,"tags":[...]}
 *   type: 0 Root, 1 Folder, 2 Bookmark, 3 Separator (the
 *   BookmarkNode::Type ordinals).
 *
 * detach() unlinks a subtree but keeps it addressable until attach()
 * or destroy() — the undo stack's resurrect path.  Mutations emit the
 * "bookmarks" change topic. */
uint64_t rc_bm_root(void);
/* Missing file = empty store (first run), not an error. */
RcStatus rc_bm_load(const char *pathUtf8);
RcStatus rc_bm_load_mem(const uint8_t *xbel, size_t len);
/* Atomic temp + fsync + rename. */
RcStatus rc_bm_save(const char *pathUtf8);
int64_t rc_bm_child_count(uint64_t node);      /* -1 on bad handle */
uint64_t rc_bm_child_at(uint64_t node, int64_t row);
uint64_t rc_bm_parent(uint64_t node);
char *rc_bm_get(uint64_t node);                /* JSON; rc_string_free */
uint64_t rc_bm_create(uint64_t parent, int64_t row,
                      const char *jsonUtf8);   /* 0 on error */
RcStatus rc_bm_attach(uint64_t parent, int64_t row, uint64_t node);
RcStatus rc_bm_detach(uint64_t node);
RcStatus rc_bm_destroy(uint64_t node);
RcStatus rc_bm_set_title(uint64_t node, const char *valueUtf8);
RcStatus rc_bm_set_url(uint64_t node, const char *valueUtf8);
RcStatus rc_bm_set_desc(uint64_t node, const char *valueUtf8);
RcStatus rc_bm_set_expanded(uint64_t node, int expanded);
/* JSON string array replaces the tag list. */
RcStatus rc_bm_set_tags(uint64_t node, const char *jsonUtf8);
/* Exact-url dedup lookup: first matching bookmark handle, or 0. */
uint64_t rc_bm_find(const char *urlUtf8);

/* --- history store (RCORE02b) -----------------------------------------
 * rusqlite-backed visit log + per-host icon table; every mutation
 * writes through, so the database is always current.  Default path
 * <data dir>/history.db (rc_set_data_dir); an explicit path override
 * exists for tests.
 *
 * The listing collapses consecutive rows identical in
 * (url,title,ts) — the legacy file parser's dedup rule.  Mutations
 * emit the "history" change topic. */
RcStatus rc_hist_open(const char *pathOrNullUtf8);
/* Existence check without opening — gates the legacy-file import. */
int rc_hist_exists(const char *pathOrNullUtf8);
int64_t rc_hist_count(void);                   /* -1 when not open */
/* {"url","title","ts"} of deduped row, newest first; rc_string_free */
char *rc_hist_entry_at(int64_t row);
RcStatus rc_hist_add(const char *urlUtf8, const char *titleUtf8,
                     int64_t tsMs);
RcStatus rc_hist_update_title(const char *urlUtf8,
                              const char *titleUtf8);
RcStatus rc_hist_remove(const char *urlUtf8, const char *titleUtf8,
                        int64_t tsMs);
RcStatus rc_hist_clear(void);

/* Per-host favicons (the HIST01 persistence folded into the core):
 * PNG blobs keyed by host. */
RcStatus rc_hist_icon_set(const char *hostUtf8,
                          const uint8_t *png, size_t len);
RcStatus rc_hist_icon_get(const char *hostUtf8, RcBuffer *out);
RcStatus rc_hist_icon_clear(void);

/* --- session store (RCORE03) ----------------------------------------
 * The canonical session file "<data dir>/session.dat" — a versioned
 * binary schema (magic "ARSS", version 1) written atomically
 * (temp + fsync + rename, 0600) and decoded under strict bounds: a
 * corrupt or truncated file fails RC_CORRUPT whole, so it can never
 * loop the crash-restore prompt or drop part of the window set.
 *
 * The C ABI carries the session as a JSON manifest:
 *   {"version":1,"windows":[{"shell":"<base64>","current":0,
 *     "tabs":[{"url","container","group","engine","state":"<base64>"}],
 *     "groups":[{"id","name","color","collapsed"]}]}]}
 * "shell" is the opaque window-chrome blob (the Qt shell's own
 * serialization), "state" the opaque per-tab engine-state blob
 * (WebEngine's serialized history today; a different engine supplies
 * its own bytes — the format survives the swap).  "container" is the
 * tab's container binding, "engine" its engine tag ("webengine").
 *
 * rc_session_load returns RC_NOT_FOUND when no session exists.
 * rc_session_encode/decode are the pure codec (test seam). */
RcStatus rc_session_save(const uint8_t *jsonUtf8, size_t len);
RcStatus rc_session_load(RcBuffer *outJson);
int rc_session_exists(void);
RcStatus rc_session_clear(void);
RcStatus rc_session_encode(const uint8_t *jsonUtf8, size_t len,
                           RcBuffer *out);
RcStatus rc_session_decode(const uint8_t *blob, size_t len,
                           RcBuffer *outJson);

#define RC_SESSION_FILE "session.dat"

#ifdef __cplusplus
}
#endif

#endif // ARORA_RUSTCORE_H
