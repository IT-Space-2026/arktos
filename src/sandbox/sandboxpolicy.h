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

#ifndef SANDBOXPOLICY_H
#define SANDBOXPOLICY_H

#include <qstringlist.h>

// SAND01: declarative sandbox policy — one rule list, consumed by every
// per-platform backend generator (Linux bwrap, macOS Seatbelt, Windows
// AppContainer, OpenBSD unveil/pledge).  The policy is permissive: the
// whole filesystem stays read/write EXCEPT the denylist (credential
// stores, keyrings, other browsers' profiles) which is masked, and the
// read-only roots (/etc /usr /boot) which are remounted read-only.
// Network access is intentionally unrestricted — a browser needs it.
//
// deniedDirs/deniedFiles are split because the backends mask them
// differently (bwrap mounts a tmpfs over a directory but cannot do so
// over a file — it needs a /dev/null bind instead; Seatbelt emits
// subpath vs literal rules).  Denied paths that do not exist on the
// host are dropped entirely by defaultPolicy(): bwrap would create the
// missing mount point on the real filesystem, and hiding an absent
// path buys nothing.
class SandboxPolicy
{
public:
    QString homeDir;
    QStringList deniedDirs;
    QStringList deniedFiles;
    QStringList readOnlyRoots;

    // $HOME-relative denylist shared by every backend.  Covers
    // credentials, key material, cloud/CLI config, keyrings and the
    // profile/cookie stores of other browsers.  It is a denylist on a
    // permissive policy — best-effort by design, not exhaustive.
    static QStringList homeRelativeDeniedPaths();

    // System roots remounted read-only by the generators.  Entries are
    // filtered to paths that exist when defaultPolicy() builds a
    // policy; the launcher script repeats the check at run time.
    static QStringList readOnlyRootCandidates();

    // Builds the policy for this host: expands the home-relative
    // denylist to absolute paths and buckets each entry by kind
    // (dir/symlink-to-dir -> deniedDirs, file/other -> deniedFiles,
    // missing -> dropped).  Read-only roots are filtered to existing
    // directories.
    static SandboxPolicy defaultPolicy();
};

#endif // SANDBOXPOLICY_H
