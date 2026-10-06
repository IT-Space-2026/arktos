# SEC03 — credential / sensitive storage audit

Audit of everything Arora persists at rest, what is protected, and what
this task changed. Date: 2026-10-06. Tree: Qt 6.11.3 / QtWebEngine port.

## What the app itself writes

| Store | Path (under `~/.local/share/Arora/` unless noted) | Contents | Protection |
|---|---|---|---|
| `autofill.dat` | app data dir | saved form data incl. **passwords** | **was plaintext QDataStream — now AES-256-GCM sealed** (`ARSEC1` blob, this task) |
| `securestore.key` | app data dir | 256-bit seal key (new, this task) | 0600 file perms |
| `proxy/password` | QSettings ini (`~/.config/Arora/`) | **proxy auth password** | **was plaintext — now `arsec1:` base64 sealed value** (this task) |
| `history` | app data dir | URLs, titles, visit counts | plaintext (browsing metadata, same class as Chrome's unencrypted History db) |
| `bookmarks.xbel` | app data dir | bookmarks | plaintext |
| `searchengines/` | app data dir | OpenSearch XML | plaintext |
| `lastsession` | QSettings | session tab URLs (+inline history items) | plaintext |
| `adblock_subscription_*` | app data dir | filter lists | plaintext |
| `userscripts/` | app data dir | user-supplied JS (user-installed) | plaintext, user-provided |
| NAM disk cache | `~/.cache/Arora/` | app-side fetches: OpenSearch icons, adblock lists, **suggestion responses (typed query text)** | plaintext — Chromium equivalent is also unencrypted; noted, no credentials |
| WebEngine profile "arora" | `QtWebEngine/arora/` | Cookies, Local Storage, IndexedDB, Service Worker, Cache Storage, Favicons | Chromium-managed: cookie **values** are OSCrypt-encrypted; on Linux without a keyring OSCrypt uses its built-in fallback key → obfuscation only, same guarantee class as our key file. DOM storage/session files are plaintext per upstream Chromium design. |
| OTR profile | in-memory | private browsing state | nothing persisted |

## Changes made

- New `src/securestore.{h,cpp}`: AES-256-GCM seal/open over the system
  OpenSSL EVP API, resolved at runtime via `QLibrary` (libcrypto.so.3 /
  .so.1.1 / .so.1.0.2 / .so) — no OpenSSL dev headers exist on target
  machines, mirroring how Qt's own TLS backend plugin loads OpenSSL.
  Blob format: `"ARSEC1" | 12-byte nonce | 16-byte GCM tag | ct`.
  Key: 32 bytes from `QRandomGenerator::system()`, written once to
  `securestore.key` via `QSaveFile` and forced to 0600.
- `autofill.dat`: `saveFormData()` seals the QDataStream payload;
  `loadFormData()` detects the magic and unseals, falling back to the
  legacy plaintext stream for migration (rewritten sealed on next
  save). Written atomically via `QSaveFile`, perms forced 0600.
  Fail-secure: when the crypto backend is unavailable and any stored
  form has a password, the file is NOT written rather than persisting
  credentials as plaintext (in-memory autofill still works that
  session; password-free forms keep the legacy plaintext path).
- `proxy/password` QSettings value: sealed via `SecureStore::
  sealString()` on save (`arsec1:` prefix), transparently unsealed or
  passed-through on read at both consumers (settings dialog, NAM).
- The existing Preferences → "store password forms" checkbox remains
  the opt-out gate for password capture entirely.

## Threat model / honest limits

- Key beside the data (0600, dir already 0700) defeats: other local
  users, offline disk/backup reads, casual file snooping. It does NOT
  defeat malware running as the same uid — identical to Chromium's
  no-keyring fallback. No system keyring (libsecret/kwallet) is
  integrated; candidate follow-up for PACK01-era hardening.
- History/bookmarks/session/DOM-storage are plaintext by design, as in
  mainstream Chromium — full profile confidentiality needs OS-level
  disk encryption, out of browser scope.
- The NAM suggestion cache can hold typed query text; covered by
  SEC11's suggestion-privacy review rather than at-rest encryption.

## Verification

- `--autofill-smoke` extended: asserts `autofill.dat` carries the
  `ARSEC1` magic and contains neither captured credential string in
  plaintext, plus SecureStore round-trip, tamper rejection (flipped
  byte → GCM auth failure) and the `arsec1:` string form. PASS, exit 0.
- `make check` programs `tst_settingsdialog` 6/6 and
  `tst_networkaccessmanager` 9/9 PASS offscreen; `--quit-after-load`,
  `--nam-smoke`, `--cookie-smoke`, `--settings-smoke` exit 0.
