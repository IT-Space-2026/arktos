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

#include "extensionreviewdialog.h"

#include <qdialogbuttonbox.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qlabel.h>
#include <qlistwidget.h>
#include <qpushbutton.h>
#include <qboxlayout.h>

// Everything fed from manifest.json is attacker-influenced text —
// every label that shows it is forced Qt::PlainText so markup-looking
// names render literally (SEC10 rule).
static QLabel *plainLabel(const QString &text, QWidget *parent)
{
    QLabel *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

ExtensionReviewDialog::ExtensionReviewDialog(
        const ExtensionManager::Manifest &manifest,
        const QString &sourcePath,
        Mode mode,
        QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(mode == Install ? tr("Install Extension")
                                   : tr("Load Extension"));

    QVBoxLayout *layout = new QVBoxLayout(this);

    QString heading = manifest.name;
    if (heading.isEmpty())
        heading = QFileInfo(sourcePath).fileName();
    if (!manifest.version.isEmpty())
        heading += QLatin1String("  ") + manifest.version;
    m_nameLabel = plainLabel(heading, this);
    m_nameLabel->setObjectName(QLatin1String("nameLabel"));
    QFont headingFont = m_nameLabel->font();
    headingFont.setBold(true);
    m_nameLabel->setFont(headingFont);
    layout->addWidget(m_nameLabel);

    QString sourceText = sourcePath;
    if (QFileInfo(sourcePath).isFile()
            && sourcePath.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive))
        sourceText += tr("  (package)");
    m_sourceLabel = plainLabel(tr("Source: %1").arg(sourceText), this);
    m_sourceLabel->setObjectName(QLatin1String("sourceLabel"));
    layout->addWidget(m_sourceLabel);

    if (!manifest.description.isEmpty())
        layout->addWidget(plainLabel(manifest.description, this));

    m_permissionsHint = plainLabel(
        mode == Install
            ? tr("It requests access to:")
            : tr("For this session it requests access to:"), this);
    m_permissionsHint->setObjectName(QLatin1String("permissionsHint"));
    layout->addWidget(m_permissionsHint);

    m_permissionsList = new QListWidget(this);
    m_permissionsList->setObjectName(QLatin1String("permissionsList"));
    m_permissionsList->setAccessibleName(tr("Requested permissions"));
    m_permissionsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_permissionsList->setFocusPolicy(Qt::NoFocus);
    m_permissionsList->setMaximumHeight(110);
    for (const QString &permission : manifest.permissions) {
        QListWidgetItem *item = new QListWidgetItem(
            describePermission(permission), m_permissionsList);
        item->setToolTip(permission);
    }
    for (const QString &pattern : manifest.hostPermissions) {
        QListWidgetItem *item = new QListWidgetItem(
            describeHostPermission(pattern), m_permissionsList);
        item->setToolTip(pattern);
    }
    if (m_permissionsList->count() == 0) {
        QListWidgetItem *item = new QListWidgetItem(
            tr("This extension declares no permissions."), m_permissionsList);
        item->setFlags(Qt::NoItemFlags);
    }
    layout->addWidget(m_permissionsList);

    m_warningsList = new QListWidget(this);
    m_warningsList->setObjectName(QLatin1String("warningsList"));
    m_warningsList->setAccessibleName(tr("Warnings"));
    m_warningsList->setSelectionMode(QAbstractItemView::NoSelection);
    m_warningsList->setFocusPolicy(Qt::NoFocus);
    m_warningsList->setMaximumHeight(110);

    // Anything but manifest_version 3 is rejected by Chromium, so the
    // approve button stays disabled — the dialog becomes read-only.
    const bool fatal = manifest.valid && manifest.manifestVersion != 3;
    if (!manifest.error.isEmpty())
        new QListWidgetItem(manifest.error, m_warningsList);
    if (fatal)
        new QListWidgetItem(
            tr("Qt WebEngine will reject this package — installation "
               "cannot proceed."),
            m_warningsList);
    if (!manifest.unsupported.isEmpty())
        new QListWidgetItem(
            tr("Declares APIs unavailable in Qt WebEngine — these "
               "features will not work: %1")
                .arg(manifest.unsupported.join(QLatin1String(", "))),
            m_warningsList);
    if (!manifest.unverified.isEmpty())
        new QListWidgetItem(
            tr("Declares APIs with only partial Qt WebEngine support: %1")
                .arg(manifest.unverified.join(QLatin1String(", "))),
            m_warningsList);
    // Qt WebEngine surfaces actionPopupUrl but has no toolbar to host
    // a browser-action button or its popup in — say so up front.
    if (manifest.hasAction)
        new QListWidgetItem(
            tr("Declares a toolbar action — Qt WebEngine has no "
               "extension toolbar, so its button and popup will not "
               "appear."),
            m_warningsList);
    if (m_warningsList->count())
        layout->addWidget(m_warningsList);

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_approveButton = m_buttonBox->addButton(
        mode == Install ? tr("Install") : tr("Load"),
        QDialogButtonBox::AcceptRole);
    m_approveButton->setObjectName(QLatin1String("approveButton"));
    m_approveButton->setEnabled(!fatal);
    // Consent must be deliberate — Cancel is the default button.
    m_buttonBox->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(m_buttonBox, &QDialogButtonBox::accepted,
            this, &ExtensionReviewDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected,
            this, &ExtensionReviewDialog::reject);
    layout->addWidget(m_buttonBox);
}

