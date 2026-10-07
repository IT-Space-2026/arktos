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

#ifndef SECURESTORE_H
#define SECURESTORE_H

#include <qbytearray.h>
#include <qstring.h>

// Authenticated encryption for credentials the app itself persists at
// rest (autofill.dat form passwords, the proxy password in QSettings).
// AES-256-GCM through the system OpenSSL EVP API, resolved at runtime
// with QLibrary — there are no OpenSSL development headers to link
// against, so this mirrors the dlopen pattern Qt's own TLS backend
// plugin uses.
//
// The 256-bit key is generated once and stored as securestore.key in
// the application data directory with 0600 permissions.  That is the
// same guarantee Chromium gives on Linux without a desktop keyring
// (where OSCrypt falls back to a hardcoded password): it defeats
// offline and other-user reads of the store, not malware already
// running as the same uid.  See .devin/SEC03-report.md.
//
// Optional master-passphrase mode (SEC13): when the user sets a
// passphrase the key is Argon2id-derived in memory instead — the
// params/salt file securestore.kdf replaces securestore.key and no
// key material exists on disk; ciphertext then needs the passphrase.
// The derived key lives in RAM only and is zeroized by lock().
class QWidget;

namespace SecureStore {

// True when the OpenSSL EVP symbols resolved.  When false seal()
// returns an empty blob and open() fails — callers decide how to
// degrade (the autofill store refuses to persist password forms).
bool isAvailable();

// True when blob carries the sealed-format magic
// ("ARSEC1" | nonce 12 | tag 16 | ciphertext).
bool isSealed(const QByteArray &blob);

// Encrypt + authenticate.  Returns an empty array when the crypto
// backend or the key file is unavailable.
QByteArray seal(const QByteArray &plain);

// Verify + decrypt a sealed blob.  *ok is false on bad magic,
// truncated input or a GCM authentication (tamper) failure; the
// returned array is empty in that case.
QByteArray open(const QByteArray &blob, bool *ok = nullptr);

// QSettings-friendly wrappers: sealString() returns
// "arsec1:<base64>" when the backend works and the unchanged input
// otherwise (keep the old plaintext behavior rather than lose the
// setting).  openString() decrypts the prefixed form and passes
// legacy plaintext values through so existing settings keep working.
QString sealString(const QString &plain);
QString openString(const QString &stored, bool *ok = nullptr);

// ---- master-passphrase key custody (SEC13) ----

// True when securestore.kdf exists: the encryption key is derived
// from a master passphrase and no securestore.key file is used.
bool passphraseProtectionEnabled();

// False only in passphrase mode while locked.  seal()/open() try an
// interactive unlock prompt first (when a QApplication exists and
// interactive unlock is enabled) and fail securely when it is denied.
bool isUnlocked();

// Derives the key from the passphrase and verifies it against the
// verifier stored in securestore.kdf.  A wrong passphrase returns
// false cleanly (the GCM tag on the verifier rejects it — no oracle).
bool unlock(const QString &passphrase);

// Drops the cached derived key and wipes it from RAM.  Also clears
// any cached file key so lock() is safe to call unconditionally.
void lock();

// Interactive variant of unlock(): prompts up to three times via a
// password-echo dialog.  Returns the resulting unlocked state.
// Non-interactive contexts (no QApplication, or disabled via
// setInteractiveUnlockEnabled) simply return isUnlocked().
bool ensureUnlocked(QWidget *parent = nullptr);
void setInteractiveUnlockEnabled(bool enabled);
bool isInteractiveUnlockEnabled();

// Mode transitions.  Each re-seals the known consumer stores
// (autofill.dat, the proxy password setting) from the old key to the
// new one before switching custody.  enable/change create a fresh
// salt; disable writes a fresh random securestore.key.  All return
// false with *error describing the failure; change and disable
// require the store to already be unlocked.
bool enablePassphraseProtection(const QString &passphrase,
                                QString *error = nullptr);
bool disablePassphraseProtection(QString *error = nullptr);
bool changePassphrase(const QString &newPassphrase,
                      QString *error = nullptr);

#ifdef AUTOTESTS
// Clears every cached key/param and deletes both store files — test
// isolation helper, compiled only into autotest binaries.
void resetForTests();
#endif

} // namespace SecureStore

#endif // SECURESTORE_H
