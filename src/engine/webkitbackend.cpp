/*
 * Copyright 2026 The Arktos Authors
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

#include "webkitbackend.h"

// glib/WPE headers declare a struct member named `signals` — Qt's
// keyword macros must be out of the way for the include (the harness
// used no_keywords; here the undef is scoped to this TU so the rest
// of the app keeps the signal/slot spellings — use the Q_EMIT forms
// below the include if this file ever needs them).
#ifdef ARKTOS_WEBKIT_LINKED
#ifdef signals
#undef signals
#endif
#ifdef slots
#undef slots
#endif
#ifdef emit
#undef emit
#endif
#include <wpe/webkit.h>
#endif

WebkitBackend::WebkitBackend(QObject *parent)
    : Engine::Backend(parent)
{
}

bool WebkitBackend::librariesLinked()
{
#ifdef ARKTOS_WEBKIT_LINKED
    return true;
#else
    return false;
#endif
}

QString WebkitBackend::wpeVersion()
{
#ifdef ARKTOS_WEBKIT_LINKED
    // The call is also what keeps the DT_NEEDED on libWPEWebKit real
    // — an unreferenced link would let --as-needed drop the library
    // and make the gate's "links with libs present" claim vacuous.
    return QStringLiteral("%1.%2.%3")
            .arg(webkit_get_major_version())
            .arg(webkit_get_minor_version())
            .arg(webkit_get_micro_version());
#else
    return QString();
#endif
}

QString WebkitBackend::id() const
{
    return QStringLiteral("webkit");
}

QString WebkitBackend::displayName() const
{
    const QString version = wpeVersion();
    if (!version.isEmpty())
        return QStringLiteral("WebKit %1").arg(version);
    return QStringLiteral("WebKit");
}

QString WebkitBackend::engineNote() const
{
    return tr("Experimental engine — WPE WebKit backend (early)");
}

Engine::Capabilities WebkitBackend::capabilities() const
{
    // WK06 build gate: nothing is proven or wired yet — every bit
    // stays off.  WK07 fills the conservative spike-verified table.
    Engine::Capabilities caps;
    caps.downloads = false;
    return caps;
}

bool WebkitBackend::initialize()
{
    // Direct linking IS the loadable probe — a libWPEWebKit that
    // failed to resolve would have stopped the process at exec.
    // WK07 keeps this shape while adding real startup work.
    return librariesLinked();
}

Engine::Profile *WebkitBackend::createProfile(
        const Engine::ProfileOptions &, QObject *)
{
    return nullptr;
}

Engine::Page *WebkitBackend::createPage(Engine::Profile *, QObject *)
{
    return nullptr;
}

QString WebkitBackend::pageUnavailableReason() const
{
    if (!librariesLinked())
        return tr("the WPE WebKit libraries were not found at build "
                  "time");
    return tr("the WebKit backend cannot host pages yet");
}

QWidget *WebkitBackend::createView(Engine::Page *, QWidget *)
{
    return nullptr;
}
