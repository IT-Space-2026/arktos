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

#ifndef ENGINEREGISTRY_H
#define ENGINEREGISTRY_H

// ENG05 — the process-wide engine registry.
//
// The QtWebEngine backend is always present (registered lazily on
// first lookup).  Alternate backends join through registerBackend():
// the Servo backend when the tree is built with `qmake servo=1` and
// the servo-embed artifact resolves at runtime, fake backends in the
// autotests.  The registry does NOT own the backends it lists — a
// backend removed before it is destroyed must be unregistered.
//
// defaultBackendId() persists under "MainWindow/defaultEngine" and
// drives the engine new explicit tabs open on; an id that is no
// longer registered resolves back to WebEngine so a stale pref can
// never strand the New Tab action.

#include <qicon.h>
#include <qlist.h>
#include <qstring.h>

class QPalette;

namespace Engine {
class Backend;
}

namespace EngineRegistry {

QList<Engine::Backend *> backends();
Engine::Backend *backendForId(const QString &id);
void registerBackend(Engine::Backend *backend);
void unregisterBackend(Engine::Backend *backend);

// A backend engineregistry.cpp cannot reference directly (the servo
// translation unit only exists in servo=1 objects, while shared
// subproject builds link engineregistry.o unconditionally) installs a
// probe from its own static init; backends() runs each probe once.
void addProbe(void (*probe)());

QString defaultBackendId();
void setDefaultBackendId(const QString &id);
// The default resolved to a live backend — never null while the
// WebEngine backend exists.
Engine::Backend *defaultBackend();

// A small palette-safe engine glyph for chrome surfaces (the URL-bar
// indicator, the settings combo): a rounded badge carrying the
// backend's initial.  Generated rather than themed — no bundled icon
// set ships per-engine artwork.
QIcon backendIcon(const QString &id, const QPalette &palette,
                  int extent = 16);

} // namespace EngineRegistry

#endif // ENGINEREGISTRY_H
