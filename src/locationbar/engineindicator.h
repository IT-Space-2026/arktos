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

#ifndef ENGINEINDICATOR_H
#define ENGINEINDICATOR_H

// ENG05/ENG08 — the per-tab engine glyph as a toolbar button at the
// right of the url-bar stack (Edge IE-mode style).  One window-level
// instance shows which engine the ACTIVE tab runs on; clicking opens
// a menu offering a reload of the current url on each OTHER
// registered backend.  The button reports the request — the window
// resolves it against the current tab and performs the swap.
//
// Hidden entirely while fewer than two backends are registered (a
// missing servo-embed artifact leaves no dead menu) and on tabs with
// no engine at all (widget tabs — the host cell hides).  ENG19: a
// HARD engine lock — tor window, private tab, active container level —
// keeps the button visible but disabled with the reason in its
// tooltip: a vanished affordance reads as a bug (the ENG19 report was
// a tor window, where hiding is correct but unexplained), while a
// greyed glyph answers "why can't I switch" in place.  Swap is never
// enabled under a lock — the button only explains.

#include <qpointer.h>
#include <qtoolbutton.h>

class QMenu;

class EngineIndicator : public QToolButton
{
    Q_OBJECT

public:
    explicit EngineIndicator(QWidget *parent = nullptr);

    void setEngineId(const QString &id);
    QString engineId() const { return m_engineId; }

    // A tab the swap path refuses without a hard-lock reason keeps
    // the button hidden even when alternate backends exist (legacy
    // contract for callers that never set a lock reason).
    void setSwappable(bool swappable);

    // ENG19: a non-empty reason renders the button disabled-but-
    // visible with the explanation in its tooltip/status tip — the
    // swap stays unreachable, the lock explains itself.
    void setLockReason(const QString &reason);
    QString lockReason() const { return m_lockReason; }
    bool isLocked() const { return !m_lockReason.isEmpty(); }

    void refresh();

signals:
    void switchRequested(const QString &engineId);

private:
    void rebuildMenu();

    QString m_engineId;
    bool m_swappable;
    QString m_lockReason;
    QMenu *m_menu;
};

#endif // ENGINEINDICATOR_H
