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

#ifndef SITEBINDING_P_H
#define SITEBINDING_P_H

// CA08: the page-binding boilerplate the location-bar site widgets
// (SitePanel, SitePadlockButton, SiteShieldButton) each carried —
// the WebView/EngineTab disconnect-swap dance, the live-backend
// probe, and the trust-list refresh wiring.

#include "domainblocklist.h"
#include "engineinterface.h"
#include "engineregistry.h"
#include "enginetab.h"
#include "siteshield.h"
#include "trancolist.h"
#include "webview.h"

#include <qobject.h>

// Drops both page bindings — the disconnect order every bound
// widget runs before swapping its view/tab pointer (the engine-tab
// binding clears first: an early return while it is still set would
// keep the wrong tab's connections alive).
template <typename ViewPtr, typename TabPtr>
inline void unbindPageBinding(QObject *owner, ViewPtr &view,
                              TabPtr &tab)
{
    if (tab) {
        QObject::disconnect(tab, nullptr, owner, nullptr);
        tab = nullptr;
    }
    if (view)
        QObject::disconnect(view, nullptr, owner, nullptr);
    view = nullptr;
}

// The tab's backend pointer is raw — a stack-owned backend (the
// autotest fakes) can die while a bound tab still stands, and
// refresh paths still run during tab-stack teardown.  Keep it as a
// QPointer so a dead backend reads as absent; the registry
// membership probe is a plain pointer compare, safe even if the
// backend is already gone.
inline Engine::Backend *liveEngineBackend(EngineTab *tab)
{
    Engine::Backend *backend = tab ? tab->backend() : nullptr;
    return (backend && EngineRegistry::backends().contains(backend))
        ? backend : nullptr;
}

// The trust-list refresh feeds shared by every trust-aware
// location-bar widget: a refreshed threat-list or Tranco table can
// change the verdict for already-seen hosts, so the shared level
// cache drops and the widget repaints on the same signals.  Slot
// order across objects is not guaranteed, so each widget re-clears —
// a second clear is a no-op.
template <typename Receiver, typename Fn>
inline void wireTrustListRefresh(Receiver *receiver, Fn refresh)
{
    QObject::connect(DomainBlocklist::instance(),
                     &DomainBlocklist::updateFinished,
                     receiver, [receiver, refresh](bool) {
        SiteShieldButton::clearTrustCache();
        refresh();
    });
    QObject::connect(TrancoList::instance(),
                     &TrancoList::updateFinished,
                     receiver, [receiver, refresh](bool) {
        SiteShieldButton::clearTrustCache();
        refresh();
    });
}

#endif // SITEBINDING_P_H
