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

#ifndef EXTENSIONREVIEWDIALOG_H
#define EXTENSIONREVIEWDIALOG_H

#include "extensionmanager.h"

#include <qdialog.h>
#include <qstring.h>

class QDialogButtonBox;
class QLabel;
class QListWidget;
class QPushButton;

// EXT02: the consent gate every load/install funnels through — shows
// the manifest's name/version/source, its declared permissions
// humanized into Chrome-store-style risk text, host permissions, and
// the support caveats Qt WebEngine has (unsupported chrome.* APIs,
// partially-plumbed APIs, toolbar actions with no host UI).  Nothing
// reaches QWebEngineExtensionManager until the user approves here.
class ExtensionReviewDialog : public QDialog
{
    Q_OBJECT

public:
    enum Mode {
        Load,    // unpacked folder, this session only
        Install  // folder or .zip, persisted under the profile
    };

    ExtensionReviewDialog(const ExtensionManager::Manifest &manifest,
                          const QString &sourcePath,
                          Mode mode,
                          QWidget *parent = nullptr);

    // Runs the dialog modally; true only when the user pressed the
    // approve button.  The approve button is disabled for manifests
    // Chromium will reject outright (anything but manifest_version 3).
    static bool review(const ExtensionManager::Manifest &manifest,
                       const QString &sourcePath,
                       Mode mode,
                       QWidget *parent = nullptr);

    // Chrome-store-style description of one chrome.* permission name;
    // unknown names get a generic "uses the X API" line.
    static QString describePermission(const QString &permission);
    // Same for host_permissions entries ("*://*/*" -> all websites).
    static QString describeHostPermission(const QString &pattern);

private:
    QLabel *m_nameLabel;
    QLabel *m_sourceLabel;
    QLabel *m_permissionsHint;
    QListWidget *m_permissionsList;
    QListWidget *m_warningsList;
    QDialogButtonBox *m_buttonBox;
    QPushButton *m_approveButton;
};

#endif // EXTENSIONREVIEWDIALOG_H
