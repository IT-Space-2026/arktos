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

#ifndef PROFILEMANAGERDIALOG_H
#define PROFILEMANAGERDIALOG_H

#include <qdialog.h>
#include <qpointer.h>
#include <qstring.h>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class ProfileStore;
class ProfileVault;

// MPROF05: the dedicated Profile Manager — Firefox about:profiles
// parity.  A profile list (avatar + name + this-window/in-use marker
// + open-window count + data-dir size) with New / Rename / picture /
// Delete / Open Window actions.  Persistence goes through
// ProfileStore — the lane-local registry that already speaks
// MPROF01's profiles.json + profiles/<id>/ layout, so what the dialog
// writes survives the real-registry merge untouched.
//
// Window binding is live (MPROF03): every BrowserMainWindow carries a
// bound persona, so "Open Window" is enabled for any registered
// profile and opens a real window on it.
class ProfileManagerDialog : public QDialog
{
    Q_OBJECT

public:
    // `store` is the registry the dialog edits; `windowProfileId` is
    // the calling window's bound profile (its BrowserMainWindow::
    // profileId()).  `vault` is the PVAULT engine — nullptr takes the
    // singleton.
    ProfileManagerDialog(ProfileStore *store,
                         const QString &windowProfileId,
                         QWidget *parent = nullptr,
                         ProfileVault *vault = nullptr);

    // Single shared manager — a second request raises the open one
    // instead of stacking dialogs.  `profileId` defaults to the
    // active (default) profile on this lane.
    static void showManager(QWidget *parent,
                            const QString &profileId = QString());
    // Test seam: the live singleton, null when none is open.
    static ProfileManagerDialog *instance();

    // Re-reads the registry into the list (rows rebuilt, selection
    // kept when the id survives).
    void reloadProfiles();

    // Public for the autotest legs (same convention as SettingsDialog
    // and PROF03's ProfileWipeConfirmDialog).
    QTreeWidget *profileTree;
    QPushButton *newButton;
    QPushButton *renameButton;
    QPushButton *pictureButton;
    QPushButton *removePictureButton;
    QPushButton *passwordButton;
    QPushButton *vaultButton;
    QPushButton *deleteButton;
    QPushButton *openWindowButton;
    QPushButton *closeButton;

    QString selectedProfileId() const;
    int openWindowCount(const QString &id) const;

private slots:
    void newProfile();
    void renameSelected();
    void choosePicture();
    void removePicture();
    void deleteSelected();
    void openWindow();
    void editPassword();
    void toggleVaultLock();

private:
    void updateButtons();

    ProfileStore *m_store;
    ProfileVault *m_vault;
    QString m_windowProfileId;

    static QPointer<ProfileManagerDialog> s_instance;
};

#endif // PROFILEMANAGERDIALOG_H
