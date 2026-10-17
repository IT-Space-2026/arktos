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

#include "updatedialog.h"

#include "browserapplication.h"
#include "downloadmanager.h"

#include <qboxlayout.h>
#include <qcombobox.h>
#include <qcoreapplication.h>
#include <qdesktopservices.h>
#include <qdialogbuttonbox.h>
#include <qlabel.h>
#include <qpushbutton.h>

UpdateDialog::UpdateDialog(QWidget *parent)
    : QDialog(parent)
    , m_checker(new UpdateChecker(this))
    , m_checkStarted(false)
{
    setWindowTitle(tr("Check for Updates"));
    setModal(false);

    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *currentLabel = new QLabel(
        tr("Current version: %1")
            .arg(QCoreApplication::applicationVersion()), this);
    layout->addWidget(currentLabel);

    m_statusLabel = new QLabel(tr("Checking for updates…"), this);
    m_statusLabel->setObjectName(QLatin1String("updateStatusLabel"));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_detailLabel = new QLabel(this);
    m_detailLabel->setObjectName(QLatin1String("updateDetailLabel"));
    m_detailLabel->setWordWrap(true);
    // AUDIT04: release titles are server input — render them as
    // text, never as markup.
    m_detailLabel->setTextFormat(Qt::PlainText);
    m_detailLabel->setVisible(false);
    layout->addWidget(m_detailLabel);

    // Only surfaces when distro detection could not pick one package
    // format — the user chooses explicitly instead of a silent guess.
    m_assetCombo = new QComboBox(this);
    m_assetCombo->setObjectName(QLatin1String("updateAssetCombo"));
    m_assetCombo->setVisible(false);
    layout->addWidget(m_assetCombo);

    m_releasesLink = new QLabel(this);
    m_releasesLink->setObjectName(QLatin1String("updateReleasesLink"));
    m_releasesLink->setTextFormat(Qt::RichText);
    m_releasesLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_releasesLink->setText(QString::fromLatin1(
        "<a href=\"%1\">%2</a>")
        .arg(QString::fromUtf8(
            UpdateChecker::releasesPageUrl().toEncoded()),
             tr("View all releases")));
    connect(m_releasesLink, &QLabel::linkActivated,
            this, [this](const QString &) { viewAllReleases(); });
    layout->addWidget(m_releasesLink);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(this);
    m_downloadButton = buttonBox->addButton(
        tr("Download Latest Version"), QDialogButtonBox::ActionRole);
    m_downloadButton->setObjectName(
        QLatin1String("updateDownloadButton"));
    m_downloadButton->setEnabled(false);
    connect(m_downloadButton, &QPushButton::clicked,
            this, &UpdateDialog::downloadLatest);
    buttonBox->addButton(QDialogButtonBox::Close);
    connect(buttonBox, &QDialogButtonBox::rejected,
            this, &UpdateDialog::reject);
    layout->addWidget(buttonBox);

    connect(m_checker, &UpdateChecker::finished,
            this, &UpdateDialog::checkFinished);
}

void UpdateDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!m_checkStarted) {
        m_checkStarted = true;
        check();
    }
}

void UpdateDialog::check()
{
    m_statusLabel->setText(tr("Checking for updates…"));
    m_detailLabel->setVisible(false);
    m_assetCombo->setVisible(false);
    m_downloadButton->setEnabled(false);
    m_checker->check();
}

QString UpdateDialog::statusText() const
{
    return m_statusLabel->text();
}

void UpdateDialog::checkFinished(const UpdateChecker::Result &result)
{
    m_pick = UpdateChecker::platformAssets(result.assets);

    switch (result.status) {
    case UpdateChecker::Status::UpToDate:
        m_statusLabel->setText(tr("You're up to date."));
        m_downloadButton->setEnabled(false);
        break;
    case UpdateChecker::Status::UpdateAvailable: {
        m_statusLabel->setText(tr("Version %1 is available.")
            .arg(result.latestVersion));
        if (!result.releaseTitle.isEmpty()
                && result.releaseTitle != result.latestVersion)
            m_detailLabel->setText(result.releaseTitle);
        m_detailLabel->setVisible(!m_detailLabel->text().isEmpty());
        if (m_pick.candidates.isEmpty()) {
            // No package for this OS in the release — the button
            // degrades to opening the release page rather than
            // downloading a foreign-OS file.
            m_downloadButton->setText(tr("Open Release Page"));
            m_downloadButton->setToolTip(
                tr("This release has no package for your operating "
                   "system — open the release page instead."));
        } else {
            m_downloadButton->setText(tr("Download Latest Version"));
            // Tooltips auto-detect rich text — the asset name is
            // server input, so show it escaped.
            m_downloadButton->setToolTip(
                m_pick.candidates.first().name.toHtmlEscaped());
            if (m_pick.ambiguous) {
                m_assetCombo->clear();
                for (const UpdateChecker::ReleaseAsset &asset
                        : m_pick.candidates)
                    m_assetCombo->addItem(asset.name);
                m_assetCombo->setVisible(true);
            }
        }
        m_downloadButton->setEnabled(true);
        break;
    }
    case UpdateChecker::Status::NoReleases:
        m_statusLabel->setText(tr("No releases published yet."));
        m_downloadButton->setEnabled(false);
        break;
    case UpdateChecker::Status::Error:
        m_statusLabel->setText(tr("Couldn't check for updates."));
        if (!result.errorString.isEmpty()) {
            m_detailLabel->setText(result.errorString);
            m_detailLabel->setVisible(true);
        }
        m_downloadButton->setEnabled(false);
        break;
    default:
        break;
    }
}

void UpdateDialog::downloadLatest()
{
    // AUDIT04: refusing the check is not enough — in a tor process a
    // github.com asset fetch (or the system-handler fallback below)
    // would ride clearnet, so the sinks refuse on their own.
    if (BrowserApplication::isTorMode()) {
        m_statusLabel->setText(tr("Update downloads are disabled in "
                                  "Tor mode."));
        return;
    }
    if (m_pick.candidates.isEmpty()) {
        viewAllReleases();
        return;
    }
    const int row = m_assetCombo->isVisible()
        ? m_assetCombo->currentIndex() : 0;
    const UpdateChecker::ReleaseAsset asset =
        m_pick.candidates.at(qBound(0, row,
                                    m_pick.candidates.size() - 1));
    // A hidden retry page carries the download — WebEngine only
    // starts downloads from a page — and the normal DownloadManager
    // UI picks up progress/completion from there.
    DownloadManager *manager = BrowserApplication::downloadManager();
    manager->download(manager->retryPage(false),
                      asset.downloadUrl, true);
    accept();
}

void UpdateDialog::viewAllReleases()
{
    if (BrowserApplication::isTorMode()) {
        m_statusLabel->setText(tr("Update checks are disabled in Tor "
                                  "mode."));
        return;
    }
    QDesktopServices::openUrl(UpdateChecker::releasesPageUrl());
}
