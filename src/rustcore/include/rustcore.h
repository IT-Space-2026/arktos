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

#ifdef __cplusplus
}
#endif

#endif // ARORA_RUSTCORE_H
