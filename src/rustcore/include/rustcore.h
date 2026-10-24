/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#ifndef ARKTOS_RUSTCORE_H
#define ARKTOS_RUSTCORE_H

/*
 * C ABI for libarktos_rustcore — the shared, engine-neutral Rust core
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
    RC_EMPTY_PASSPHRASE = 12,
    RC_WRONG_PASSWORD = 13,     /* vault: password rejected by the wrap */
    RC_TAMPERED = 14,           /* vault: MAC check failed */
    RC_UNSUPPORTED_ALGO = 15    /* vault: unknown algo_id */
} RcStatus;

typedef struct RcBuffer {
    uint8_t *data;
    size_t len;
} RcBuffer;

#define RC_CREDENTIALS_FILE "credentials.dat"
#define RC_AUTOFILL_FILE "autofill-store.dat"

/* Change topics emitted so far: "credentials" (rc_cred_* map),
 * "autofill" (rc_autofill_* record set), "settings" (rc_settings_*
 * mutations), "profiles" (rc_profiles_* registry mutations),
 * "torctl" (RC14: a control reply/event/link-state push is pending),
 * "icons" (RC16: the icon cache changed). */
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

/* --- autofill record store (RCORE05) --------------------------------
 * The saved-form records live in <data dir>/autofill-store.dat — a
 * sealed ARSEC1 blob under the same custody key as credentials.dat:
 * one unlock opens both, passphrase/lock/reseal transitions cover it.
 * The legacy autofill.dat is a read-only import source the Qt side
 * parses itself and replays through rc_autofill_set_forms.
 *
 * Form JSON (both directions):
 *   [{"url","name","has_password":bool,"elements":[["k","v"],...]}]
 * Address JSON (ADDR01/02 — an appended tail in the same payload):
 *   [{"country","name","organization","street","city","state","zip",
 *     "phone","email","last_used":int}]
 * Mutations emit the "autofill" change topic. */
/* Whether the Rust store file exists — gates the legacy import. */
int rc_autofill_store_present(void);
/* Whole record list as a JSON array; rc_string_free.  NULL on error
 * (locked store, corrupt file — rc_last_error_message says which). */
char *rc_autofill_forms(void);
/* Atomic whole-set replace; RC_INVALID_ARGUMENT keeps the old set. */
RcStatus rc_autofill_set_forms(const uint8_t *jsonUtf8, size_t len);
/* Same pair for the saved-address list. */
char *rc_autofill_addresses(void);
RcStatus rc_autofill_set_addresses(const uint8_t *jsonUtf8, size_t len);
/* Re-seal the autofill file between explicit keys mid-transition. */
RcStatus rc_autofill_reseal(const uint8_t *from32, const uint8_t *to32);

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

/* MD03: the SEC05 web-permission broker's decision layer — the
 * feature tables (never-prompted captures/clipboard/fonts vs the
 * ask-capable notifications/geolocation/mouse-lock), the
 * security-origin key normalization, the remembered-decision
 * consult and the session-scoped answer map.  Durable rows live in
 * sitedecisions.json's "webperm" kind under the anchored data dir;
 * the Qt manager keeps the prompt queue and the engine's
 * grant()/deny() — this surface only answers questions.
 *
 * rc_perm_call: generic granular endpoint {"op":..., params} ->
 *   JSON result.  Ops: decide (feature gate -> stored -> session ->
 *   ask), stored (mid-queue recheck), resolve (session-record
 *   always; persist on remember && !otr), remove (row + both
 *   session variants), clear (kind + session map), feature_class,
 *   key_for, session_clear.  NULL on error — callers fail toward
 *   their own local consult, never a silent grant.
 *   rc_string_free() the result. */
char *rc_perm_call(const uint8_t *jsonUtf8, size_t len);

/* MD02: the acceptNavigationRequest-time verdicts — interstitial
 * carrier classification, the browser-handled scheme allowlist, the
 * SEC02 external-handoff gate, per-profile scheme/nav-block
 * overrides, the abp-subscribe rewrite, the staged-pdf viewer
 * clamp, the file: directory reroute, the CONT04 container divert
 * and the sanitize-on-view pdf claim — phase-scoped so the Qt
 * page's kept gates stay in their exact order.  MD09 folds the
 * pop-up gate in too: the createWindow consult (enabled / shaped /
 * gesture / scheme floor / suffix-matched allow list) plus the
 * exception-record ops — persistent rows are sitedecisions.json's
 * "popup" kind, the session overlay is process-global.  The Qt
 * side keeps all execution — nonce resolution, tor holds, tab
 * spawning, dialogs, window/probe creation.
 *
 * rc_nav_decide: one navigation manifest (JSON; {"url","type",
 *   "is_main_frame", "current_url", "current_is_managed",
 *   "target_is_dir", "target_is_managed", "otr", "container",
 *   "container_host", "pdf_viewer", "pdf_sanitize", "gesture",
 *   "phase"}) -> verdict JSON.  "container_host" carries the
 *   caller's canonical rule key (www-folded, root-dots trimmed —
 *   the fold needs the public-suffix table that stays caller-side);
 *   absent means "look the raw url host up", an explicit "" means
 *   "unnormalizable — no rule".  Phases: "pre" | "mid" | "claim";
 *   an absent/unknown phase runs all three and reports the first
 *   non-continue verdict.
 *   NULL on malformed input — the adapter fails OPEN (treat NULL
 *   as "continue", never as a block).
 *
 * rc_nav_call: generic granular endpoint {"op":..., params} ->
 *   JSON result.  Ops: decide, browser_scheme, scheme_class,
 *   rate_limit_interstitial, untrusted_input, page_link,
 *   staged_viewer, nav_rule, popup_decide, popup_is_allowed,
 *   popup_allow, popup_remove, popup_list, popup_clear,
 *   popup_clear_session.  NULL on error — rc_string_free() the
 *   result. */
char *rc_nav_decide(const uint8_t *jsonUtf8, size_t len);
char *rc_nav_call(const uint8_t *jsonUtf8, size_t len);

/* PERF01: monotonic verdict-state generation — bumped by every
 * mutation that could change an evaluate() verdict (snapshot loads,
 * downgrade/allowance writes, urlstrip + blocklist swaps, data-dir
 * switches).  The Qt-side verdict cache stamps entries with it; a
 * stale stamp is never served. */
uint64_t rc_policy_eval_generation(void);

/* --- site-rank table (TRUST04 + SEC19) ------------------------------
 * The top-site rank table the site-trust signals consult — a lookup
 * never leaves the machine.  The active table merges three sources:
 * the vendored Tranco top-100k seed (compiled in), "<data dir>/
 * tranco-list.dat" (the remote Tranco top-1M — the top-1m.csv.zip
 * body the crate unzips) and "<data dir>/majestic-million.csv" (the
 * Majestic Million second source — a plain GlobalRank,TldRank,
 * Domain,... CSV the crate reads directly), both written by the
 * Qt-side TrancoList updater.  The merge is best-rank-wins: a
 * domain's effective rank is the lowest any list assigns, so an
 * update can only ever add or improve coverage, never un-rank.
 *
 * rc_tranco_rank returns the host's rank — the exact host first,
 * then the most specific listed ancestor (the registrable-domain
 * probe).  0 means unlisted; bad pointers and malformed hosts read
 * as unlisted too — the check never breaks on an FFI hiccup. */
uint32_t rc_tranco_rank(const char *hostUtf8);

/* Swap in a caller-supplied "rank,domain" table body (update/test
 * seam).  RC_CORRUPT when the body yields no usable entries; the
 * previous table stays active. */
RcStatus rc_tranco_load(const uint8_t *textUtf8, size_t len);

/* Re-run the seed-union-overrides merge.  RC_CORRUPT when an
 * override file exists but yields no usable entries — previous
 * table stays active. */
RcStatus rc_tranco_reload(void);
size_t rc_tranco_count(void);

/* Usable-entry count of one data-dir override file
 * ("tranco-list.dat", "majestic-million.csv"), parsed under the
 * grammar its name selects — the updater's per-file validation
 * probe before a reload.  0 when the file is absent, unreadable,
 * oversized or yields nothing (a NULL name reads as 0). */
size_t rc_tranco_file_entries(const char *nameUtf8);

/* --- Homograph lookalike detector (TRUST05) -------------------------
 * IDN/homograph verdict: the host is punycode-decoded, case-folded
 * and mapped through the UTS #39 confusable table, then probed
 * against the Tranco table's skeleton index.  It flags only when the
 * skeleton coincides with a DIFFERENT popular domain — benign IDNs
 * resembling nothing popular stay clean.
 *
 * Returns a JSON string (free with rc_string_free), or NULL on a bad
 * pointer:
 *   {"verdict":"lookalike","lookalike_of":"<ranked domain>",
 *    "mixed_script":<bool>,"skeleton":"<skeleton>"}
 *   {"verdict":"clean","mixed_script":<bool>,"skeleton":"..."}
 * This is a warning signal for the site-trust composite (TRUST01),
 * never a block decision on its own. */
char *rc_lookalike_check(const char *hostUtf8);

/* --- site-trust composite (TRUST01) ----------------------------------
 * The WOT-style trust verdict the lock-icon badge and its popover
 * consume — advisory only, it NEVER blocks.  Inputs: the domain
 * blocklist, the Tranco rank table, the homograph lookalike probe,
 * the caller-passed https state and the history store's first-seen
 * rollup.
 *
 * rc_sitetrust_lookup returns a JSON string (free with
 * rc_string_free), or NULL on a bad pointer:
 *   {"level":"green"|"amber"|"red","signals":[...],"rank":N,
 *    "lookalike_of":"..."}
 * Level mapping: a threat-list hit or a confusable∩popular twin is
 * "red"; plaintext http or an unknown Tranco rank is "amber";
 * Tranco-known + https is "green".  signals[] reports each input
 * that evaluated true — "threat", "lookalike", "rank", "first_seen"
 * (no prior visit in history) — and "lookalike_of" appears only
 * when the lookalike probe fired.  first_seen is informational: it
 * never decides the level alone. */
char *rc_sitetrust_lookup(const char *hostUtf8, int isHttps);

/* Probe-selection bits for rc_sitetrust_lookup_opts — TRUST03's
 * per-signal Preferences switches.  A cleared bit skips that
 * advisory probe entirely: it never runs and cannot appear in
 * signals[].  The rank and first-seen inputs are ungated. */
#define RC_SITETRUST_PROBE_THREATLIST 0x1u
#define RC_SITETRUST_PROBE_LOOKALIKE  0x2u
#define RC_SITETRUST_PROBE_ALL \
    (RC_SITETRUST_PROBE_THREATLIST | RC_SITETRUST_PROBE_LOOKALIKE)

