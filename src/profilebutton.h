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

#ifndef PROFILEBUTTON_H
#define PROFILEBUTTON_H

#include <qtoolbutton.h>

class QAction;
class QLabel;
class QMenu;

// PROF01: shared base for the nav bar's oversized-face buttons —
// the app-menu logo (MENU17/MENU19) and the profile avatar.  A
// widget added through QToolBar::addWidget keeps its own iconSize
// property, but QToolButton::initStyleOption unconditionally swaps
// it for the parent toolbar's, so a plain setIconSize() changes the
// accessor value and never the pixels.  This base restores its own
// iconSize on the style option at the two read sites — paint and
// sizeHint — so the glyph grows while every sibling stays at the
// bar's size.  The face caps at a square one pixel over the glyph
// and NavBarToolBar's compacted vertical margins hand the extra
// pixels back, so the row keeps the height the location field
// drives.
class NavBarAccentButton : public QToolButton
{
public:
    explicit NavBarAccentButton(QWidget *parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};

// PROF01: the profile button — the rightmost widget on the
// navigation bar, face-matched to the app-menu logo on the left.
// Its face is the circular avatar (the bundled silhouette until
// PROF02 lets the user pick a picture); clicking drops a menu whose
// header shows avatar + profile name (app-global via
// ProfileIdentity, live across every window) and whose first row
// renames the profile through a small dialog.
class ProfileButton : public NavBarAccentButton
{
    Q_OBJECT

public:
    explicit ProfileButton(QWidget *parent = nullptr);

    QMenu *profileMenu() const;
    // Repaints face + header from ProfileIdentity — the identity's
    // signals already drive it; exposed so a late iconSize change
    // can rebake the avatar at the final size.
    void refreshIdentity();

protected:
    void changeEvent(QEvent *event) override;

private:
    void retranslateUi();
    void renameProfile();
    // MPROF05: rebuilds the 'New Window' switcher submenu from the
    // profile registry on every open.
    void populateNewWindowMenu();
    // PVAULT03: keeps 'Lock Profile' honest — enabled only when the
    // vault can actually seal this profile right now, with the
    // blocking reason on the tooltip.
    void updateLockAction();

    QMenu *m_menu;
    QLabel *m_headerAvatar;
    QLabel *m_headerName;
    QAction *m_renameAction;
    QMenu *m_newWindowMenu;
    QAction *m_managerAction;
    QAction *m_lockAction;
};

#endif // PROFILEBUTTON_H
