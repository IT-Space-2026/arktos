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
#ifndef UPDATEDIALOG_H
#define UPDATEDIALOG_H

#include "updatechecker.h"

#include <qdialog.h>

class QComboBox;
class QLabel;
class QPushButton;

// UPD01: the "Check for Updates" popup.  Shows the running version,
// runs the GitHub releases lookup on show, and reports one honest
// outcome line — up to date / newer version / couldn't check / no
// releases published.  The download button picks the host OS's
// package from the release assets (UpdateChecker::platformAssets) and
// hands the url to the normal DownloadManager; with no matching asset
// it falls back to opening the release page instead of downloading a
// foreign package.
class UpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UpdateDialog(QWidget *parent = nullptr);

    // Starts (or re-runs) the lookup.  Called once automatically the
    // first time the dialog is shown.
    void check();
    // Test seam — the checker must see the loopback api url before
    // check() runs.
    UpdateChecker *checker() const { return m_checker; }
    QString statusText() const;

    void showEvent(QShowEvent *event) override;

private slots:
    void checkFinished(const UpdateChecker::Result &result);
    void downloadLatest();
    void viewAllReleases();

private:
    UpdateChecker *m_checker;
    QLabel *m_statusLabel;
    QLabel *m_detailLabel;
    QLabel *m_releasesLink;
    QComboBox *m_assetCombo;
    QPushButton *m_downloadButton;
    UpdateChecker::PlatformPick m_pick;
    bool m_checkStarted;
};

#endif // UPDATEDIALOG_H
