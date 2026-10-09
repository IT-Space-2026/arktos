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

#ifndef ARORA_RUSTDL_H
#define ARORA_RUSTDL_H

/*
 * C ABI for libarora_rustdl — the accelerated download engine
 * (DLACC03/DLACC06).  Rules, mirroring rustcore:
 *
 *  - Fallible functions return DlStatus; DL_OK is the only success.
 *    Call-site failures describe themselves via
 *    dl_last_error_message() on the same thread; worker failures
 *    surface via dl_error_message(handle).
 *  - Strings handed out are freed with dl_string_free().
 *  - A download is an opaque handle (a registry id — never a pointer
 *    into Rust).  dl_poll is the read side, dl_cancel the interrupt,
 *    dl_free the destructor.  Polling is the contract: no callbacks
 *    cross the FFI.
 *  - Part files live in a per-download randomized subdirectory under
 *    the directory given to dl_set_temp_dir (0700).  The destination
 *    file only ever appears through an atomic rename after the merge
 *    and size check.
 *  - Error strings never contain the request URL's query or fragment
 *    (DLACC05 log hygiene): host + path only.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DlStatus {
    DL_OK = 0,
    DL_INVALID_ARGUMENT = 1,
    DL_IO = 2,
    DL_NETWORK = 3,         /* connect/TLS/reset class failures */
    DL_HTTP = 4,            /* server answered with a non-2xx status */
    DL_NOT_FOUND = 5,       /* unknown handle */
    DL_UNAVAILABLE = 6,     /* temp dir unset or RNG/platform missing */
    DL_BUSY = 7,            /* terminal state reached — nothing to do */
    DL_BLOCKED = 8          /* the policy gate refused the request */
} DlStatus;

typedef enum DlState {
    DL_PROBING = 0,         /* Accept-Ranges / Content-Length probe */
    DL_RUNNING = 1,
    DL_MERGING = 2,         /* segments done, assembling output */
    DL_DONE = 3,
    DL_FAILED = 4,
    DL_CANCELLED = 5
} DlState;

typedef struct DlProgress {
    int32_t state;          /* DlState */
    int64_t bytes_done;
    int64_t bytes_total;    /* -1 while the server did not say */
    int64_t speed_bps;      /* smoothed instantaneous rate */
    int32_t connections;    /* segments in flight */
    int32_t reserved;
} DlProgress;

typedef uint64_t DlHandle;  /* registry id, never a raw pointer */

/* --- housekeeping --------------------------------------------------- */

/* Directory the engine creates its per-download 0700 part-file dirs
 * under — the Qt side passes BrowserPaths' data dir subfolder.
 * Must be called before dl_start; idempotent and cheap. */
DlStatus dl_set_temp_dir(const char *utf8Path);

/* 1 when the crate was built and linked — lets the UI show the
 * accelerated engine only when it is actually present. */
int dl_is_available(void);

/* --- policy gate (DLACC04) --------------------------------------------
 *
 * The custom engine bypasses QWebEngineUrlRequestInterceptor, so the
 * privacy/adblock gate the engine path gets for free must be replayed
 * on the Rust path — for the initial request AND every redirect hop
 * (Chromium does not re-run the interceptor on download redirects;
 * this engine deliberately does, which is stricter, not weaker).
 *
 * dl_set_gate installs a process-wide hook consulted before EVERY
 * wire request.  The hook runs on a Rust worker thread and must only
 * touch thread-safe state (the interceptor's lock-guarded policy
 * snapshots and AdBlockNetwork::match qualify; GUI objects do not).
 * It may be invoked concurrently by parallel downloads.
 *
 *   url         candidate request target for THIS hop
 *   prev_url    the URL that redirected us here ("" on the first
 *               request — a user-initiated LAN download stays legal)
 *   first_party the document URL the download was initiated from
 *               ("" when unknown) — the adblock first-party context
 *   scope       the requesting profile's HTTPS-First downgrade scope
 *               ("" is a valid scope)
 *   out_url     buffer for the DL_GATE_REWRITE target
 *   out_reason  buffer for a short DL_GATE_BLOCK explanation (host +
 *               rule class only — never the URL's query/fragment)
 *
 * Return values:
 *   DL_GATE_ALLOW    send the request to `url` as-is
 *   DL_GATE_BLOCK    refuse — the download fails with out_reason
 *   DL_GATE_REWRITE  replace `url` by out_url and re-run the gate on
 *                    it (https-first upgrades and url-strip land here)
 *
 * With no gate installed the crate allows everything — the Qt side
 * always installs one before the first dl_start. */
