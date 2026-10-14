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

#include "engineindicator.h"

#include "engineinterface.h"
#include "engineregistry.h"
#include "safetext.h"

#include <qmenu.h>

EngineIndicator::EngineIndicator(QWidget *parent)
    : QToolButton(parent)
    , m_engineId(QStringLiteral("webengine"))
    , m_swappable(true)
    , m_menu(nullptr)
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setAccessibleName(tr("Page Engine"));
    setAccessibleDescription(
        tr("Shows the engine rendering this tab and offers to reload "
           "the page on another engine."));

    m_menu = new QMenu(this);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow,
            this, &EngineIndicator::rebuildMenu);

    refresh();
}

void EngineIndicator::setEngineId(const QString &id)
{
    m_engineId = id.isEmpty() ? QStringLiteral("webengine") : id;
    refresh();
}

void EngineIndicator::setSwappable(bool swappable)
{
    m_swappable = swappable;
    refresh();
}

void EngineIndicator::setLockReason(const QString &reason)
{
    m_lockReason = reason;
    refresh();
}

void EngineIndicator::refresh()
{
    Engine::Backend *backend = EngineRegistry::backendForId(m_engineId);
    setIcon(EngineRegistry::backendIcon(m_engineId, palette()));
    const QString name = backend
        ? backend->displayName() : m_engineId;
    const bool locked = isLocked();
    // ENG29: a backend caveat (Servo's experimental/debug-artifact
    // notes) rides the tooltip — the lightest surface that still
    // answers "why is this engine slow" at the glyph itself.
    const QString note = backend ? backend->engineNote() : QString();
    QString tip = locked
        ? tr("Rendered by %1 — %2").arg(name, m_lockReason)
        : tr("Rendered by %1").arg(name);
    if (!note.isEmpty())
        tip += QLatin1String(" — ") + note;
    setToolTip(tip);
    setStatusTip(locked
        ? tr("Engine locked: %1").arg(m_lockReason)
        : tr("Reload this page on another engine"));

    // ENG19: visible whenever an engine choice exists AND the tab
    // offers one — a locked tab stays shown-but-disabled so the
    // affordance explains itself instead of vanishing (the user
    // report behind ENG19 read the intentional tor hide as a
    // regression).  setSwappable(false) with no lock reason still
    // hides outright, and fewer than two registered backends hides
    // everything — there is no choice to explain.
    const bool multi = EngineRegistry::backends().count() >= 2;
    setEnabled(!locked && m_swappable);
    setVisible(multi && (m_swappable || locked));
}

void EngineIndicator::rebuildMenu()
{
    m_menu->clear();

    Engine::Backend *current = EngineRegistry::backendForId(m_engineId);
    QAction *header = m_menu->addAction(
        tr("Rendered by %1").arg(current
            ? current->displayName() : m_engineId));
    header->setEnabled(false);
    m_menu->addSeparator();

    for (Engine::Backend *backend : EngineRegistry::backends()) {
        if (backend->id() == m_engineId)
            continue;
        QAction *action = m_menu->addAction(
            SafeText::menu(tr("Reload in %1").arg(backend->displayName())));
        action->setIcon(
            EngineRegistry::backendIcon(backend->id(), palette()));
        // ENG19: a backend that would refuse the page right now —
        // Servo's single-view slot claimed or spent — reads as a
        // disabled entry with the reason, never a click that dies
        // silently.
        const QString unavailable = backend->pageUnavailableReason();
        if (!unavailable.isEmpty()) {
            action->setEnabled(false);
            action->setToolTip(unavailable);
            action->setStatusTip(unavailable);
        } else if (const QString note = backend->engineNote();
                   !note.isEmpty()) {
            // ENG29: a pickable backend with a caveat (Servo's
            // slower-than-Chromium baseline, escalated on a debug
            // artifact) explains itself on hover before the swap.
            action->setToolTip(note);
            action->setStatusTip(note);
        }
        connect(action, &QAction::triggered,
                this, [this, id = backend->id()]() {
            emit switchRequested(id);
        });
    }
}
