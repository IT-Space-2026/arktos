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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef SITEDECISIONSTORE_H
#define SITEDECISIONSTORE_H

// SITED01: the Qt façade over rustcore's rc_sitedec_* ABI — one
// durable, engine-neutral store behind sitedecisions.json for every
// "decision for a site" record (permissions, JS rules, container
// assignments, pop-up exceptions, HTTPS allowances, cookie rules and
// the adblock whitelist).  Only compiled in rustcore builds; the
// managers keep their QSettings paths for CONFIG+=no-rust.

#include <qhash.h>
#include <qstring.h>

namespace SiteDecisionStore {

// Decision kinds — one per migrated legacy store.  Values are the
// wire strings persisted in sitedecisions.json; never rename one
// without migrating the file.
inline constexpr char KindWebPermission[] = "webperm";
inline constexpr char KindJavaScript[] = "js";
inline constexpr char KindContainer[] = "container";
inline constexpr char KindPopup[] = "popup";
inline constexpr char KindHttpAllow[] = "http-allow";
inline constexpr char KindCookie[] = "cookie";
inline constexpr char KindAdBlock[] = "adblock";

// Fetch the stored value; false when no row exists (or the call
// failed — callers treat both as "no decision").
bool get(const char *kind, const QString &key, QString *value);
// Record/overwrite a row; false only on FFI failure.
bool set(const char *kind, const QString &key, const QString &value);
bool remove(const char *kind, const QString &key);
bool clear(const char *kind);
// Replace kind's rows wholesale — the "in-memory list is
// authoritative" mirror-write used by the list-shaped stores.
bool replace(const char *kind, const QHash<QString, QString> &entries);
// Every row of kind, sorted by key.
QHash<QString, QString> entries(const char *kind);
// The whole store as a JSON document — the read-locked policy
// snapshot for IO-thread consumers.
QByteArray snapshot();
// Longest-suffix host match: fills *value (and *matchedKey when
// non-null) for the rule governing host.  False when nothing matches.
bool lookup(const char *kind, const QString &host, QString *value,
            QString *matchedKey = nullptr);
// Whether the store file exists — the legacy-import gate.
bool storePresent();
// Re-read the disk file (test/repair seam).
bool reload();
// Empty every kind — test-suite reset / clear-site-data path.
bool reset();

} // namespace SiteDecisionStore

#endif // SITEDECISIONSTORE_H