typedef enum DlGateAction {
    DL_GATE_ALLOW = 0,
    DL_GATE_BLOCK = 1,
    DL_GATE_REWRITE = 2
} DlGateAction;

typedef int (*DlGateFn)(const char *url_utf8,
                        const char *prev_url_utf8,
                        const char *first_party_utf8,
                        const char *scope_utf8,
                        char *out_url_utf8, size_t out_url_cap,
                        char *out_reason, size_t out_reason_cap,
                        void *ctx);

/* Install (or clear, with NULL) the process-wide gate.  Cheap and
 * idempotent; pass NULL `ctx` when the hook needs none. */
DlStatus dl_set_gate(DlGateFn fn, void *ctx);

char *dl_last_error_message(void);  /* "" when nothing failed */
void dl_string_free(char *s);

/* --- downloads ------------------------------------------------------- */

/* Starts a download.  `connections` is clamped to 1..16 (0 picks the
 * default 8); a server without Accept-Ranges always gets a single
 * stream regardless — the engine falls back internally rather than
 * bouncing the request back to Chromium.  `suggestedName` and
 * `cookieFile` may be NULL.  On success `outHandle` receives the id
 * and the worker owns the transfer until dl_free.
 *
 * `optionsJsonUtf8` (may be NULL) is a flat JSON object of policy
 * inputs the gate and header builder consult per hop (DLACC04):
 *   "user_agent"      profile UA string — the download must not be a
 *                     fingerprint diff from the page's fetches
 *   "first_party"     document URL the download was initiated from;
 *                     doubles as the Referer source and the adblock
 *                     first-party context
 *   "referer_policy"  int, mirrors PrivacyRequestInterceptor's REF01
 *                     levels: 0 strict-origin-when-cross-origin,
 *                     1 trimmed, 2 same-site-only, 3 never; an
 *                     https->http hop never carries a Referer
 *   "scope"           requesting profile's downgrade scope for the
 *                     gate's https-first/https-only decisions
 *   "proxy"           reqwest-style URL — "socks5h://h:p" (remote
 *                     DNS), "socks5://", "http://"; absent = direct
 *   "require_proxy"   bool — fail closed when no usable proxy is
 *                     configured (tor windows set this; a direct
 *                     clearnet socket from them is the release-
 *                     blocking bug this flag exists to prevent)
 * Unknown keys are ignored; absent keys pick the conservative default
 * (no UA override, no referer, direct fetch, allow). */
DlStatus dl_start(const char *urlUtf8,
                  const char *destDirUtf8,
                  const char *suggestedNameUtf8,
                  int32_t connections,
                  const char *cookieFileUtf8,
                  const char *optionsJsonUtf8,
                  DlHandle *outHandle);

DlStatus dl_poll(DlHandle handle, DlProgress *out);
DlStatus dl_cancel(DlHandle handle);
void dl_free(DlHandle handle);

/* Worker-side error ("" while none).  Free with dl_string_free(). */
char *dl_error_message(DlHandle handle);

/* Name shown in the card UI — the sanitized suggested file name the
 * engine adopted (resolved once the probe ran, so it can reflect the
 * server's Content-Disposition).  Free with dl_string_free(). */
char *dl_file_name(DlHandle handle);

/* Final destination path once DL_DONE ("" before).  Free with
 * dl_string_free(). */
char *dl_output_path(DlHandle handle);

#ifdef __cplusplus
}
#endif

#endif // ARORA_RUSTDL_H