bool ExtensionReviewDialog::review(const ExtensionManager::Manifest &manifest,
                                   const QString &sourcePath,
                                   Mode mode,
                                   QWidget *parent)
{
    ExtensionReviewDialog dialog(manifest, sourcePath, mode, parent);
    return dialog.exec() == QDialog::Accepted;
}

QString ExtensionReviewDialog::describePermission(const QString &permission)
{
    // Chrome-web-store-style wording for the chrome.* permissions Qt
    // WebEngine registers; kept in one table so new names only need a
    // single entry.
    static const struct { const char *name; const char *text; } table[] = {
        { "activeTab", QT_TR_NOOP("Access the current tab when you invoke it") },
        { "alarms", QT_TR_NOOP("Schedule periodic tasks") },
        { "background", QT_TR_NOOP("Run in the background") },
        { "bookmarks", QT_TR_NOOP("Read and change your bookmarks") },
        { "browsingData", QT_TR_NOOP("Clear your browsing data") },
        { "clipboardRead", QT_TR_NOOP("Read data you copy and paste") },
        { "clipboardWrite", QT_TR_NOOP("Modify data you copy and paste") },
        { "contentSettings", QT_TR_NOOP("Change settings that control site content") },
        { "contextMenus", QT_TR_NOOP("Add items to browser context menus") },
        { "cookies", QT_TR_NOOP("Read and modify cookies") },
        { "debugger", QT_TR_NOOP("Control and inspect web pages through the debugger") },
        { "declarativeNetRequest", QT_TR_NOOP("Block or modify network requests") },
        { "declarativeNetRequestFeedback", QT_TR_NOOP("Observe network request blocking") },
        { "declarativeNetRequestWithHostAccess", QT_TR_NOOP("Block or modify network requests") },
        { "desktopCapture", QT_TR_NOOP("Capture content of your screen") },
        { "downloads", QT_TR_NOOP("Manage your downloads") },
        { "fontSettings", QT_TR_NOOP("Change font settings") },
        { "geolocation", QT_TR_NOOP("Detect your physical location") },
        { "history", QT_TR_NOOP("Read and change your browsing history") },
        { "identity", QT_TR_NOOP("Access your account identity") },
        { "idle", QT_TR_NOOP("Detect when your computer is idle") },
        { "management", QT_TR_NOOP("Manage your extensions") },
        { "menus", QT_TR_NOOP("Add items to browser menus") },
        { "nativeMessaging", QT_TR_NOOP("Communicate with cooperating native applications") },
        { "notifications", QT_TR_NOOP("Display notifications") },
        { "offscreen", QT_TR_NOOP("Run invisible offscreen pages") },
        { "pageCapture", QT_TR_NOOP("Save pages as local files") },
        { "power", QT_TR_NOOP("Override power management settings") },
        { "privacy", QT_TR_NOOP("Change your privacy-related settings") },
        { "proxy", QT_TR_NOOP("Read and change your proxy settings") },
        { "readingList", QT_TR_NOOP("Read and change your reading list") },
        { "scripting", QT_TR_NOOP("Inject scripts into web pages") },
        { "sessions", QT_TR_NOOP("Read and restore recently closed tabs and windows") },
        { "sidePanel", QT_TR_NOOP("Show content in a side panel") },
        { "storage", QT_TR_NOOP("Store data on your device") },
        { "system.cpu", QT_TR_NOOP("Read your computer's CPU information") },
        { "system.display", QT_TR_NOOP("Read your display configuration") },
        { "system.memory", QT_TR_NOOP("Read your computer's memory information") },
        { "system.storage", QT_TR_NOOP("Read your storage device information") },
        { "tabCapture", QT_TR_NOOP("Capture the visible content of tabs") },
        { "tabGroups", QT_TR_NOOP("Manage tab groups") },
        { "tabs", QT_TR_NOOP("Read your browsing history and open tabs") },
        { "topSites", QT_TR_NOOP("Read a list of your most visited websites") },
        { "tts", QT_TR_NOOP("Speak text aloud") },
        { "ttsEngine", QT_TR_NOOP("Provide text-to-speech voices") },
        { "unlimitedStorage", QT_TR_NOOP("Store unlimited data on your device") },
        { "webNavigation", QT_TR_NOOP("Observe your navigation between pages") },
        { "webRequest", QT_TR_NOOP("Observe your network traffic") },
        { "webRequestBlocking", QT_TR_NOOP("Observe and modify your network traffic") },
    };
    for (const auto &entry : table) {
        if (permission == QLatin1String(entry.name))
            return tr(entry.text);
    }
    if (permission == QLatin1String("<all_urls>"))
        return describeHostPermission(permission);
    return tr("Use the '%1' browser API").arg(permission);
}

QString ExtensionReviewDialog::describeHostPermission(const QString &pattern)
{
    if (pattern == QLatin1String("<all_urls>")
            || pattern == QLatin1String("*://*/*"))
        return tr("Read and change all your data on all websites");

    // "scheme://host/path" -> show the hosts the extension can reach.
    QString host = pattern;
    const int schemeSep = host.indexOf(QLatin1String("://"));
    if (schemeSep >= 0)
        host = host.mid(schemeSep + 3);
    const int slash = host.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        host.truncate(slash);
    if (host.isEmpty())
        host = pattern;
    return tr("Read and change your data on %1").arg(host);
}
