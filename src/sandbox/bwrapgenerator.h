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

#ifndef BWRAPGENERATOR_H
#define BWRAPGENERATOR_H

#include <qstring.h>
#include <qstringlist.h>

class SandboxPolicy;

// SAND01 Linux backend: translates a SandboxPolicy into the bubblewrap
// command line that re-executes the browser, and into the self-contained
// `arora-sandbox` launcher script installed next to the binary.  Both
// functions are pure — they map inputs to output text without touching
// the filesystem — so they are fully unit-testable on any platform.
namespace BwrapGenerator {

// Complete argv for execvp(): bwrap flags first, then `-- program
// args...`.  Environment variables are inherited (bwrap keeps them by
// default); ARORA_SANDBOXED is set inside so wrapped children never
// try to wrap themselves again.
QStringList commandLine(const SandboxPolicy &policy,
                        const QString &program,
                        const QStringList &programArgs);

// The launcher script installed as `arora-sandbox` next to the binary.
// It is host-agnostic: the denylist is baked in as $HOME-relative
// entries and each path's kind (dir -> tmpfs mask, file -> /dev/null
// bind, missing -> skipped) is resolved when the script runs, matching
// SandboxPolicy::defaultPolicy()'s bucketing.
QString launcherScript();

} // namespace BwrapGenerator

#endif // BWRAPGENERATOR_H
