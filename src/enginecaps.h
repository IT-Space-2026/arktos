/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#ifndef ENGINECAPS_H
#define ENGINECAPS_H

// MS05: the Qt-side read of ac_engine_caps — the facade's static
// per-engine capability registry (rustcore ABI v7; the MB04 table
// ported from the mw-bridge lane).  Chrome asks "can this engine
// do X" here instead of reading Backend::capabilities() or
// hardcoding backend->id() checks; answers are cached per engine
// id, which is safe because the registry is static by design — it
// reports what an engine supports when its backend is present,
// never whether the backend resolved this run (backend PRESENCE
// stays the engine registry's runtime question).
//
// no-rust builds, unregistered engine ids and FFI failures answer
// Unknown so callers fall back to the adapter-local truth they
// used before MS05 — the facade refines the verdict, it never
// invents a worse one.

#include <qstring.h>

class EngineCaps
{
public:
    enum class State {
        Supported,
        Partial,
        Unsupported,
        Unknown,    // no-rust build, unregistered id, FFI failure
    };

    // The registry grade for `feature` on `engineId` — the
    // backend->id() spelling is fine ("webengine" resolves to the
    // qtwebengine row).
    static State featureState(const QString &engineId,
                              const QString &feature);

    // The caveat note a graded feature row carries; empty when the
    // row has none or the lookup failed.
    static QString featureNote(const QString &engineId,
                               const QString &feature);

    // The gate question — "may this surface light up": Supported or
    // Partial answers true, Unsupported false, and Unknown returns
    // the caller's `fallback` (its pre-MS05 adapter-local truth).
    static bool featureUsable(const QString &engineId,
                              const QString &feature, bool fallback);

    // MS06: a one-line rollup of an engine's non-supported grades on
    // the chrome-facing features — "unsupported: reader mode,
    // extensions" / "limited: session restore" — for the surfaces
    // that annotate a whole backend (settings engine combo, engine-
    // indicator menu).  Empty when every listed feature grades usable
    // AND when the registry cannot grade the id at all (no-rust,
    // unregistered, FFI failure), so callers append nothing and keep
    // their pre-MS06 tooltip verbatim.
    static QString capabilitySummary(const QString &engineId);
};

#endif // ENGINECAPS_H
