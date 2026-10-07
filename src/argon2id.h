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

#ifndef ARORA_ARGON2ID_H
#define ARORA_ARGON2ID_H

// Bundled Argon2id (version 0x13) key-derivation, pure C99, no OS
// dependencies — SEC13 requires the master-passphrase KDF to build
// identically on every platform, so the algorithm is vendored here
// rather than resolved from a system library that may not exist.
// Implements the raw-hash core of RFC 9106 (no secret / associated-
// data inputs, no encoding string — SecureStore only needs key bytes).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Derives outlen bytes of key material from the password using
// Argon2id with the given time cost (passes), memory cost (KiB) and
// lane count.  Returns 0 on success, -1 on invalid parameters or
// allocation failure.  The output buffer is always wiped on failure.
int arora_argon2id(void *out, size_t outlen,
                   const void *password, size_t passwordLen,
                   const void *salt, size_t saltLen,
                   uint32_t timeCost, uint32_t memoryKiB,
                   uint32_t lanes);

#ifdef __cplusplus
}
#endif

#endif // ARORA_ARGON2ID_H
