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
#define RC_AUTOFILL_FILE "autofill-store.dat"

/* Change topics emitted so far: "credentials" (rc_cred_* map),
 * "autofill" (rc_autofill_* record set). */
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
 * Mutations emit the "autofill" change topic. */
/* Whether the Rust store file exists — gates the legacy import. */
int rc_autofill_store_present(void);
/* Whole record list as a JSON array; rc_string_free.  NULL on error
 * (locked store, corrupt file — rc_last_error_message says which). */
char *rc_autofill_forms(void);
/* Atomic whole-set replace; RC_INVALID_ARGUMENT keeps the old set. */
RcStatus rc_autofill_set_forms(const uint8_t *jsonUtf8, size_t len);
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
 *    "excerpt", "dir", "length":n, "content":"..."}
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
 * keyword set and the urlloading/searchEngineFallback opt-in.
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
 *   store is not open. */
char *rc_classify_input(const char *inputUtf8,
                        const char *optionsJsonUtf8);
RcStatus rc_frecency_score(const char *jsonUtf8, int64_t *out);
char *rc_history_suggest(const char *termUtf8, int64_t limit);

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

#ifdef __cplusplus
}
#endif

#endif // ARORA_RUSTCORE_H