/* Same verdict as rc_sitetrust_lookup with the probes bitmask
 * applied — the threat-list and lookalike inputs each answer their
 * own Preferences switch. */
char *rc_sitetrust_lookup_opts(const char *hostUtf8, int isHttps,
                               unsigned int probes);

/* Re-run both input-table merges (blocklist + Tranco) — the list
 * updaters call this after dropping a new file.  Both reloads run
 * even when the first fails; the first error is reported. */
RcStatus rc_sitetrust_reload(void);

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
 *   {"offers":[[appid,status,codebase,version,hash_sha256],...]}
 *   (hash_sha256 is "" when the offer doesn't declare one)
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

/* --- PDF sanitization (PDF01) --------------------------------------
 * The built-in viewer only ever opens the rewrite this produces: the
 * Qt side fetches the PDF body through the profile, calls this and
 * displays the sanitized copy from a 0700 temp dir.  Strips the
 * action surface the renderer sandbox does not policy — /JS and
 * /JavaScript payloads, /Launch, /AA and /OpenAction triggers,
 * /Names->/EmbeddedFiles attachments, AcroForm /XFA and remote
 * submit/import actions — then re-serializes a standalone PDF.
 * RC_CORRUPT when the input is not a parseable PDF: the caller
 * refuses the view rather than showing unsanitized bytes. */
RcStatus rc_pdf_sanitize(const uint8_t *data, size_t len,
                         RcBuffer *out);

/* --- pdf document rasterization (PDF02) -----------------------------
 * The shell-owned document view renders pages through this surface,
 * normally inside the `arktos --pdf-worker` confined subprocess (one
 * document per worker).  The same calls work in-process as the
 * fallback when no sandbox backend exists.
 *
 * rc_pdfdoc_open returns an opaque handle (0 = failure, error text
 * via rc_last_error_message).  The handle is single-threaded: one owner,
 * rc_pdfdoc_close releases it. */
uintptr_t rc_pdfdoc_open(const uint8_t *data, size_t len);
/* Page count, 0 on a bad handle. */
int32_t rc_pdfdoc_page_count(uintptr_t doc);
/* Page dimensions in PDF points (1/72in); RC_INVALID_ARGUMENT when
 * the handle or page index is bad. */
RcStatus rc_pdfdoc_page_size(uintptr_t doc, int32_t page,
                             double *w, double *h);
/* Document /Title or NULL — rc_string_free the result. */
char *rc_pdfdoc_title(uintptr_t doc);
/* Page rasterized to PNG bytes (rc_buffer_free) — scale 1.0 = 72dpi.
 * RC_CORRUPT on render failure, RC_INVALID_ARGUMENT on a bad
 * handle/page/scale or a bitmap over the pixel cap. */
RcStatus rc_pdfdoc_render_png(uintptr_t doc, int32_t page,
                              double scale, RcBuffer *out);
/* Page index holding the next/prev `needle` match from `from_page`
 * (dir >= 0 forward, < 0 backward, wraps once), -1 on no match or a
 * bad handle.  Text is page-granular.  out_total may be NULL; when
 * given it receives the document-wide occurrence count. */
int32_t rc_pdfdoc_find(uintptr_t doc, const char *needleUtf8,
                       int32_t from_page, int32_t dir,
                       int32_t *out_total);
void rc_pdfdoc_close(uintptr_t doc);

/* --- reader-mode extraction (RDR01) ----------------------------------
 * The article extraction that used to run as injected JS (vendored
 * Mozilla Readability.js + isProbablyReaderable) lives in the core —
 * the feature no longer needs page-side script eval, so a JS-one-way
 * engine supports reader mode too.  The Qt side hands over the
 * serialized DOM and renders the returned fragment in its own reader
 * surface.
 *
 * rc_readability_probe: 1 when the document looks article-like
 * (isProbablyReaderable), 0 otherwise — bad input reads as "not an
 * article", never an error.  Input bound: 24 MiB.
 *
 * rc_readability_extract: JSON verdict to outJson (rc_buffer_free):
 *   {"ok":bool, "probably":bool, "title", "byline", "siteName",
 *    "excerpt", "dir", "lang", "length":n, "content":"..."}
 * "ok" mirrors the JS enter() contract — false when the page is not
 * article-like or the extractor found nothing; "content" is populated
 * only when ok and arrives re-sanitized against the reader-view
 * ruleset (dead tags removed, on* and javascript: attributes
 * stripped).  base_url is the page's absolute URL (NULL allowed;
 * non-authority URLs degrade to "no base").  RC_INVALID_ARGUMENT on
 * bad pointers or oversized input. */
int rc_readability_probe(const uint8_t *html, size_t len);
RcStatus rc_readability_extract(const uint8_t *html, size_t len,
                                const char *base_urlUtf8,
                                RcBuffer *outJson);

/* --- extension-package verification (EXT06) --------------------------
 * The untrusted-bytes boundary of the extension system: downloaded
 * packages (.zip/.crx) and update payloads are verified in Rust
 * before Qt's installer sees a byte.
 *
 * rc_ext_verify_package consumes the package in memory and writes a
 * JSON verdict to outJson (rc_buffer_free).  RC_OK means a verdict
 * was produced — the JSON carries:
 *   {"status":"valid"|"rejected", "error":reason|null,
 *    "format":"zip"|"crx3"|null,
 *    "signing":"unsigned"|"signed"|"unknown",
 *    "signature_check":"none"|"structure-only",
 *    "pinning":"not-requested"|"no-pinning-configured",
 *    "crx_id":hex|null, "sha256":hex|null,
 *    "expected_sha256":"match"|"mismatch"|"not-declared",
 *    "entries":n, "uncompressed_total":n,
 *    "manifest":<rc_ext_manifest_check verdict>|null}
 * An "unsigned" CRX3 is a classification, not a failure.  Signature
 * verification itself is v2 scope — v1 verifies the header's protobuf
 * structure only (honest "structure-only"); a caller-supplied pinned
 * pubkey reports "no-pinning-configured".  expected_sha256 is the
 * update manifest's declared digest (NULL when undeclared); a
 * mismatch rejects the package.  Zip checks: EOCD/CD bounds,
 * multi-disk + zip64 refusal, entry-count and uncompressed-size caps,
 * local-header verification and traversal/absolute/drive-letter
 * member-name rejection.
 *
 * rc_ext_manifest_check parses a manifest.json document (<= 1 MiB)
 * into the classification the review dialog consumes:
 *   {"valid","manifest_version","name","version","description",
 *    "update_url","key","has_background","has_action",
 *    "content_script_count","permissions":[...],"host_permissions":[...],
 *    "unsupported":[...],"unverified":[...],"dangerous":[...],
 *    "errors":[...]}
 * RC_CORRUPT means the bytes are not readable JSON — field problems
 * land in "errors". */
RcStatus rc_ext_verify_package(const uint8_t *pkg, size_t len,
                               const uint8_t *expected_sha256_or_null,
                               const uint8_t *pinned_pubkey_or_null,
                               size_t pubkey_len,
                               RcBuffer *outJson);
RcStatus rc_ext_manifest_check(const uint8_t *json, size_t len,
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

/* --- bookmark HTML import/export (BKIMP01) ----------------------------
 * rc_bm_import_html parses a Netscape bookmark document in-memory —
 * the DL/DT/H3/A/HR grammar of tools/htmlToXBel, bounded scans, no
 * XML detour — and inserts the tree under `parent` (a root or folder
 * handle).  Every inserted bookmark passes the store's url-scheme
 * policy: http/https/ftp/mailto and relative urls pass; javascript:
 * passes only with RC_BM_IMPORT_BOOKMARKLETS (the caller's bookmarklet
 * consent — the same gate Bookmarks::create applies to every insert);
 * every other scheme is refused and counted.  Folders merge by title
 * under the same parent and same-url bookmarks dedup, so re-running
 * the same document is idempotent.
 *
 * Returns a JSON summary {"imported","folders","separators",
 * "duplicates","skipped","skipped_by_scheme":{scheme:count},
 * "truncated"} — free with rc_string_free(); NULL on error
 * (rc_last_error_message).  RC_BM_IMPORT_DRY_RUN runs the full scan +
 * accounting without inserting — the consent-probe pass a caller runs
 * before creating anything. */
#define RC_BM_IMPORT_BOOKMARKLETS 0x1
#define RC_BM_IMPORT_DRY_RUN      0x2
char *rc_bm_import_html(uint64_t parent, const uint8_t *html, size_t len,
                        int flags);          /* JSON; rc_string_free */

/* Writes the tree in the standard NETSCAPE-Bookmark-file-1 shape to
 * `path` — atomic temp + fsync + rename, mode 0600 on unix (exports
 * can carry token-bearing urls). */
RcStatus rc_bm_export_html(const char *pathUtf8);

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
/* Deletes every visit at or after sinceMs — the ranged leg of a
 * clear-private-data plan (CLR01).  Icons are kept. */
RcStatus rc_hist_clear_since(int64_t sinceMs);

/* Deletes every visit at or before cutoffMs — the bulk retention
 * expiry behind HistoryManager's daysToExpire rule (the caller
 * computes the local-time boundary).  Icons are kept.  outRemoved
 * may be NULL; when given it receives the deleted row count. */
RcStatus rc_hist_expire_older_than(int64_t cutoffMs,
                                   int64_t *outRemoved);

/* Filtered listing — the store-side query surface (RC04).  specJson
 * is an object, all fields optional:
 *   "term"     case-insensitive substring on url OR title (the
 *              history dialog's all-columns fixed-string match)
 *   "from_ms"  inclusive visited_at lower bound (ms-epoch)
 *   "until_ms" inclusive visited_at upper bound
 *   "limit"    max rows returned — absent or <0 is unbounded
 *   "offset"   rows skipped after filtering
 * Returns a JSON array [{"url","title","ts"}, ...] in the same
 * deduped newest-first order rc_hist_entry_at iterates.  NULL on a
 * malformed spec or an unopened store; rc_string_free. */
char *rc_hist_query(const char *specJsonUtf8);

/* Per-host favicons (the HIST01 persistence folded into the core):
 * PNG blobs keyed by host. */
RcStatus rc_hist_icon_set(const char *hostUtf8,
                          const uint8_t *png, size_t len);
RcStatus rc_hist_icon_get(const char *hostUtf8, RcBuffer *out);
RcStatus rc_hist_icon_clear(void);

/* --- session store (RCORE03; per-profile + lifecycle in RC13) --------
 * The canonical session file "<profile dir>/session.dat" — a versioned
 * binary schema (magic "ARSS", bin version 4) written atomically
 * (temp + fsync + rename, 0600) and decoded under strict bounds: a
 * corrupt or truncated file fails RC_CORRUPT whole, so it can never
 * loop the crash-restore prompt or drop part of the window set.
 * RC13 makes the store per-profile: anchor the data dir to the
 * persona's profile dir (rc_set_data_dir) and each rc_session_* op
 * lands on that persona's own file.
 *
 * The C ABI carries the session as a JSON manifest:
 *   {"version":1,"windows":[{"shell":"<base64>","current":0,
 *     "tabs":[{"url","container","group","engine","state":"<base64>",
 *              "pinned","title","scroll":{"x","y"}}],
 *     "groups":[{"id","name","color","collapsed"]}],
 *     "profile","activeContainer"}]}
 * "shell" is the opaque window-chrome blob (the Qt shell's own
 * serialization), "state" the opaque per-tab engine-state blob
 * (WebEngine's serialized history today; a different engine supplies
 * its own bytes — the format survives the swap).  "container" is the
 * tab's container binding, "engine" its engine tag ("webengine"),
 * "scroll" its recorded viewport position (v4, optional).
 * "private":true on a window or tab is a strip marker — private data
 * is never persisted; the flagged record drops out of the stored
 * blob entirely.
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

/* --- session lifecycle marker (RC13) ---------------------------------
 * "<profile dir>/session.state" — the clean-shutdown flag +
 * crash-dirty record + restore-loop guard, so "restore previous
 * session?" works identically on every engine.  Mirrors
 * BrowserProfile's markSession* sentinels; a still-armed marker at
 * boot means the previous run never recorded an exit.
 *
 * rc_session_mark_start arms this run's marker and returns 1 when
 * the previous run died uncleanly (crash counter bumped, "unclean"
 * breadcrumb written), 0 otherwise, -1 on error.
 * rc_session_last_exit yields "clean rc=N", "terminated by signal N
 * (NAME)" or "unclean (crash or external kill)" — RC_NOT_FOUND when
 * no exit was ever recorded. */
int      rc_session_mark_start(void);
RcStatus rc_session_mark_exit(int32_t code);
RcStatus rc_session_mark_terminated(int32_t signal);
int      rc_session_dirty(void);
int      rc_session_crash_count(void);
RcStatus rc_session_last_exit(RcBuffer *out);
RcStatus rc_session_set_restoring(int on);
int      rc_session_restoring(void);

#define RC_SESSION_STATE_FILE "session.state"

typedef struct RcSettings RcSettings;

/* --- session snapshot/restore policy (MD08) -----------------------------
 * The session pipeline's decision layer — a read-only typed view
 * over the bound profile's settings store ("session/" prefix).
 * `h` is the bound profile's RcSettings (rc_settings_open, or the
 * ac_session_policy per-op open); NULL is legal for the "defaults"
 * op only — the other reads degrade to the compiled-in constants on
 * an absent store, except "stale" which genuinely needs the dir.
 * Manifest grammar:
 *   {"op":"defaults"}                 -> the resolved default set
 *   {"op":"policy"}                   -> the whole resolved set
 *   {"op":"snapshot"}                 -> {"interval_ms","maxwait_ms"}
 *   {"op":"dirty","kind":<k>}         -> {"dirty":b,"kind":k}; kinds
 *                                        "tabs"|"chrome"|"sidebar"|
 *                                        "lifecycle", unknown kinds
 *                                        answer true (parity with
 *                                        the old every-change-dirty)
 *   {"op":"retention"}                -> {"max_age_days","max_windows",
 *                                        "max_tabs_per_window"}
 *                                        (0 = no limit)
 *   {"op":"stale"}                    -> {"stale":b,"age_ms":n|null,
 *                                        "max_age_days":n} — reads
 *                                        session.dat's mtime under
 *                                        the bound scope
 *   {"op":"restore"}                  -> {"startup_default":int,
 *                                        "crash_restore":"ask"|
 *                                        "always"|"never"}
 *   {"op":"lifecycle"}                -> {"suspend_idle_ms":int,
 *                                        "memory_purge":b,
 *                                        "lazy_restore":b,
 *                                        "lazy_restore_pinned":b} —
 *                                        the tab-lifecycle rows the
 *                                        Settings dialog owns under
 *                                        tabs/ + performance/,
 *                                        resolved with the same
 *                                        defaults + arithmetic the
 *                                        C++ ran
 * Store keys: session/snapshotIntervalMs, session/snapshotMaxWaitMs,
 * session/dirtyOn{Tabs,Chrome,Sidebar,Lifecycle}, session/maxAgeDays,
 * session/maxWindows, session/maxTabsPerWindow, session/crashRestore,
 * session/startupModeDefault — plus the lifecycle op's Qt-owned rows
 * tabs/suspendTabs, tabs/suspendTabsMinutes, tabs/memoryPurge,
 * performance/lazyRestoreTabs, performance/alwaysLoadPinnedTabs —
 * all optional; the compiled-in
 * constants (3000/15000 ms, every class dirty, no caps, "ask",
 * StartPageStartup) are the defaults so a missing or mistyped row
 * can never change behavior.  NULL return on malformed input or a
 * store-required op without one — the caller keeps its compiled-in
 * consult, never guesses.  Read-only: no change topic is emitted.
 * Free the result with rc_string_free(). */
char    *rc_sesspolicy_call(const RcSettings *h,
                            const uint8_t *json, size_t len);

/* --- DEVT03 devtools client ----------------------------------------
 *
 * Engine-neutral devtools channel: callers speak WebDriver-BiDi-
 * shaped commands (session.new, session.subscribe,
 * browsingContext.getTree/navigate, script.evaluate,
 * storage.getCookies); the WebEngine backend translates them onto
 * CDP inside the crate (Qt's Chromium ships no BiDi mapper), so a
 * future Servo backend can swap in a native BiDi transport without
 * touching callers.
 *
 * The callback fires on an internal thread for:
 *   kind="response"  id=<bidi id>   json={"result":..}|{"error":{..}}
 *   kind="event"     id=0           json={"method":..,"params":..}
 *   kind="state"     id=0           json={"state":"closed"}
 * It must be thread-safe and non-blocking (queue to the GUI thread).
 *
 * `path` is the ws path ("/devtools/browser/<guid>"); `origin` is the
 * per-session allow-origins token arming the debug server. */
typedef struct RcBidiClient RcBidiClient;
typedef void (*RcBidiCallback)(void *userdata, const char *kindUtf8,
                               uint64_t id, const char *jsonUtf8);
RcBidiClient *rc_bidi_connect(const char *hostUtf8, uint16_t port,
                              const char *pathUtf8,
                              const char *originUtf8,
                              RcBidiCallback cb, void *userdata);
uint64_t rc_bidi_command(RcBidiClient *client, const char *methodUtf8,
                         const char *paramsJsonUtf8);
int rc_bidi_is_alive(const RcBidiClient *client);
void rc_bidi_free(RcBidiClient *client);

/* --- omnibox (OMNI01) -------------------------------------------------
 * The location-bar routing decision and the frecency ranking the
 * completion dropdown displays — ported logic, the Qt side thin-shells.
 *
 * rc_classify_input(input, optionsJson) -> verdict JSON:
 *   {"kind":"navigate","url":...}            load this url (web/ftp)
 *   {"kind":"internal","url":...}           explicit non-web scheme
 *                                           (about:, mailto:, qrc:…)
 *   {"kind":"file","path":...}              local filesystem path —
 *                                           build with QUrl::fromLocalFile
 *   {"kind":"search","engine":null|<kw>,
 *     "query":...}                          resolve through the context
 *                                           engine, or keyword kw's
 * optionsJson: {"keywords":[...],"search_fallback":bool} — the live
 * keyword set and the urlloading/searchInAddressField opt-in.
 * NULL return is an argument error, not an empty verdict.
 *
 * rc_frecency_score(json, out) — pure scoring seam:
 *   {"visits":[...],"now_ms":...,"typed":n,"bookmarked":bool}
 *   (now_ms optional; visits are visit stamps in ms-epoch, bucketed
 *   by local calendar days like QDateTime::daysTo).
 *
 * rc_history_suggest(term, limit) -> JSON array
 *   [{"url","title","ts","frecency","score"}, ...] ranked rows from the
 *   history store, newest-title representative per url; NULL when the
 *   store is not open.
 *
 * rc_history_top_hosts(limit) -> JSON array
 *   [{"host","url","title","visits","first_seen"}, ...] — the hosts
 *   rollup ranked by visit count (MD27 chrome.topSites), newest
 *   visit on the host supplying the representative url/title; NULL
 *   when the store is not open. */
char *rc_classify_input(const char *inputUtf8,
                        const char *optionsJsonUtf8);
RcStatus rc_frecency_score(const char *jsonUtf8, int64_t *out);
char *rc_history_suggest(const char *termUtf8, int64_t limit);
char *rc_history_top_hosts(int64_t limit);

/* --- command palette matching (CPAL01) --------------------------------
 * The query -> ranked-items decision of the command palette.  The row
 * registry stays Qt-side (palette rows are QActions, WebViews and
 * bookmark handles) — the shell marshals each row's match text and
 * stable id in and reads ordered indices back.
 *
 * rc_pal_score is the subsequence fuzzy scorer (the
 * CommandPalette::fuzzyScore port, plus a camel-hump word-boundary
 * bonus the lowered-haystack reference could not see): >= 0 matches,
 * higher is better, -1 is no match or a bad pointer.
 *
 * rc_pal_match ranks a whole item set in one call:
 *   request {"items":[{"match":"...","id":"..."},...],
 *            "mru":["id",...]}       (the MRU id list, recent first)
 *   -> JSON [{"index":n,"score":n},...]   matched rows in display
 *      order — score descending, ties keep item order; an id found
 *      at mru position k adds the legacy 60-k recency boost.
 * NULL on a bad pointer or malformed request
 * (rc_last_error_message); free the result with rc_string_free. */
int64_t rc_pal_score(const char *queryUtf8, const char *candidateUtf8);
char *rc_pal_match(const char *queryUtf8, const char *itemsJsonUtf8);

/* --- QR encoding (QRC01) ---------------------------------------------
 * Nayuki's qrcodegen-rs behind the ABI — the same code lineage as the
 * vendored C++ qrcodegen it replaces, so the matrix is bit-identical.
 *
 * rc_qr_encode(text, eccLevel, out) encodes UTF-8 text at eccLevel
 * (0=Low 1=Medium 2=Quartile 3=High — the qrcodegen Ecc ordinals) and
 * fills *out (free with rc_buffer_free) with:
 *   u32le size, then size*size module bytes, row-major, 1 = dark,
 *   0 = light, no quiet zone (renderers add the spec's 4 modules).
 * RC_INVALID_ARGUMENT on a bad pointer, out-of-range level or an
 * over-capacity payload — rc_last_error_message says which. */
RcStatus rc_qr_encode(const char *textUtf8, int32_t eccLevel,
                      RcBuffer *out);

/* --- second-opinion TLS verification (SEC22) -------------------------
 * rc_tls_check performs a REAL blocking TLS handshake to host:port
 * (TLS 1.2/1.3, ALPN "http/1.1", SNI=host, 5 s connect / 5 s per-io /
 * 15 s total) and evaluates the chain with rustls+webpki against the
 * platform root store plus any rc_tls_add_root anchors — independent
 * of whatever the engine decided.  Run it on a worker thread, never
 * the UI thread.
 *
 * Returns a JSON verdict (free with rc_string_free; NULL only on a
 * bad host pointer):
 *   {"status": "verified"|"warning"|"unverified"|"refused",
 *    "error_class": <class>|null, "detail": "...",
 *    "host": "...", "port": n, "revocation": "not-checked",
 *    "negotiated": {"tls_version","cipher_suite","alpn"}|null,
 *    "roots": {"native":n,"extra":m},
 *    "chain": [{"subject","issuer","serial","not_before","not_after",
 *               "signature_algorithm","sans":[...]}]}
 *
 * status / error_class vocabulary:
 *   verified    — handshake completed, chain validated (class null)
 *   warning     — chain evaluated and FAILED:
 *                 expired, not-yet-valid, bad-hostname,
 *                 untrusted-root, broken-chain, weak-signature, revoked
 *   unverified  — chain never evaluated; NOT a bad-chain signal:
 *                 network (dns/connect/timeout/refused/eof),
 *                 protocol (TLS-level failure before the chain),
 *                 root-store (no trust anchors usable)
 *   refused     — probe declined: private-host (loopback/private/LAN/
 *                 .onion without RC_TLS_F_ALLOW_LOCAL), invalid-host,
 *                 invalid-port
 *
 * The probe is a DIRECT handshake: the Qt side must not issue it for
 * tor-mode pages or when the app's traffic rides a proxy — a direct
 * connection would bypass SOCKS and de-anonymize the user.
 * Revocation is not checked in v1 (no OCSP/CRL fetch — reported
 * verbatim as "not-checked"). */
#define RC_TLS_F_ALLOW_LOCAL 0x01u   /* permit private/loopback
                                        targets (test fixture seam) */
char *rc_tls_check(const char *hostUtf8, uint16_t port, uint32_t flags);

/* Extra trust anchors consulted by later probes — the fixture-CA test
 * seam and the hook for enterprise/user roots.  RC_CORRUPT on non-DER
 * input; the platform store is unaffected by either call. */
RcStatus rc_tls_add_root(const uint8_t *der, size_t len);
RcStatus rc_tls_clear_roots(void);

/* ------------------------------------------------------------------ */
/* SITED01: consolidated per-site decision store                        */
/*                                                                      */
/* One durable, engine-neutral store for every "decision for a site"  */
/* record: web-permission grants, JavaScript rules, container           */
/* assignments, pop-up exceptions, HTTPS-only allowances, cookie        */
/* rules and adblock whitelist entries.  Rows are                       */
/*   (kind, host-or-origin key) -> small decision value                 */
/* persisted atomically to sitedecisions.json under the data dir.       */
/* Mutations emit the "sitedecisions" notification topic.               */

/* Fetch the decision value for (kind, key) into *out (caller frees
 * with rc_buffer_free).  RC_OK on a hit, RC_NOT_FOUND on a miss. */
RcStatus rc_sitedec_get(const char *kind, const char *key,
                        RcBuffer *out);

/* Record value under (kind, key) — persists atomically. */
RcStatus rc_sitedec_set(const char *kind, const char *key,
                        const char *value);

/* Drop the row for (kind, key); RC_OK whether or not it existed. */
RcStatus rc_sitedec_remove(const char *kind, const char *key);

/* Remove every row of kind. */
RcStatus rc_sitedec_clear(const char *kind);

/* Replace kind's rows wholesale with a {"key":"value"} JSON object —
 * the mirror-write used by list-shaped stores. */
RcStatus rc_sitedec_replace(const char *kind, const uint8_t *json,
                            size_t len);

/* Every row of kind as a {"key":"value"} JSON object — caller frees
 * with rc_string_free(); NULL on bad arguments. */
char *rc_sitedec_list(const char *kind);

/* The whole store as
 * {"version":1,"generation":N,"kinds":{"kind":{"key":"value"}}} —
 * the IO-thread policy snapshot.  Caller frees with rc_string_free(). */
char *rc_sitedec_snapshot(void);

/* Host-suffix lookup: {"key":..,"value":..} JSON for the longest
 * stored host suffix matching host, or NULL when nothing matches. */
char *rc_sitedec_lookup(const char *kind, const char *host);

/* 1 when the store file exists — the legacy-import gate. */
int rc_sitedec_store_present(void);

/* Re-read the disk file into memory. */
RcStatus rc_sitedec_reload(void);

/* Empty every kind (store file removed). */
RcStatus rc_sitedec_reset(void);

/* ------------------------------------------------------------------ */
/* RC15: per-profile cookie database                                   */
/*                                                                      */
/* The Servo/WPE jar's persistent home — and Chromium's durable       */
/* mirror (Chromium's QWebEngineCookieStore stays engine-owned; a     */
/* mirror-role db is the honest "the engine owns the jar" shadow).    */
/* Storage is cookies.db under the profile dir (or the anchored data  */
/* dir on this surface), SQLite/WAL like history.db, partitioned by   */
/* container id ("" = the default context).                            */
/*                                                                      */
/* Records travel as JSON — {"container":"","name":"","value":"",    */
/* "domain":"","path":"/","expires":ms|null,"secure":b,"httpOnly":b, */
/* "sameSite":0..3,"hostOnly":b,"created":ms} — the Qt-verified field */
/* set SiteDecisionStore policy and send-matching need.  Domains      */
/* normalize (lowercase, no leading dot); the leading-dot-vs-exact    */
/* distinction lives in hostOnly, Qt-verbatim.  Mutations emit the    */
/* "cookies" change topic.                                            */
/*                                                                      */
/* Opens (or re-asserts) the db.  path NULL resolves to               */
/* <data dir>/cookies.db; an explicit path binds a profile dir.       */
RcStatus rc_cookies_open(const char *path_or_null);

/* 1 while a db is open. */
int rc_cookies_is_open(void);

/* The declared mode: 0 authoritative (this file IS the jar —        */
/* Servo/WPE), 1 mirror (an engine owns the jar — Chromium's         */
/* durable shadow), -1 on error.  Read from the db's meta table.     */
int64_t rc_cookies_role(void);

/* Declare the mode (0 or 1) — persisted so a db reports what it     */
/* last served.  The ops are identical either way; the flag is the   */
/* honesty contract, not hidden behavior.                            */
RcStatus rc_cookies_set_role(int64_t role);

/* Upsert one record JSON (shape above; `len` bytes at `json`).      */
/* An already-past expiry deletes the row (the Set-Cookie eviction   */
/* rule); an identical rewrite is a no-op (no notification).         */
RcStatus rc_cookies_set(const uint8_t *json, size_t len);

/* One cookie by key into *out (rc_buffer_free the record JSON).     */
/* RC_NOT_FOUND on a miss; container NULL = the default partition.   */
RcStatus rc_cookies_get(const char *domain, const char *name,
                        const char *path, const char *container_or_null,
                        RcBuffer *out);

/* Delete one cookie by key — RC_OK either way. */
RcStatus rc_cookies_remove(const char *domain, const char *name,
                           const char *path,
                           const char *container_or_null);

/* A partition's rows (NULL container = all) as a JSON array of      */
/* records — caller frees with rc_string_free().                     */
char *rc_cookies_list(const char *container_or_null);

/* The send-match: which cookies ride a request.  `request` is JSON  */
/* {"url":"https://h/p","container":"","first_party":"h"|null,       */
/* "now_ms":0} — first_party engages the SameSite cross-site         */
/* filter; now_ms 0 = system clock.  Returns a JSON array in         */
/* RFC6265 §5.4 send order — caller frees with rc_string_free().     */
char *rc_cookies_for_url(const uint8_t *request, size_t len);

/* Parse + store one Set-Cookie header value against `url` — the     */
/* authoritative backend's ingest lane.  Returns a verdict JSON      */
/* {"stored":bool,"reason":...} — caller frees with rc_string_free().*/
char *rc_cookies_store_header(const char *url, const char *header,
                              const char *container_or_null);

/* The cookie-policy gate — may this attempt proceed, and with what  */
/* rewrites.  Consults the sibling sitedecisions.json "cookie" kind  */
/* (the file CookieJar's exceptions already persist through) plus    */
/* the caller's policy snapshot.  Input JSON:                        */
/*   {"host":"h","third_party":b,"block_3p":b,                      */
/*    "accept_policy":0|1|2 (always/never/first-party),              */
/*    "keep_policy":0|1|2,"session_length":days(-1 unset),           */
/*    "filter_tracking":b,"name":"n","session":b,"expires_ms":ms}    */
/* Output: {"accept":b,"rule":"block"|"tracking"|"third-party"|      */
/*          "allow"|"session"|"policy","session_only":b,             */
/*          "expires_cap_ms":ms|null} — caller frees.                */
char *rc_cookies_gate(const uint8_t *input, size_t len);

/* Import a QNetworkCookie::toRawForm file — one raw line per row,   */
/* the shape the vendored NetworkCookieJar saveState streams.        */
/* Lines lacking a Domain attribute are skipped (no request host to  */
/* key them on).  Returns {"imported":N,"skipped":M} — caller frees. */
char *rc_cookies_import(const char *path,
                        const char *container_or_null);

/* Export the partition (NULL container = all) as toRawForm lines —  */
/* the same file shape rc_cookies_import reads back.  Atomic write.  */
RcStatus rc_cookies_export(const char *path,
                           const char *container_or_null);

/* Wipe a partition (NULL container = the whole db). */
RcStatus rc_cookies_clear(const char *container_or_null);

/* The "clear site data" sweep — drops rows scoped to the host or a  */
/* subdomain it controls, plus rows on a parent domain the site      */
/* still receives (CookieJar::removeCookiesForHost verbatim).        */
RcStatus rc_cookies_remove_for_host(const char *host,
                                    const char *container_or_null);

/* Row count — a partition, or the whole db on NULL.  -1 on error. */
int64_t rc_cookies_count(const char *container_or_null);

/* Delete rows expired at/before now_ms. */
RcStatus rc_cookies_purge_expired(int64_t now_ms);

/* The exit-sweep: drop every session row (also run at open — a     */
/* persistent db can only have inherited them from a dead run).     */
RcStatus rc_cookies_purge_session(void);

/* ------------------------------------------------------------------ */
/* RC16: per-profile site-icon/favicon cache                           */
/*                                                                      */
/* Icon records keyed by page-url/origin, pointing at PNG files      */
/* under the profile's CACHE dir (<data dir>/cache/icons/ on this    */
/* anchored surface — profiles/<id>/cache/icons for the facade).     */
/* The cache is disposable by contract: never stored under the data  */
/* root proper.  Fetch stays Qt/engine-side (the page already has    */
/* the bytes); this store owns the index, dedup-by-host file naming, */
/* the expiry sweep and the file lifecycle.  On a profile's first    */
/* touch the legacy sources are folded in once — the pre-rustcore    */
/* <profile>/icons/ png files and the history.db icons table —       */
/* gated by a stamp that survives an explicit clear.                 */
/*                                                                      */
/* The icon key is the historyIconKey rule the LocationBarSiteIcon   */
/* consumers rely on: the lowercase host of the page url, or the     */
/* whole url when it carries none.  Mutations emit the "icons"       */
/* change topic.                                                      */

/* Store the favicon PNG for url's icon key; a rewrite carrying      */
/* identical bytes is a no-op. */
RcStatus rc_icons_put(const char *urlUtf8, const uint8_t *png,
                      size_t len);

/* The stored PNG for url's key into *out (rc_buffer_free);          */
/* RC_NOT_FOUND on a miss. */
RcStatus rc_icons_get(const char *urlUtf8, RcBuffer *out);

/* Drop the record for url's key — RC_OK either way. */
RcStatus rc_icons_remove(const char *urlUtf8);

/* Wipe the whole icon cache — the clearIconDatabase equivalent. */
RcStatus rc_icons_clear(void);

/* Expiry sweep: drop records not refreshed at-or-before cutoffMs;   */
/* outRemoved may be NULL.  HistoryManager passes its history        */
/* retention boundary — icons age out with the visits they belong    */
/* to and re-record on the next load's iconChanged. */
RcStatus rc_icons_sweep(int64_t cutoffMs, int64_t *outRemoved);

/* Record count; -1 when no data dir is anchored. */
int64_t rc_icons_count(void);

/* The store's icon key for a url (lowercase host, or the whole url */
/* when hostless) — caller frees with rc_string_free. */
char *rc_icons_key(const char *urlUtf8);

/* The index rows as a JSON array [{"key","file","updated","pages"}] */
/* — caller frees with rc_string_free. */
char *rc_icons_list(void);

/* ------------------------------------------------------------------ */
/* RC02: typed per-profile settings store                              */
/*                                                                     */
/* Replaces ProfileSettings' persistence — a key-value map of          */
/* string/bool/int/double/bytes/string-list values under one versioned */
/* settings.json per scope, written atomically (temp+fsync+rename).    */
/* The scope binds at open:                                            */
/*   profile  <data root>/profiles/<id>/settings.json                  */
/*   global   <data root>/settings.json                                */
/*                                                                     */
/* Legacy QSettings ini files are one-shot READ sources: a profile     */
/* open auto-imports profiles/<id>/settings.ini; the global ini (a     */
/* path only Qt knows) merges through rc_settings_import_ini with the  */
/* MPROF04 classification filter — profile scopes take classified      */
/* keys, the global scope takes the rest, existing keys always win.    */
/* The ini files are never modified or deleted.  Mutations persist     */
/* synchronously and emit the "settings" change topic.                 */
/*                                                                     */
/* Typed getters convert like QVariant does — an ini-imported "42"     */
/* serves get_int fine.  Absent keys report RC_NOT_FOUND.              */

typedef struct RcSettings RcSettings;

/* Open the store for profile_id (NULL/empty = global scope) under
 * data_root.  Returns NULL on error (rc_last_error_message has it). */
RcSettings *rc_settings_open(const char *data_root,
                             const char *profile_id_or_null);
void rc_settings_close(RcSettings *h);

/* One-shot merge of a QSettings ini file, scope-filtered as above.
 * Idempotent — a second call is a no-op. */
RcStatus rc_settings_import_ini(RcSettings *h, const char *ini_path);

int rc_settings_contains(RcSettings *h, const char *key);

/* Raw typed fetch: *out receives {"t":"s|b|i|d|x|l","v":...} JSON —
 * the QVariant-faithful accessor. */
RcStatus rc_settings_get(RcSettings *h, const char *key, RcBuffer *out);

/* Typed reads — QVariant-style conversion; RC_NOT_FOUND when absent. */
RcStatus rc_settings_get_string(RcSettings *h, const char *key,
                                RcBuffer *out);
RcStatus rc_settings_get_bool(RcSettings *h, const char *key,
                              int *out);
RcStatus rc_settings_get_int(RcSettings *h, const char *key,
                             int64_t *out);
RcStatus rc_settings_get_double(RcSettings *h, const char *key,
                                double *out);
RcStatus rc_settings_get_bytes(RcSettings *h, const char *key,
                               RcBuffer *out);
/* String list as a JSON array of strings in *out. */
RcStatus rc_settings_get_list(RcSettings *h, const char *key,
                              RcBuffer *out);

/* Typed writes — persist atomically, emit "settings". */
RcStatus rc_settings_set_string(RcSettings *h, const char *key,
                                const char *value);
RcStatus rc_settings_set_bool(RcSettings *h, const char *key,
                              int value);
RcStatus rc_settings_set_int(RcSettings *h, const char *key,
                             int64_t value);
RcStatus rc_settings_set_double(RcSettings *h, const char *key,
                                double value);
RcStatus rc_settings_set_bytes(RcSettings *h, const char *key,
                               const uint8_t *value, size_t len);
/* String list as a JSON array of strings. */
RcStatus rc_settings_set_list(RcSettings *h, const char *key,
                              const uint8_t *json, size_t len);

/* Drop key (idempotent). */
RcStatus rc_settings_remove(RcSettings *h, const char *key);

/* Keys under prefix (boundary match: prefix and prefix/…; NULL/empty
 * lists all) as a JSON array — rc_string_free the result. */
char *rc_settings_list(RcSettings *h, const char *prefix_or_null);

/* Flush — mutations already write through; kept for the
 * QSettings::sync() call shape. */
RcStatus rc_settings_sync(RcSettings *h);

/* ------------------------------------------------------------------ */
/* RC19: container registry + storage-namespace map                    */
/*                                                                     */
/* Firefox-style containers — the registry ContainerManager persists  */
/* inside profile settings (the "containers" group), so this module   */
/* is a typed specialization over the settings store: every registry  */
/* op runs on an RcSettings handle bound to the OWNING persona's      */
/* scope (the group is a classified persona key — a global-scope      */
/* handle sees no containers).  The on-disk keys match the C++        */
/* layout exactly — containers/order (id list in creation order),     */
/* containers/<id>/name, containers/<id>/color (QColor name() text),  */
/* containers/<id>/icon (optional — the C++ ignores the key).  A      */
/* registry written here is what ContainerManager::loadRegistry       */
/* reads on the next scope open; "order" is authoritative and stale   */
/* groups are pruned on read, mirroring its hygiene pass.             */
/*                                                                     */
/* rc_containers_namespace is the storage-namespace map — the pure   */
/* derivation ContainerManager::storagePath/cachePathFor/             */
/* defaultDataPathFor/storageNameFor implement, plus the              */
/* cookie-partition key the RC15 cookie db partitions rows on.        */
/* "Always open in <container>" host rules are not here — they live   */
/* in sitedecisions.json's "container" kind.                          */
/*                                                                     */
/* Mutations persist atomically and emit the "containers" change      */
/* topic.                                                             */

/* The registry as a JSON array of {"id","name","color","icon"} in
 * creation order — NULL + stashed error on a bad handle.
 * rc_string_free the result. */
char *rc_containers_list(RcSettings *h);

/* One row, same shape — NULL when the id is unregistered (a miss is
 * not an error).  rc_string_free the result. */
char *rc_containers_get(RcSettings *h, const char *id);

/* Create from JSON {"name","color","icon"?} — the id is minted
 * library-side (v4 uuid, the C++ hex-with-dashes shape).  Returns the
 * row JSON or NULL + stashed error.  rc_string_free the result. */
char *rc_containers_create(RcSettings *h, const uint8_t *json,
                           size_t len);

/* Patch from JSON {"name"?,"color"?,"icon"?} — absent keys keep
 * their stored value; "icon":"" or null clears it.  RC_NOT_FOUND on
 * an unregistered id. */
RcStatus rc_containers_update(RcSettings *h, const char *id,
                              const uint8_t *json, size_t len);

/* The store half of deleteContainer — the <id>/ group and its
 * "order" entry.  RC_NOT_FOUND on an unregistered id.  The on-disk
 * storage trees are the caller's job — rc_containers_namespace
 * derives their paths. */
RcStatus rc_containers_remove(RcSettings *h, const char *id);

/* The storage namespace for a registered id — JSON
 * {"storage_name","storage_path","cache_path","default_data_path",
 * "cookie_partition"}.  data_root is the APP data root
 * (BrowserPaths::dataRoot).  NULL + stashed error on a bad id — the
 * default ("") container has no namespace of its own.
 * rc_string_free the result. */
char *rc_containers_namespace(const char *data_root,
                              const char *profile_id,
                              const char *container_id);

/* ------------------------------------------------------------------ */
/* MD04: download-interception policy (dlwpolicy)                     */
/*                                                                     */
/* The DownloadManager's decision layer — a typed specialization    */
/* over the profile scope's settings store like the container       */
/* registry, so every op runs on an RcSettings handle bound to the  */
/* owning persona.  The suggested-name sanitizer, the dangerous-    */
/* extension/executable-mime threat tables, the save-as prompt      */
/* gate, the external-handler handoff, the rustdl engine claim,     */
/* the finished-row removal verdict, the per-profile rule view and  */
/* the download history records live here.                          */
/*                                                                     */
/* The legacy downloadmanager/ keys read from the bound profile's   */
/* settings.json (Qt still owns their writes); facade-authored      */
/* rules (always_save_mimes, blocked_extensions) and the record     */
/* list live in downloads.json beside it — records_list falls back  */
/* to the legacy download_N_* keys while that file is absent, so    */
/* pre-move history is never stranded.  The dialogs, the QProcess   */
/* handoff and the filesystem execution stay caller-side — this     */
/* surface only answers questions.                                  */
/*                                                                     */
/* rc_dlw_call: generic granular endpoint {"op":..., params} ->     */
/*   JSON result.  `h` is the bound profile's RcSettings            */
/*   (rc_settings_open, or the ac_dlw_call per-op open); NULL is    */
/*   legal for the pure ops (sanitize_name, threat, external_argv)  */
/*   — store-bound ops then error rather than guess rules.  Ops:    */
/*   verdict, external_gate, engine_claim, row_remove, rules,       */
/*   rule_get, rule_set, records_list, records_save, records_clear. */
/*   NULL on error — callers fail toward their own local consult.   */
/*   rc_string_free() the result.                                   */
char *rc_dlw_call(RcSettings *h, const uint8_t *jsonUtf8, size_t len);

/* ------------------------------------------------------------------ */
/* MD05: privacy policy store view (privacy)                          */
/*                                                                     */
/* The per-persona privacy/ flag reads — a typed specialization     */
/* over the profile scope's settings store like dlwpolicy, so every */
/* op runs on an RcSettings bound to the owning persona.  The       */
/* request interceptors' snapshot reads, the PRIV08 Sec-GPC         */
/* decision and the unified tracker-strip/adblock policy read live  */
/* here; the Qt writers keep owning the keys and all execution      */
/* (setHttpHeader, script insertion, dialogs).                      */
/*                                                                     */
/* rc_privacy_call: generic granular endpoint {"op":..., params} -> */
/*   JSON result.  `h` is the bound profile's RcSettings            */
/*   (rc_settings_open, or the ac_privacy_call per-op open); NULL   */
/*   is legal only for the pure "defaults" op — store-bound ops     */
/*   error rather than guess a toggle.  Ops: flags (the whole       */
/*   typed set), flag (one named row), gpc (the Sec-GPC decision    */
/*   with the tor_mode fold), verdict ({gpc,strip,adblock} — the    */
/*   unified read the tracker-strip and adblock consumers share),   */
/*   defaults.  NULL on error — callers keep their local consult.   */
/*   rc_string_free() the result.                                   */
/*                                                                     */
/* rc_privacy_gpc: the request-time Sec-GPC decision on the bound   */
/*   store — 1 send "Sec-GPC: 1", 0 don't, -1 error (a -1 keeps the */
/*   caller's verdict-bit fallback).  tor_mode != 0 answers 1       */
/*   unconditionally — the tor pipeline's uniform-constant rule.    */
char *rc_privacy_call(RcSettings *h, const uint8_t *jsonUtf8,
                      size_t len);
int   rc_privacy_gpc(RcSettings *h, int tor_mode);

/* ------------------------------------------------------------------ */
/* MD06: URL-sanitize policy (urlclean)                                 */
/*                                                                     */
/* The per-profile URL-cleaning decision layer — a typed             */
/* specialization over the profile scope's settings store plus its   */
/* own profiles/<id>/urlclean.json, the same two-tier split          */
/* dlwpolicy made: the legacy urlcleaner/extraParams key is only     */
/* ever READ through the bound store (Qt keeps the writes — no       */
/* import step strands a pre-move user's list), while the            */
/* facade-authored ruleset override (the urlstrip grammar —          */
/* params/scoped/unwrap/exceptions) and the copy-path toggle live    */
/* in urlclean.json.  The nav-path enable stays the privacy module's  */
/* urlCleaning/stripTrackingParams read; the copy path defaults ON.  */
/*                                                                     */
/* rc_urlclean_call: generic granular endpoint {"op":..., params} -> */
/*   JSON result.  `h` is the bound profile's RcSettings             */
/*   (rc_settings_open, or the ac_urlclean_call per-op open); NULL   */
/*   is legal only for the pure "defaults" op — store-bound ops      */
/*   error rather than guess a toggle.  Ops: clean (url, purpose     */
/*   copy|nav, exempt, unwrap -> {url,changed,removed,unwrapped,     */
/*   enabled}), enabled, set_enabled (purpose copy only — the nav    */
/*   key is Qt-owned), params, ruleset_get, ruleset_set,             */
/*   ruleset_reset, clean_policy (the snapshot-facing composite:     */
/*   toggles + the merged ruleset doc the loader embeds as           */
/*   "urlclean_ruleset"), defaults.  Mutating ops emit the           */
/*   "urlclean" change topic.  NULL on error — callers keep their    */
/*   local consult.  rc_string_free() the result.                    */
char *rc_urlclean_call(RcSettings *h, const uint8_t *jsonUtf8,
                       size_t len);

/* ------------------------------------------------------------------ */
/* RC20: update-check policy + manifest parse                          */
/*                                                                     */
/* UpdateChecker's non-network half — the fetch stays Qt-side        */
/* (UPD01's manual Help > Check for Updates); everything the reply's */
/* bytes steer lives here: the GitHub release-manifest parse behind  */
/* the AUDIT04 trusted-download-URL gate, version normalize/compare, */
/* the platform asset pick, and the channel rule.                    */
/*                                                                     */
/* The check state (lastCheckMs, snoozeUntilMs, skippedVersion,      */
/* channel) is a typed specialization over the settings store — the  */
/* GLOBAL scope by design (the check is application-level;           */
/* "updates" is not an MPROF04 profile group), so state ops take a   */
/* global-bound RcSettings handle — ac_settings(core, NULL) reaches  */
/* the same store.  The keys: updates/lastCheckMs (int),             */
/* updates/snoozeUntilMs (int), updates/skippedVersion (string),     */
/* updates/channel (string).  Mutations emit the "updates" change    */
/* topic.                                                            */
/*                                                                     */
/* Like torctl this binds no profile, so there is no AcUpdates       */
/* facade handle — rc_* is the whole surface.                        */

/* UpdateChecker::normalizedVersion — rc_string_free the result;
 * NULL + stashed error on a bad pointer. */
char *rc_updates_normalize_version(const char *version);

/* UpdateChecker::compareVersions — -1/0/+1 on the normalized forms
 * (numeric elementwise when both sides parse, string compare else);
 * -2 with a stashed error on a bad pointer — never a verdict. */
int rc_updates_compare_versions(const char *a, const char *b);

/* The AUDIT04 download-URL gate — github.com / *.github.com /
 * *.githubusercontent.com over https, or the api url's own host with
 * no scheme downgrade.  1 trusted / 0 refused, -1 on bad input. */
int rc_updates_is_trusted_url(const char *url, const char *api_url);

/* The channel rule — 1 when a manifest with draft/prerelease flags
 * is an offer on `channel` ("stable"/"beta", "" = stable), 0 when
 * not, -1 on an unknown channel or bad pointer. */
int rc_updates_channel_accepts(const char *channel,
                               int draft, int prerelease);

/* UpdateChecker::parseReleaseJson — returns JSON {"version",
 * "title","page_url","draft","prerelease","assets":[{"name","url",
 * "size"}]} with untrusted asset URLs already dropped, or NULL +
 * stashed error on malformed input.  rc_string_free the result. */
char *rc_updates_parse_manifest(const uint8_t *payload, size_t len,
                                const char *api_url);

/* UpdateChecker::platformAssetsFor — assets_json is the "assets"
 * array parse_manifest emits (or any [{"name","url","size"}] list).
 * Returns {"candidates":[{...}],"ambiguous":bool} preference-ordered
 * — rc_string_free; NULL + stashed error on bad input. */
char *rc_updates_platform_pick(const char *os,
                               const char *os_release_id_like,
                               const uint8_t *assets_json, size_t len);

/* The GLOBAL scope's check state as JSON {"last_check_ms",
 * "snooze_until_ms","skipped_version","channel"} — rc_string_free;
 * NULL + stashed error on a bad handle. */
char *rc_updates_state(RcSettings *h);

/* Stamp the last-check time (ms since epoch). */
RcStatus rc_updates_record_check(RcSettings *h, int64_t now_ms);

/* Snooze notifications until `until_ms` (<= 0 clears). */
RcStatus rc_updates_snooze(RcSettings *h, int64_t until_ms);

/* The tag the user dismissed — NULL clears the filter. */
RcStatus rc_updates_set_skipped(RcSettings *h,
                                const char *version_or_null);

/* Persist the channel — "stable"/"beta" only; an unknown name is
 * RC_ERR_INVALID and changes nothing. */
RcStatus rc_updates_set_channel(RcSettings *h, const char *channel);

/* The notify verdict for a remote tag vs the local version at
 * now_ms: newer, unskipped, unsnoozed — 1 notify / 0 don't, -1 on
 * bad input. */
int rc_updates_should_notify(RcSettings *h, const char *remote_version,
                             const char *local_version, int64_t now_ms);

/* ------------------------------------------------------------------ */
/* RC21: per-profile side-panel notes store                            */
/*                                                                     */
/* The sidebar's ordered multi-note list — profiles/<id>/notes.json   */
/* in the C++ writer's byte shape: a compact JSON array of            */
/* {"body","markdown","title"} objects (QJsonObject's sorted-key      */
/* emission — both writers produce identical bytes), array order =    */
/* list order.  The load mirrors the model: missing/unreadable file   */
/* or a non-array document is an empty store, a non-object element    */
/* is a blank note, wrong-typed fields default.                       */
/*                                                                     */
/* Two bindings: rc_notes_open takes a data root + persona slug (the */
/* facade lane); rc_notes_open_file takes an explicit file (the      */
/* standalone NotesModel(storageFile) lane).  A NULL/empty profile   */
/* id or path mints the VOLATILE off-the-record store the model      */
/* hands private/tor windows — CRUD in memory, sync() a no-op,       */
/* nothing reaches disk.                                              */
/*                                                                     */
/* Mutations dirty the decoded list and emit the "notes" topic;      */
/* rc_notes_sync commits atomically (temp+rename+fsync, parent dir   */
/* created like the C++ mkpath) and rc_notes_close flushes too —     */
/* the Qt AutoSaver debounce contract carries across unchanged.      */

typedef struct RcNotes RcNotes;

/* Open the persona store: slug → profiles/<id>/notes.json,
 * NULL/empty → volatile, non-slug → NULL + stashed error.
 * Release with rc_notes_close. */
RcNotes *rc_notes_open(const char *data_root, const char *profile_id);
/* Open a store on an explicit file; NULL/empty → volatile. */
RcNotes *rc_notes_open_file(const char *path);
/* Close — a dirty persistent handle flushes first (the model
 * destructor's saveIfNeccessary).  NULL-tolerant. */
void rc_notes_close(RcNotes *h);

/* Row count; -1 on a bad handle. */
int64_t rc_notes_count(const RcNotes *h);
/* One note as {"body","markdown","title"} JSON; NULL on a bad
 * handle or out-of-range row (NotFound stashed).  rc_string_free. */
char *rc_notes_get(const RcNotes *h, int64_t row);
/* The whole list in the persisted array shape; rc_string_free. */
char *rc_notes_list(const RcNotes *h);

/* insertNote — {title, body} at the clamped row (out-of-range
 * positions prepend/append like qBound), markdown cleared; NULL
 * strings read as "".  Returns the new row, -1 on error. */
int64_t rc_notes_insert(RcNotes *h, int64_t row, const char *title,
                        const char *body);

/* Field setters — Qt::EditRole / BodyRole / MarkdownRole.
 * RC_NOT_FOUND out of range; an identical value is RC_OK with
 * out_changed=0 (the model's setData false return); out_changed
 * may be NULL. */
RcStatus rc_notes_set_title(RcNotes *h, int64_t row, const char *value,
                            int *out_changed);
RcStatus rc_notes_set_body(RcNotes *h, int64_t row, const char *value,
                           int *out_changed);
RcStatus rc_notes_set_markdown(RcNotes *h, int64_t row, int on,
                               int *out_changed);

/* removeRows — RC_INVALID on a refused range (negative, empty or
 * overflowing), the model's false return. */
RcStatus rc_notes_remove(RcNotes *h, int64_t row, int64_t count);

/* The AutoSaver-fired save(): persists when dirty; a clean or
 * volatile store is a no-op. */
RcStatus rc_notes_sync(RcNotes *h);

/* 1 file-backed / 0 volatile (or bad handle) — isPersistent(). */
int rc_notes_is_persistent(const RcNotes *h);
/* 1 while uncommitted mutations exist, 0 clean/bad handle. */
int rc_notes_is_dirty(const RcNotes *h);

/* ------------------------------------------------------------------ */
/* RC03: profile registry                                              */
/*                                                                     */
/* The persona list — <data root>/profiles.json, byte-compatible with */
/* the MPROF01 writer that ProfileRegistry and ProfileStore share:    */
/* {"version":1,"profiles":[{id,name,avatar,createdAt}]} emitted in   */
/* QJsonDocument::Indented layout.  The registry always describes at  */
/* least "default" — a missing file materializes the synthesized row  */
/* on open (default_name_or_null seeds its name — the caller's        */
/* QSettings "profile/name" value or localized fallback), a corrupt   */
/* file yields the same view without being rewritten.                 */
/*                                                                     */
/* ids are filesystem slugs ([a-z0-9_-], <=64); the avatar field is   */
/* a basename joined under profiles/<id>/ — separators and dot        */
/* segments are refused.  delete refuses "default" and removes        */
/* profiles/<id>/ recursively; live-object teardown and the vault     */
/* wipe stay on the Qt adapter wrapping this primitive.  Mutations    */
/* persist atomically and emit the "profiles" change topic.           */
/*                                                                     */
/* The active id is per-handle process state — the ambient persona    */
/* BrowserPaths::activeProfileId exposes; never persisted.            */

typedef struct RcProfiles RcProfiles;

/* Open the registry under data_root (a real path — the BrowserPaths
 * data root).  NULL on error; release with rc_profiles_close. */
RcProfiles *rc_profiles_open(const char *data_root,
                             const char *default_name_or_null);
void rc_profiles_close(RcProfiles *h);

/* Re-read from disk — the seam for callers that changed the file
 * behind the registry's back. */
RcStatus rc_profiles_reload(RcProfiles *h);

/* JSON array of {id,name,avatar,createdAt}; rc_string_free. */
char *rc_profiles_list(RcProfiles *h);
/* One row as a JSON object; NULL on error (rc_last_error_message). */
char *rc_profiles_get(RcProfiles *h, const char *id);

/* Create from a display name (slugified unique id + profiles/<id>/);
 * returns the new id — rc_string_free.  NULL on failure. */
char *rc_profiles_create(RcProfiles *h, const char *display_name);
/* Blank names resolve to "Profile" like ProfileStore::renameProfile. */
RcStatus rc_profiles_rename(RcProfiles *h, const char *id,
                            const char *name);
/* Avatar field — basename only; NULL/empty clears.  The PNG itself
 * is written Qt-side (decode/scale stays there). */
RcStatus rc_profiles_set_avatar(RcProfiles *h, const char *id,
                                const char *name_or_null);
/* Removes the row + profiles/<id>/ recursively; refuses "default"
 * and non-slug ids. */
RcStatus rc_profiles_delete(RcProfiles *h, const char *id);

/* Ambient persona — per-handle process state.  set_active resolves
 * non-slug ids to "default" like BrowserPaths::setActiveProfileId. */
char *rc_profiles_active(RcProfiles *h);
RcStatus rc_profiles_set_active(RcProfiles *h, const char *id);

/* Per-profile roots mirroring BrowserPaths exactly — a NULL or
 * non-slug id resolves to "default".  All rc_string_free results. */
char *rc_profiles_dir(RcProfiles *h, const char *id);
char *rc_profiles_cache_dir(RcProfiles *h, const char *id);
char *rc_profiles_engine_storage_dir(RcProfiles *h, const char *id);
char *rc_profiles_engine_cache_dir(RcProfiles *h, const char *id);
char *rc_profiles_settings_dir(RcProfiles *h, const char *id);
/* profiles/<id>/<avatar field or profile-avatar.png>. */
char *rc_profiles_avatar_path(RcProfiles *h, const char *id);
/* <root>/profiles.json — the file this handle owns. */
char *rc_profiles_file(RcProfiles *h);
/* Display name -> slug (no handle needed); rc_string_free. */
char *rc_profiles_slugify(const char *display_name);

/* ------------------------------------------------------------------ */
/* OSE01: OpenSearch engine registry                                   */
/*                                                                      */
/* The canonical search-engine store — searchengines.json under the   */
/* data dir holds one record per engine (the rc_opensearch_parse      */
/* field map plus a "keywords" string array), the display order and   */
/* the removed-bundled blocklist.  The OpenSearchManager Qt adapter   */
/* hydrates engine objects from these records and every mutation      */
/* writes through; mutations emit the "searchengines" change topic.   */
/*                                                                      */
/* Bundled descriptors are vendored into the crate (the same files    */
/* the no-rust qrc ships) and seed through rc_ose_seed_bundled /      */
/* rc_ose_restore_bundled — never as an implicit read fallback, so    */
/* deleting a bundled engine stays deleted.                            */

/* 1 when the store file exists — the legacy-import gate. */
int rc_ose_store_present(void);

/* Engine names in display order — JSON string array; rc_string_free. */
char *rc_ose_list(void);

/* One engine record as JSON — NULL when absent; rc_string_free. */
char *rc_ose_get(const char *nameUtf8);

/* Upsert an engine record JSON (name + search template required;
 * "keywords" replaces the bindings when present, preserved when
 * absent). */
RcStatus rc_ose_put(const uint8_t *jsonUtf8, size_t len);

/* Parse an OpenSearch descriptor (bounded, DTD-refused) and upsert
 * it — the .xml import path; keywords survive a replace. */
RcStatus rc_ose_import(const uint8_t *xmlUtf8, size_t len);

/* Drop an engine by name; RC_OK whether or not it existed. */
RcStatus rc_ose_remove(const char *nameUtf8);

/* Re-key an engine keeping its order slot and keywords —
 * RC_NOT_FOUND on a missing source, RC_INVALID_ARGUMENT on an
 * empty/taken target. */
RcStatus rc_ose_rename(const char *oldUtf8, const char *newUtf8);

/* Replace the display order with a JSON name array. */
RcStatus rc_ose_reorder(const uint8_t *jsonUtf8, size_t len);

/* Replace an engine's keyword bindings (JSON string array);
 * RC_NOT_FOUND when the engine does not exist. */
RcStatus rc_ose_set_keywords(const char *nameUtf8,
                             const uint8_t *jsonUtf8, size_t len);

/* The engine name a keyword resolves to — NULL when unbound;
 * rc_string_free. */
char *rc_ose_engine_for_keyword(const char *keywordUtf8);

/* Every bound keyword — JSON string array; rc_string_free. */
char *rc_ose_keywords(void);

/* The removed-bundled blocklist — JSON string array; rc_string_free. */
char *rc_ose_removed_bundled(void);
RcStatus rc_ose_block_bundled(const char *nameUtf8);
RcStatus rc_ose_unblock_bundled(const char *nameUtf8);

/* The vendored bundled engine names — JSON string array. */
char *rc_ose_bundled_names(void);

/* Insert every bundled engine that is absent and not blocked. */
RcStatus rc_ose_seed_bundled(void);

/* Clear the blocklist and re-add every bundled engine, replacing
 * same-named records but keeping their keyword bindings — the
 * "restore defaults" semantic. */
RcStatus rc_ose_restore_bundled(void);

/* Wipe the registry and remove searchengines.json — the test-suite
 * reset seam; re-arms the Qt legacy-migration gate. */
RcStatus rc_ose_reset(void);

/* URL expansion — the buildUrl()/parseTemplate() port, byte-identical
 * to the Qt implementation (probed on Qt 6.12):
 *   spec JSON {"template","method","params":[["k","v"],...],"term",
 *              "language","source"} -> encoded URL string.
 * {searchTerms} encodes per QUrl::toPercentEncoding (only
 * [A-Za-z0-9-._~] raw); existing-query pairs and appended Param
 * values encode per QUrlQuery's component table (valid %XX triplets
 * preserved, uppercase-normalized).  NULL on a bad spec;
 * rc_string_free. */
char *rc_ose_expand(const uint8_t *specJsonUtf8, size_t len);

/* Engine + slot kind ("search"|"suggestions"|"image") + term ->
 * expanded URL; NULL when the engine or slot is absent. */
char *rc_ose_url(const char *nameUtf8, const char *kindUtf8,
                 const char *termUtf8, const char *languageUtf8,
                 const char *sourceUtf8);

/* keyword + terms -> the bound engine's search URL; NULL when the
 * keyword is unbound. */
char *rc_ose_keyword_url(const char *keywordUtf8, const char *termUtf8,
                         const char *languageUtf8, const char *sourceUtf8);

/* ------------------------------------------------------------------ */
/* UAG01: user-agent construction + per-site spoof table               */
/*                                                                      */
/* The pure UA string logic — the effective-UA decision (configured   */
/* override, else the de-badged factory UA presenting the bumped      */
/* Chrome milestone), the Sec-CH-UA brand version a UA implies, and   */
/* the useragents.xml switcher-list parse.  The per-site spoof table  */
/* lives as the "uaspoof" kind inside the sitedecisions store; the    */
/* clearnet interceptor applies a hit as the User-Agent request       */
/* header while the tor interceptor never consults it.  Mutations     */
/* emit the "sitedecisions" change topic.                              */

/* Builds the effective UA from a JSON context
 * {"factory_ua","presented_major","override"} — a non-empty override
 * wins verbatim; otherwise the factory UA loses its
 * "QtWebEngine/<ver>" token and its Chrome/<major> is bumped to the
 * presented milestone.  NULL on a bad context; rc_string_free. */
char *rc_ua_build(const uint8_t *contextJsonUtf8, size_t len);

/* The Sec-CH-UA full version a UA implies — the UA's own Chrome
 * major over the engine version's build tail; empty when the UA does
 * not claim Chrome.  rc_string_free. */
char *rc_ua_brand_version(const char *uaUtf8,
                          const char *engineVersionUtf8);

/* Parses a useragentswitcher document into a JSON array preserving
 * document order:
 *   [{"type":"separator"},
 *    {"type":"agent","description":..,"useragent":..}]
 * A malformed tail keeps the entries that parsed.  rc_string_free. */
char *rc_ua_presets(const uint8_t *xmlUtf8, size_t len);

/* The stored per-site UA override governing host (longest-suffix
 * match), or NULL when nothing applies.  rc_string_free. */
char *rc_ua_spoof(const char *hostUtf8);

/* Records/drops a per-site override.  Values carrying control bytes
 * are refused — a spoof lands verbatim in a User-Agent header. */
RcStatus rc_ua_spoof_set(const char *hostUtf8, const char *uaUtf8);
RcStatus rc_ua_spoof_remove(const char *hostUtf8);

/* Every spoof row as {"host":"ua"} JSON.  rc_string_free. */
char *rc_ua_spoof_list(void);

/* ------------------------------------------------------------------ */
/* CLR01: clear-private-data decision engine                            */
/*                                                                      */
/* "Which stores get wiped" is a pure (period, categories, scope) ->  */
/* ordered operation list function; the shell executes the ops        */
/* per-store.  The plan carries profile scoping: "persistent" ops     */
/* cover the normal + materialized container profiles and the         */
/* persistent shell stores (the OTR profile is absent — a persistent  */
/* clear never reaches into a live private session); "otr" scope      */
/* contains only the off-the-record context's own in-memory state,    */
/* so a private-window clear can never touch the persistent stores    */
/* either.                                                             */
/*                                                                      */
/* Plan JSON:                                                          */
/*   {"scope":"persistent"|"otr", "period":name, "since_secs":n,       */
/*    "ops":[{"op":NAME, "since_secs":n, "ranged":bool,                */
/*            "scope"?:..., "degraded"?:true, ...flags}, ...]}         */
/* Op names:                                                           */
/*   containers.unmaterialized   on-disk trees of never-materialized   */
/*                               containers (cookies/sitedata/cache/   */
/*                               visited_links flags)                  */
/*   history.clear               the visit log — the one ranged        */
/*                               store (since_secs honored via          */
/*                               rc_hist_clear_since)                   */
/*   visited_links.clear         Chromium's visited-link db            */
/*   downloads.clear             whole download list (+hide)           */
/*   downloads.clear_otr         off-the-record rows only              */
/*   search.clear                ToolbarSearch recent lists            */
/*   cookies.clear               per-scope profile cookie stores       */
/*   sitedata.page_sweep         live-page DOM-storage wipe (must run  */
/*                               before sitedata.defer_wipe)           */
/*   sitedata.defer_wipe         storage trees at next profile start   */
/*   cache.clear                 per-scope profile http caches         */
/*   nam_cache.clear             the app-side fetch cache              */
/*   icons.clear                 the per-host favicon store            */
/*   session.clear               the saved-session blob                */
/*   storage.defer_full_wipe     PRIV01 network+cache tree removal     */
/*   containers.wipe_unmaterialized  whole unmaterialized trees        */
/* "degraded" marks a ranged request a store cannot honor — it        */
/* executes as a full wipe.                                            */

/* Category bits — the ClearPrivateData::ClearCategory enum mirrors
 * these exactly (static_assert'd on the Qt side). */
#define RC_CLEAR_HISTORY    0x01u
#define RC_CLEAR_DOWNLOADS  0x02u
#define RC_CLEAR_SEARCH     0x04u
#define RC_CLEAR_COOKIES    0x08u
#define RC_CLEAR_SITEDATA   0x10u
#define RC_CLEAR_CACHE      0x20u
#define RC_CLEAR_ICONS      0x40u
#define RC_CLEAR_SESSION    0x80u
/* Deep storage wipe — the exit-time path only (deferred storage-tree
 * and unmaterialized-container removal). */
#define RC_CLEAR_FULL_WIPE  0x100u

/* rc_clearplan(periodUtf8, categories, otrActive) -> plan JSON;
 * rc_string_free.  period: "all"/"everything"/"" or the "hour"/"day"/
 * "week" windows; "secs:<digits>" (MD28) carries an exact window for
 * callers starting from an absolute timestamp.  NULL or an unknown
 * name -> NULL + error. */
char *rc_clearplan(const char *periodUtf8, uint32_t categories,
                   int otrActive);

/* ------------------------------------------------------------------ */
/* PVAULT01: password-sealed profile vaults ("ARV1")                    */
/*                                                                      */
/* A vault.arv is a chunked, streamable archive of a profile           */
/* directory: a random 512-bit vault key (enc + mac halves) wrapped    */
/* by an Argon2id-derived KEK; a tar-style pack encrypted in 16 MiB    */
/* chunks; HMAC-SHA-512 over the header and over the whole payload.    */
/* Because the vault key is wrapped rather than password-derived, a   */
/* password change only re-wraps the header — no re-encrypt.          */
/*                                                                      */
/* Algorithms: RC_VAULT_ALGO_AES256_GCM is the default (AES-256-GCM   */
/* per chunk + HMAC-SHA-512).  RC_VAULT_ALGO_THREEFISH_XEX is the      */
/* opt-in lane (Threefish-1024 XEX-style + Skein-MAC) — the crates    */
/* are unaudited upstream, see the module docs.                        */
/*                                                                      */
/* Errors are honest and distinct: RC_WRONG_PASSWORD means the wrap   */
/* rejected the password; RC_TAMPERED means a MAC failed after a      */
/* successful unwrap; RC_CORRUPT means malformed structure;           */
/* RC_UNSUPPORTED_ALGO means the algo_id is not implemented.  Unseal  */
/* is two-pass — the whole payload MAC verifies before any plaintext  */
/* is written — so a failed restore leaves zero artifacts.            */
/*                                                                      */
/* These calls are stateless (no rc_set_data_dir needed) and may run  */
/* on any thread.  The optional progress callback fires synchronously */
/* on the calling thread: (bytes_done, bytes_total) — queue onto the  */
/* GUI thread for widgets.                                            */

#define RC_VAULT_ALGO_AES256_GCM    0x01u
#define RC_VAULT_ALGO_THREEFISH_XEX 0x02u

typedef void (*RvProgressCallback)(uint64_t done, uint64_t total,
                                   void *userdata);

/* Seal dir -> outPath (atomic temp+rename, 0600).  cb may be NULL. */
RcStatus rv_seal(const char *dirUtf8,
                 const uint8_t *pass, size_t passLen,
                 uint8_t algo, const char *outPathUtf8,
                 RvProgressCallback cb, void *userdata);

/* Verify + unpack blobPath into outDir (absent or empty).  Two-pass;
 * a failure leaves no partial output. */
RcStatus rv_unseal(const char *blobPathUtf8,
                   const uint8_t *pass, size_t passLen,
                   const char *outDirUtf8,
                   RvProgressCallback cb, void *userdata);

/* Password + authenticity check without unpacking. */
RcStatus rv_verify(const char *blobPathUtf8,
                   const uint8_t *pass, size_t passLen);

/* Header summary JSON for UI display — UNVERIFIED fields (no
 * password): {"format","algo","algo_name","kdf","memory_kib",
 * "passes","lanes","chunk_size","chunk_count","payload_bytes",
 * "authenticated":false}.  NULL on error; rc_string_free(). */
char *rv_inspect(const char *blobPathUtf8);

/* Cheap password change: re-wraps the vault key (~217 bytes
 * rewritten; the payload never moves).  RC_WRONG_PASSWORD when
 * oldPass does not open the vault. */
RcStatus rv_rewrap(const char *blobPathUtf8,
                   const uint8_t *oldPass, size_t oldLen,
                   const uint8_t *newPass, size_t newLen);

/* ------------------------------------------------------------------ */
/* RC14: Tor control-protocol client                                   */
/*                                                                     */
/* The Rust port of torcontrol.cpp — the control-spec line protocol    */
/* over a loopback TCP connection.  Commands are serialized (one in    */
/* flight at most); every submitted command completes exactly once in  */
/* take_reply order with the final status code, or -1 when the link    */
/* died first.  Async 650 event lines and protocol complaints queue    */
/* separately; every push emits the "torctl" change topic so the Qt    */
/* side can queue a drain on the GUI thread.                           */
/*                                                                     */
/* No-leak contract: rc_tor_open accepts loopback addresses and        */
/* "localhost" (resolved + re-verified) only, and AUTHENTICATE         */
/* payloads are held in zeroizing buffers.                             */

#define RC_TOR_CONNECTED 1
#define RC_TOR_CLOSED    2

typedef struct RcTorCtl RcTorCtl;

/* Synchronous connect with a bounded timeout — a returned handle is
 * always RC_TOR_CONNECTED.  NULL + rc_last_error_message on refusal
 * or a non-loopback host. */
RcTorCtl *rc_tor_open(const char *host, uint16_t port);
/* Sever the link but keep the handle valid — pending commands flush
 * as -1 replies and the drain lanes still read.  rc_tor_close frees. */
void rc_tor_disconnect(RcTorCtl *h);
void rc_tor_close(RcTorCtl *h);            /* NULL-tolerant */
int rc_tor_state(RcTorCtl *h);             /* 0 bad handle */
char *rc_tor_error(RcTorCtl *h);           /* "" when clean; rc_string_free */

/* Queue one command; returns the reply sequence number or -1.
 * `command` must be a single line (a CRLF payload is rejected, never
 * split — the injection boundary). */
int64_t rc_tor_send(RcTorCtl *h, const char *command);

/* AUTHENTICATE lanes: 32-byte cookie as hex token, cookie read from a
 * file in-library, or a password sent as an escaped QuotedString. */
int64_t rc_tor_auth_cookie(RcTorCtl *h, const uint8_t *cookie,
                           size_t len);
int64_t rc_tor_auth_cookie_file(RcTorCtl *h, const char *path);
int64_t rc_tor_auth_password(RcTorCtl *h, const uint8_t *pass,
                             size_t len);

/* GETINFO key ("a b" key lists legal), SETEVENTS space-separated
 * list, TAKEOWNERSHIP, SIGNAL name (NEWNYM/SHUTDOWN/…). */
int64_t rc_tor_getinfo(RcTorCtl *h, const char *key);
int64_t rc_tor_setevents(RcTorCtl *h, const char *list);
int64_t rc_tor_takeownership(RcTorCtl *h);
int64_t rc_tor_signal(RcTorCtl *h, const char *name);

/* Drain lanes — call on every "torctl" change topic until empty:
 * take_reply returns RC_OK with {"seq","code","lines":[...]} JSON in
 * out (rc_buffer_free), RC_NOT_FOUND when drained; take_event /
 * take_error return a line (rc_string_free) or NULL when empty. */
RcStatus rc_tor_take_reply(RcTorCtl *h, RcBuffer *out);
char *rc_tor_take_event(RcTorCtl *h);
char *rc_tor_take_error(RcTorCtl *h);

#ifdef __cplusplus
}
#endif

#endif // ARKTOS_RUSTCORE_H
