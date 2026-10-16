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

#ifndef BROWSERPATHS_H
#define BROWSERPATHS_H

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qstandardpaths.h>
#include <qstring.h>

// Application data-directory helpers that used to live on
// BrowserApplication.
//
// MPROF01 layout: the data root holds machine-global payloads plus a
// profiles/<id>/ directory per browser profile.
//   - dataFilePath() resolves INSIDE the active profile's dir
//     (profiles/default/ today) so every existing call site lands in
//     the profile-owned store untouched — that redirect is the single
//     mechanism chosen by the MPROF01 spec; call sites for
//     machine-global files were moved to globalDataFilePath()
//     instead.  MPROF02+ migrates profile call sites to real
//     per-profile lookups.
//   - globalDataFilePath() stays at the root for shared, persona-free
//     state (the tor bundle + its data/docs dirs, locale drops, the
//     crash log, the profile-wipe marker, profiles.json).  The
//     QtWebEngine/ store tree lived here too until VSEC02 moved it
//     inside each persona's dir (see engineStorageDir).  Note the
//     tranco/blocklist
//     caches live INSIDE the profile dir instead — rustcore reads
//     them from its single data dir, which anchors to the profile
//     store.
namespace BrowserPaths {

// Root of the app's writable data store
// (QStandardPaths::AppDataLocation), created on demand.
QString dataRoot();

// <data root>/<fileName> — machine-global payloads that must NOT move
// into a profile.
QString globalDataFilePath(const QString &fileName);

// <data root>/profiles/<profileId>/ — a browser profile's private
// store.  An id that fails isProfileId() falls back to "default" so a
// bad id can never escape the profiles/ tree.
QString profileDataDir(const QString &profileId);
QString profileDataFilePath(const QString &profileId,
                            const QString &fileName);

// Profile id the process's unscoped stores belong to (MPROF03):
// the "ambient" persona — pushed by BrowserMainWindow at construction
// and again on every window activation so call sites without their
// own profile context (menus, dialogs, lazy panels) follow the
// focused window.  Non-window callers see the last active window's
// persona; before any window exists it is "default".
QString activeProfileId();

// Pushes the ambient persona.  Only profile-slug ids are honored —
// anything else (including an empty string) resolves to "default" so
// a bogus id can never steer unscoped stores outside profiles/.
void setActiveProfileId(const QString &profileId);

// Slug check shared with ProfileRegistry: lowercase letters, digits,
// '-'/'_' — an id is a path segment, so anything else is refused.
bool isProfileId(const QString &id);

// <active profile dir>/<fileName>.  Was
// BrowserApplication::dataFilePath(); MIG15 should delegate to this.
QString dataFilePath(const QString &fileName);

// Idempotent first-boot migration to the profiles/<id>/ layout:
// creates profiles/default/, renames the pre-MPROF01 profile-owned
// payloads into it (resumable — a missing source means already moved
// or never existed), and writes the migration-v1.done marker only on
// a clean pass.  dataFilePath()/profileDataFilePath() run it lazily
// once per process; tests may call it directly to re-verify after
// reseeding the root.  Returns false and fills *error when a move
// fails — treat a false return as LOUD, never as split state.
bool ensureDataLayout(QString *error = nullptr);

// VSEC02: the ENGINE side of a persona's store — where its named
// QWebEngineProfile persists cookies, LocalStorage/IndexedDB,
// service workers and visited links.  Rooted inside profiles/<id>/
// so the profile vault's rv_seal packs it with the app-side stores
// (VSEC01 F2: at the data root it stayed plaintext through a lock).
//   engineStorageDir -> profiles/<id>/QtWebEngine   (sealed)
//   engineCacheDir   -> profiles/<id>/cache/QtWebEngine
// The cache deliberately lands under the profile's "cache" name —
// the vault's existing exclusion holds it aside unsealed, the honest
// "disposable plaintext" posture the app cache already has (sealing
// a multi-hundred-MB HTTP cache would buy nothing it promises).
QString engineStorageDir(const QString &profileId);
QString engineCacheDir(const QString &profileId);

// VSEC02 one-time relocation of the pre-change layout: moves
// <data root>/QtWebEngine/arora[-<id>]/ into each persona's
// engineStorageDir() — except when a sealed vault blob owns the
// plaintext right now (state Locked or SealInterrupted), where the
// tree parks in <id>.vault/engine-hold/ instead and unseal()
// restores it next to the unpacked data.  Same never-follow-
// symlinks discipline as ensureDataLayout: the QtWebEngine dir, the
// payload entry and the destination chain are lstat-checked and a
// planted link or a both-sides collision fails the pass LOUDLY —
// nothing moves.  "arora-container-*" entries are container residue,
// not personas, and stay put.  The legacy <cache root>/QtWebEngine/
// arora* dirs are disposable — removed, not migrated.
// Marker-free and idempotent: it runs every boot (main() after
// ensureDataLayout) and lazily inside the profile factories, so it
// must be called before any named QWebEngineProfile is constructed —
// a profile that already opened storage cannot be re-pointed.
bool ensureEngineStorageLayout(QString *error = nullptr);

} // namespace BrowserPaths

#endif // BROWSERPATHS_H
