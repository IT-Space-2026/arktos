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

#ifndef PROFILEAVATARACTIONS_H
#define PROFILEAVATARACTIONS_H

#include <qobject.h>

class QAction;

// PROF02 test seam for the profile menu: two ready-made QActions that
// PROF04 drops into PROF01's menu (addAction(changePictureAction()) /
// addAction(removePictureAction())).  'Remove Picture' is enabled only
// while a custom avatar exists and tracks avatarChanged() live.
class ProfileAvatarActions : public QObject
{
    Q_OBJECT

public:
    explicit ProfileAvatarActions(QObject *parent = nullptr);

    QAction *changePictureAction() const;
    QAction *removePictureAction() const;

private:
    QAction *m_changePicture;
    QAction *m_removePicture;
};

#endif // PROFILEAVATARACTIONS_H
