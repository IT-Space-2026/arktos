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

#ifndef PLATFORMGENERATORS_H
#define PLATFORMGENERATORS_H

#include <qstring.h>
#include <qstringlist.h>

class SandboxPolicy;

// SAND01 non-Linux backends.  Every function is a pure text generator
// driven by the same SandboxPolicy — they are fully unit-tested on
// Linux even though the apply paths behind them are compiled only on
// their target platforms (and are therefore UNTESTED-ON-TARGET).

namespace SeatbeltGenerator {

// macOS sandbox-exec / sandbox_init() profile (Scheme-ish SBPL).  The
// policy is permissive: allow default, then deny reads and writes
// under each denylist path — file-write* is denied as well because
// read-only protection still leaves ~/.ssh/authorized_keys writable.
QString profile(const SandboxPolicy &policy);

} // namespace SeatbeltGenerator

namespace AppContainerGenerator {

// Windows 10/11 AppContainer capability manifest consumed by the
// ifdef'd CreateAppContainerProfile launcher path in sandboxmanager.
// There is no standard file format for this — the schema is ours:
// capabilities the process keeps (browsing needs networking) plus the
// same deny/read-only rules as declarative FileRule entries.
QString manifestXml(const SandboxPolicy &policy);

} // namespace AppContainerGenerator

struct OpenBsdPlan {
    // Ordered unveil(2) calls as "path|permissions" pairs ("" perms =
    // hidden).  A broad rwc grant over $HOME comes first, denylist
    // carve-outs after it — the last unveil touching a path wins.
    QStringList unveilRules;
    // pledge(2) promise sets — intentionally broad: a browser cannot
    // be meaningfully pledged without engine cooperation, so this is
    // documented hardening-of-helper-processes, not strong isolation.
    QString pledgePromises;
};

namespace OpenBsdGenerator {

OpenBsdPlan plan(const SandboxPolicy &policy);

} // namespace OpenBsdGenerator

#endif // PLATFORMGENERATORS_H
