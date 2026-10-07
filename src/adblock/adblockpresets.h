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

#ifndef ADBLOCKPRESETS_H
#define ADBLOCKPRESETS_H

#include <qlist.h>
#include <qstring.h>

// ADB03: curated catalog of community filter lists shown by
// AdBlockPresetsDialog.  Everything here is opt-in — a preset only
// becomes a subscription when the user ticks its box (which is also
// the TELEM01 consent to download remote lists).
//
// Locations were verified live (HTTP 200) when added; keep them in
// ABP-compatible format — a leading "[Adblock Plus" banner or "!"
// comment metadata is what AdBlockSubscription::loadRules accepts.

struct AdBlockListPreset {
    QString category;    // group label shown in the dialog
    QString title;       // list name (also the subscription title)
    QString location;    // remote list URL
    QString description; // one-line summary
};

namespace AdBlockPresets {

QList<AdBlockListPreset> all();

}

#endif // ADBLOCKPRESETS_H
