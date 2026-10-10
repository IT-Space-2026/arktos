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

// ENG05 — the per-tab engine glyph at the right end of the URL bar
// (Edge IE-mode style).  Shows which engine the tab runs on; clicking
// opens a menu offering a reload of the current url on each OTHER
// registered backend.  The button reports the request — the tab
// widget resolves which tab the bar belongs to and performs the swap.
//
// Hidden entirely while fewer than two backends are registered (a
// missing servo-embed artifact leaves no dead menu), in Tor windows
// (Tor is locked to Chromium — servo has no SOCKS5), and on tabs the
// swap refuses (private tabs: servo has no off-the-record profile).

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

    // A tab the swap path refuses (private/tor contexts) keeps the
    // button hidden even when alternate backends exist.
    void setSwappable(bool swappable);
    bool isSwappable() const { return m_swappable; }

    void refresh();

signals:
    void switchRequested(const QString &engineId);

private:
    void rebuildMenu();

    QString m_engineId;
    bool m_swappable;
    QMenu *m_menu;
};

#endif // ENGINEINDICATOR_H
