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

#ifndef SANDBOXMANAGER_H
#define SANDBOXMANAGER_H

#include <qstring.h>

// SAND01 runtime side: applies the sandbox described by SandboxPolicy
// at process start.  Linux re-executes the binary under bwrap;
// macOS/BSD apply in-process (seatbelt/unveil); Windows re-execs into
// an AppContainer.  Non-Linux apply paths are UNTESTED-ON-TARGET —
// they are compiled only on their platforms and the loop cannot run
// them — while the policy-to-text generators they consume are unit
// tested here.
//
// Escape hatches (documented dev-only): the --no-sandbox flag, the
// ARORA_NO_SANDBOX env var, and the sandbox/enabled=false QSettings
// key.  When no backend exists on the host the manager warns once and
// continues — running unsandboxed is never a hard failure.
class SandboxManager
{
public:
    // True when this process is already inside the sandbox — the bwrap
    // argv sets ARORA_SANDBOXED so children of a wrapped browser
    // (tor respawn, single-instance forward) never re-wrap.
    static bool isSandboxed();

    // "bwrap" | "seatbelt" | "appcontainer" | "unveil" | "none".
    static QString backendName();

    // Human-readable report for --sandbox-status: backend state,
    // escape-hatch visibility, resolved denylist/read-only roots.
    static QString statusReport();

    // Call at the very top of main(), before any Qt object exists.
    // On a browsing launch this re-execs arora inside the sandbox and
    // never returns; dev/utility runs (any --flag except --tor) pass
    // through unwrapped so test harnesses keep deterministic
    // environments.  ARORA_SANDBOX_FORCE=1 wraps flagged runs too —
    // used by --sandbox-smoke's fallback check.
    static void maybeReexec(int argc, char **argv);
};

#endif // SANDBOXMANAGER_H
