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

#ifndef PROFILEMENUACTIONS_H
#define PROFILEMENUACTIONS_H

#include <qdialog.h>
#include <qobject.h>
#include <qstring.h>

class QAction;
class QLineEdit;
class QPushButton;
class QWidget;

// PROF03: backend for the nav-bar profile menu (the button/menu
// itself is PROF01's UI lane).  Ships standalone — PROF04 appends
// hygieneActions() + deleteProfileAction() into the menu's
// hygiene/danger blocks.
//
// The hygiene actions are single-purpose wipes: one honest confirm
// ('Clear browsing history for all time? This cannot be undone.'),
// then the matching single-category ClearPrivateData plan is
// planned + executed, and the parent window's status bar confirms.
// 'Clear Private Data…' opens the existing full dialog.
//
// 'Delete Profile' is the dangerous row (rendered red): a confirm
// dialog itemizing exactly what dies, then a second type-to-confirm
// gate — no single click can wipe a profile.  The wipe itself never
// runs under a live browser (SEC12/HARD01: deleting Chromium's
// storage trees in-session wedges its storage services).  Instead a
// marker file is dropped into the data dir, the app quits after the
// autosave quiesce, and a respawned process — like any later start —
// wipes the marker-gated stores in main() before any profile,
// settings read or WebContents exists, then boots fresh.

// Second gate of the wipe flow: the button stays disabled until the
// edit holds the exact confirm phrase.
class ProfileWipeConfirmDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProfileWipeConfirmDialog(QWidget *parent = nullptr);

    static QString confirmPhrase();

    // Public for the autotest legs (same convention as SettingsDialog).
    QLineEdit *phraseEdit;
    QPushButton *wipeButton;
};

class ProfileMenuActions : public QObject
{
    Q_OBJECT

public:
    // `parent` widgets the confirms and provides the status bar the
    // 'cleared' notes land on.
    explicit ProfileMenuActions(QWidget *parent = nullptr);

    QAction *clearHistoryAction();
    QAction *clearCookiesAction();
    QAction *clearCacheAction();
    QAction *clearPrivateDataAction();
    // The hygiene block, in menu order — PROF04's insertion point.
    QList<QAction *> hygieneActions();
    // The danger row — a QWidgetAction so the menu paints it red.
    QAction *deleteProfileAction();

    // The hand-off to the next boot: the marker carries the pid of the
    // process that scheduled the wipe so the starter can wait out its
    // teardown before deleting stores its exit paths might still write.
    static QString wipeMarkerPath();
    static bool wipePending();
    static bool scheduleProfileWipe();

    // Boot side — called from main() before the application object
    // (and therefore before any settings read, profile bring-up or
    // WebContents): with the marker present, the wiping process is
    // waited out, then the data tree, the cache tree, the settings
    // file and the marker itself are removed and the boot continues
    // into a fresh profile.  Downloads (user files), packages and the
    // system tor binary are outside those roots and never touched.
    static bool applyPendingProfileWipe();

    // The whole 'Delete Profile' flow: itemize confirm → type-to-
    // confirm → marker → detached respawn → autosave quiesce + exit.
    // Returns false when cancelled or when the marker cannot be
    // written.  Under a real run it never returns on success — the
    // ARORA_WIPE_NO_QUIT=1 test seam stops it after the marker write.
    static bool requestProfileWipe(QWidget *parent);

    // Blocks until the pid exits or the timeout elapses — used at
    // boot so the respawned process waits out the exiting one.
    static void waitForProcessExit(qint64 pid, int timeoutMs);

private:
    void runClear(quint32 categories, const QString &confirmText,
                  const QString &doneMessage);

    QWidget *m_parent;
    QAction *m_clearHistoryAction;
    QAction *m_clearCookiesAction;
    QAction *m_clearCacheAction;
    QAction *m_clearPrivateDataAction;
    QAction *m_deleteProfileAction;
};

#endif // PROFILEMENUACTIONS_H
