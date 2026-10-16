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

#include "profileavataractions.h"

#include "profileavatardialog.h"
#include "profileidentity.h"

#include <qaction.h>
#include <qapplication.h>
#include <qwidget.h>

ProfileAvatarActions::ProfileAvatarActions(QObject *parent)
    : QObject(parent)
{
    ProfileIdentity *identity = ProfileIdentity::instance();

    m_changePicture = new QAction(tr("Change Picture..."), this);
    connect(m_changePicture, &QAction::triggered, this, [this]() {
        QWidget *parentWidget = qobject_cast<QWidget*>(this->parent());
        if (!parentWidget)
            parentWidget = QApplication::activeWindow();
        ProfileAvatarDialog::changePicture(parentWidget);
    });

    m_removePicture = new QAction(tr("Remove Picture"), this);
    m_removePicture->setEnabled(identity->hasCustomAvatar());
    connect(m_removePicture, &QAction::triggered, identity,
            &ProfileIdentity::removeAvatar);
    connect(identity, &ProfileIdentity::avatarChanged, this,
            [this, identity]() {
                m_removePicture->setEnabled(identity->hasCustomAvatar());
            });
}

QAction *ProfileAvatarActions::changePictureAction() const
{
    return m_changePicture;
}

QAction *ProfileAvatarActions::removePictureAction() const
{
    return m_removePicture;
}
