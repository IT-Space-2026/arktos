/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2008 Jason A. Donenfeld <Jason@zx2c4.com>
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

/****************************************************************************
**
** Copyright (C) 2008-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/.
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include "downloadmanager.h"

#include "autosaver.h"
#include "browserprofile.h"

#include <math.h>

#include <qdesktopservices.h>
#include <qfiledialog.h>
#include <qfileiconprovider.h>
#include <qheaderview.h>
#include <qmessagebox.h>
#include <qmetaobject.h>
#include <qmimedata.h>
#include <qprocess.h>
#include <qregularexpression.h>
#include <qset.h>
#include <qsettings.h>
#include <qstandardpaths.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <qdebug.h>

//#define DOWNLOADMANAGER_DEBUG

/*!
    DownloadItem is a widget that is displayed in the download manager list.

    It wraps a QWebEngineDownloadRequest: Chromium streams the data to
    disk itself once the item has picked a file name and accept()ed the
    request, so the item only tracks state and updates the progress bar,
    info label and buttons.
 */
DownloadItem::DownloadItem(QWebEngineDownloadRequest *download, bool requestFileName, QWidget *parent)
    : QWidget(parent)
    , m_download(download)
    , m_requestFileName(requestFileName)
    , m_bytesReceived(0)
    , m_finishedDownloading(false)
    , m_gettingFileName(false)
    , m_canceledByUser(false)
    , m_awaitingRetry(false)
    , m_offTheRecord(false)
{
    setupUi(this);
    // Server-supplied file names may contain markup-looking text;
    // the labels must always render it literally.
    fileNameLabel->setTextFormat(Qt::PlainText);
    downloadInfoLabel->setTextFormat(Qt::PlainText);
    // UIP01: secondary text — a hardcoded darkGray is unreadable under
    // a dark palette; PlaceholderText adapts to the active scheme.
    QPalette p = downloadInfoLabel->palette();
    p.setColor(QPalette::Text, p.color(QPalette::PlaceholderText));
    downloadInfoLabel->setPalette(p);
    progressBar->setMaximum(0);
    // UIP02: this row packs three compact buttons — exempt it from the
    // dialog-wide minimum button width/height polish.
    setProperty("aroraNoButtonPolish", true);
    tryAgainButton->hide();
    connect(stopButton, &QPushButton::clicked, this, &DownloadItem::stop);
    connect(openButton, &QPushButton::clicked, this, &DownloadItem::open);
    connect(tryAgainButton, &QPushButton::clicked, this, &DownloadItem::tryAgain);

    if (!requestFileName) {
        QSettings settings;
        settings.beginGroup(QLatin1String("downloadmanager"));
        m_requestFileName = settings.value(QLatin1String("alwaysPromptForFileName"), false).toBool();
    }

    init();
}

void DownloadItem::init()
{
    if (!m_download)
        return;

    m_finishedDownloading = false;
    m_bytesReceived = 0;
    m_offTheRecord = m_download->page()
        && m_download->page()->profile()->isOffTheRecord();

    openButton->setEnabled(false);
    stopButton->setEnabled(true);
    stopButton->setVisible(true);
    tryAgainButton->setEnabled(false);
    tryAgainButton->setVisible(false);

    // attach to the request
    m_url = m_download->url();
    connect(m_download, &QWebEngineDownloadRequest::stateChanged,
            this, &DownloadItem::downloadStateChanged);
    connect(m_download, &QWebEngineDownloadRequest::receivedBytesChanged,
            this, &DownloadItem::downloadProgressUpdate);
    connect(m_download, &QWebEngineDownloadRequest::totalBytesChanged,
            this, &DownloadItem::downloadProgressUpdate);

    // reset info
    downloadInfoLabel->clear();
    progressBar->setValue(0);
    progressBar->setVisible(true);
    getFileName();

    // start timer for the download estimation
    m_downloadTime.start();
    m_lastProgressTime.start();

    // catch up on a terminal state that arrived before the signals
    // were connected (instant failure of a tiny request, for example)
    if (m_download && m_download->isFinished())
        downloadStateChanged(m_download->state());
}

void DownloadItem::attach(QWebEngineDownloadRequest *download)
{
    // A retry restarts the transfer from scratch — drop the previous
    // attempt's partial file so it neither litters the directory nor
    // pushes the retry onto a "-N" dedup name.  Only a live request's
    // file is removed: a restored item may point at a complete file.
    if (m_download && !downloadedSuccessfully()
        && m_download->state() != QWebEngineDownloadRequest::DownloadInProgress)
        removePartialFile();
    m_download = download;
    init();
    emit statusChanged();
}

void DownloadItem::getFileName()
{
    if (m_gettingFileName || !m_download)
        return;

    DownloadManager *manager = qobject_cast<DownloadManager*>(parent());
    QString downloadDirectory = manager->downloadDirectory();

    QString defaultFileName = saveFileName(downloadDirectory);
    QString fileName = defaultFileName;
    if (m_requestFileName) {
        m_gettingFileName = true;
        fileName = QFileDialog::getSaveFileName(this, tr("Save File"), defaultFileName);
        m_gettingFileName = false;
        if (fileName.isEmpty()) {
            progressBar->setVisible(false);
            m_canceledByUser = true;
            stop();
            fileNameLabel->setText(tr("Download canceled: %1").arg(QFileInfo(defaultFileName).fileName()));
            setAccessibleName(fileNameLabel->text());
            return;
        }
        QFileInfo fileInfo = QFileInfo(fileName);
        manager->setDownloadDirectory(fileInfo.absoluteDir().absolutePath());
    }
    m_outputFileName = fileName;

    // SEC01: the name may describe a file the OS will run or install
    // when opened — warn before any bytes or directories are created.
    // The prompt spins a nested event loop, so the request can die
    // underneath us; m_download is a QPointer.
    if (!m_download || !confirmSafeToSave(fileName)) {
        progressBar->setVisible(false);
        m_canceledByUser = true;
        stop();
        fileNameLabel->setText(tr("Download canceled: %1").arg(QFileInfo(fileName).fileName()));
        setAccessibleName(fileNameLabel->text());
        return;
    }

    // Check file path for saving.
    QDir saveDirPath = QFileInfo(m_outputFileName).dir();
    if (!saveDirPath.exists()) {
        if (!saveDirPath.mkpath(saveDirPath.absolutePath())) {
            progressBar->setVisible(false);
            stop();
            downloadInfoLabel->setText(tr("Download directory (%1) couldn't be created.").arg(saveDirPath.absolutePath()));
            return;
        }
    }

    // Chromium writes the file itself; hand it a directory and a bare
    // file name (never a path — the suggested name is untrusted).
    QFileInfo info(m_outputFileName);
    m_download->setDownloadDirectory(info.absolutePath());
    m_download->setDownloadFileName(info.fileName());
    m_download->accept();

    fileNameLabel->setText(info.fileName());
    setAccessibleName(info.fileName());
}

QString DownloadItem::sanitizeFileName(const QString &suggestedName)
{
    // The suggested name arrives from an untrusted server or URL —
    // keep only the last path component, checking both separators so
    // a Windows-style "..\name" cannot traverse either.
    QString name = suggestedName;
    const int slash = qMax(name.lastIndexOf(QLatin1Char('/')),
                           name.lastIndexOf(QLatin1Char('\\')));
    if (slash != -1)
        name = name.mid(slash + 1);

    // Control characters corrupt the label display and the filesystem.
    QString cleaned;
    cleaned.reserve(name.size());
    for (const QChar &c : name) {
        if (c.unicode() >= 0x20 && c.unicode() != 0x7f)
            cleaned += c;
    }
    name = cleaned.trimmed();
    while (name.endsWith(QLatin1Char('.')))
        name.chop(1);
    // "." and ".." resolve outside the download directory.
    if (name.isEmpty() || name == QLatin1String(".")
        || name == QLatin1String(".."))
        return QString();

    // NAME_MAX is 255 bytes on common filesystems; leave headroom for
    // the "-NN" dedup suffix while keeping the extension readable.
    const int maxLength = 200;
    if (name.size() > maxLength) {
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        const QString suffix = (dot > 0 && name.size() - dot <= 16)
            ? name.mid(dot) : QString();
        name = name.left(maxLength - suffix.size()) + suffix;
    }
    return name;
}

bool DownloadItem::isDangerousExtension(const QString &fileName)
{
    // Suffixes that run commands, install software, or auto-execute
    // when the file is opened — covering Linux, Windows and macOS
    // since the port targets all three.
    static const QSet<QString> dangerousSuffixes = {
        QStringLiteral("appimage"), QStringLiteral("apk"),
        QStringLiteral("bash"), QStringLiteral("bat"),
        QStringLiteral("cmd"), QStringLiteral("com"),
        QStringLiteral("csh"), QStringLiteral("deb"),
        QStringLiteral("desktop"), QStringLiteral("dll"),
        QStringLiteral("dmg"), QStringLiteral("exe"),
        QStringLiteral("hta"), QStringLiteral("jar"),
        QStringLiteral("js"), QStringLiteral("jse"),
        QStringLiteral("ksh"), QStringLiteral("lnk"),
        QStringLiteral("msc"), QStringLiteral("msi"),
        QStringLiteral("pif"), QStringLiteral("pkg"),
        QStringLiteral("ps1"), QStringLiteral("reg"),
        QStringLiteral("rpm"), QStringLiteral("run"),
        QStringLiteral("scr"), QStringLiteral("sh"),
        QStringLiteral("vb"), QStringLiteral("vbe"),
        QStringLiteral("vbs"), QStringLiteral("wsf"),
        QStringLiteral("wsh")
    };
    return dangerousSuffixes.contains(QFileInfo(fileName).suffix().toLower());
}

bool DownloadItem::isExecutableMimeType(const QString &mimeType)
{
    // Content types that describe a program or installer even when
    // the file name pretends to be something innocuous.  Generic
    // application/octet-stream is deliberately absent — it labels
    // ordinary downloads and would warn on everything.
    static const QSet<QString> executableMimes = {
        QStringLiteral("application/java-archive"),
        QStringLiteral("application/vnd.android.package-archive"),
        QStringLiteral("application/vnd.debian.binary-package"),
        QStringLiteral("application/vnd.microsoft.portable-executable"),
        QStringLiteral("application/x-apple-diskimage"),
        QStringLiteral("application/x-deb"),
        QStringLiteral("application/x-desktop"),
        QStringLiteral("application/x-executable"),
        QStringLiteral("application/x-msdownload"),
        QStringLiteral("application/x-msdos-program"),
        QStringLiteral("application/x-ms-dos-executable"),
        QStringLiteral("application/x-msi"),
        QStringLiteral("application/x-ms-installer"),
        QStringLiteral("application/x-ms-shortcut"),
        QStringLiteral("application/x-rpm"),
        QStringLiteral("application/x-shellscript")
    };
    const QString mime = mimeType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    return executableMimes.contains(mime);
}

bool DownloadItem::confirmSafeToSave(const QString &fileName)
{
    const QString name = QFileInfo(fileName).fileName();
    const bool dangerousName = isDangerousExtension(name);
    const QString mime = m_download ? m_download->mimeType() : QString();
    if (!dangerousName && !isExecutableMimeType(mime))
        return true;

    QString text;
    if (dangerousName) {
        text = tr("\"%1\" is a type of file that can harm your computer.\n"
                  "It may run commands or install software when opened.")
            .arg(name);
    } else {
        // Executable MIME behind an innocent name — a disguised program.
        text = tr("The server reports that \"%1\" is a \"%2\" file, "
                  "which does not match its file name.\n"
                  "It may be disguised software that can harm your computer.")
            .arg(name, mime.section(QLatin1Char(';'), 0, 0).trimmed());
    }
    const QString source = m_url.host().isEmpty() ? m_url.toString() : m_url.host();
    text += QLatin1Char('\n') + tr("Keep this file only if you trust %1.").arg(source);

    // The file name and MIME type are server-controlled; render the
    // prompt literally so markup-looking content cannot spoof chrome.
    QMessageBox box(QMessageBox::Warning, tr("Download Security Warning"),
                    text, QMessageBox::Save | QMessageBox::Discard, this);
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::Discard);
    return box.exec() == QMessageBox::Save;
}

void DownloadItem::removeExecutableBit(const QString &path)
{
    // Chromium writes downloads without the exec bit already; strip it
    // anyway so a downloaded script or .desktop file cannot execute
    // when the user opens it from a file manager.
    if (path.isEmpty())
        return;
    QFile file(path);
    const QFileDevice::Permissions execBits =
        QFileDevice::ExeOwner | QFileDevice::ExeUser
        | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    const QFileDevice::Permissions permissions = file.permissions();
    if (permissions & execBits)
        file.setPermissions(permissions & ~execBits);
}

void DownloadItem::removePartialFile()
{
    if (m_outputFileName.isEmpty())
        return;
    // Only ever remove a regular file Chromium left half-written —
    // never a directory, symlink, or anything a crafted name points at.
    const QString path = QFileInfo(m_outputFileName).absoluteFilePath();
    const QFileInfo info(path);
    if (info.isFile() && !info.isSymLink())
        QFile::remove(path);
    // QtWebEngine streams to "<name>.download" and renames on completion.
    QFile::remove(path + QLatin1String(".download"));
    QFile::remove(path + QLatin1String(".crdownload"));
}

QString DownloadItem::saveFileName(const QString &directory) const
{
    // Chromium already folds the Content-Disposition filename into
    // suggestedFileName; both it and the URL path are untrusted input,
    // so everything goes through sanitizeFileName (SEC01).
    QString name;
    if (m_download)
        name = sanitizeFileName(m_download->suggestedFileName());
    if (name.isEmpty())
        name = sanitizeFileName(m_url.path());
    if (name.isEmpty())
        name = QLatin1String("unnamed_download");

    QFileInfo info(name);
    QString baseName = info.completeBaseName();
    QString endName = info.suffix();

    if (baseName.isEmpty()) {
        baseName = QLatin1String("unnamed_download");

#ifdef DOWNLOADMANAGER_DEBUG
        qDebug() << "DownloadItem::" << __FUNCTION__ << "downloading unknown file:" << m_url;
#endif
    }

    if (!endName.isEmpty())
        endName = QLatin1Char('.') + endName;

    QString fileName = directory + baseName + endName;
    // A name is taken when the file exists on disk or when another
    // in-flight download has already claimed it (files land later, so
    // QFile::exists alone does not cover parallel downloads).
    const auto nameInUse = [this](const QString &path) {
        if (QFile::exists(path))
            return true;
        const DownloadManager *manager = qobject_cast<const DownloadManager*>(parent());
        if (!manager)
            return false;
        for (const DownloadItem *item : manager->m_downloads) {
            if (item != this && item->m_outputFileName == path)
                return true;
        }
        return false;
    };
    if (!m_requestFileName && nameInUse(fileName)) {
        // already exists, don't overwrite
        int i = 1;
        do {
            fileName = directory + baseName + QLatin1Char('-') + QString::number(i++) + endName;
        } while (nameInUse(fileName));
    }
    return fileName;
}

void DownloadItem::stop()
{
    setUpdatesEnabled(false);
    stopButton->setEnabled(false);
    stopButton->hide();
    tryAgainButton->setEnabled(true);
    tryAgainButton->show();
    setUpdatesEnabled(true);
    if (m_download) {
        // the DownloadCancelled state transition finishes the item
        m_download->cancel();
    } else {
        emit downloadFinished();
    }
}

void DownloadItem::open()
{
    QFileInfo info(m_outputFileName);
    QUrl url = QUrl::fromLocalFile(info.absoluteFilePath());
    QDesktopServices::openUrl(url);
}

void DownloadItem::tryAgain()
{
    if (!tryAgainButton->isEnabled())
        return;

    QWebEnginePage *page = m_download ? m_download->page() : nullptr;
    if (!page) {
        // The page that started the download is gone (or this item was
        // restored from disk); use a hidden page to re-issue it.
        DownloadManager *manager = qobject_cast<DownloadManager*>(parent());
        page = manager->retryPage(m_offTheRecord);
    }
    if (!page)
        return;

    tryAgainButton->setEnabled(false);
    // DownloadManager::handleDownloadRequested re-attaches the fresh
    // request for this url to this item.
    m_awaitingRetry = true;
    page->download(m_url);
}

void DownloadItem::downloadStateChanged(QWebEngineDownloadRequest::DownloadState state)
{
    switch (state) {
    case QWebEngineDownloadRequest::DownloadInProgress:
        m_downloadTime.start();
        m_lastProgressTime.start();
        break;
    case QWebEngineDownloadRequest::DownloadCompleted:
        finished();
        break;
    case QWebEngineDownloadRequest::DownloadCancelled:
    case QWebEngineDownloadRequest::DownloadInterrupted:
        if (m_finishedDownloading || !m_download)
            break;
        m_finishedDownloading = true;
        if (state == QWebEngineDownloadRequest::DownloadInterrupted) {
            downloadInfoLabel->setText(tr("Download interrupted: %1")
                                       .arg(m_download->interruptReasonString()));
        }
        progressBar->setVisible(false);
        stopButton->setEnabled(false);
        stopButton->setVisible(false);
        tryAgainButton->setEnabled(true);
        tryAgainButton->setVisible(true);
        emit statusChanged();
        emit downloadFinished();
        break;
    case QWebEngineDownloadRequest::DownloadRequested:
        break;
    }
}

void DownloadItem::downloadProgressUpdate()
{
    if (!m_download)
        return;
    if (m_lastProgressTime.isValid() && m_lastProgressTime.elapsed() < 200)
        return;

    m_lastProgressTime.start();

    m_bytesReceived = m_download->receivedBytes();
    qint64 bytesTotal = m_download->totalBytes();
    qint64 currentValue = 0;
    qint64 totalValue = 0;
    if (bytesTotal > 0) {
        currentValue = m_bytesReceived * 100 / bytesTotal;
        totalValue = 100;
    }
    progressBar->setValue(currentValue);
    progressBar->setMaximum(totalValue);

    emit progress(currentValue, totalValue);
    updateInfoLabel();
}

qint64 DownloadItem::bytesTotal() const
{
    return m_download ? m_download->totalBytes() : 0;
}

qint64 DownloadItem::bytesReceived() const
{
    return m_bytesReceived;
}

double DownloadItem::remainingTime() const
{
    if (!downloading())
        return -1.0;

    double timeRemaining = ((double)(bytesTotal() - bytesReceived())) / currentSpeed();

    // When downloading the eta should never be 0
    if (timeRemaining == 0)
        timeRemaining = 1;

    return timeRemaining;
}

double DownloadItem::currentSpeed() const
{
    if (!downloading())
        return -1.0;

    return m_bytesReceived * 1000.0 / qMax<qint64>(m_downloadTime.elapsed(), 1);
}

void DownloadItem::updateInfoLabel()
{
    if (!m_download)
        return;

    qint64 bytesTotal = m_download->totalBytes();
    bool running = !downloadedSuccessfully();

    // update info label
    double speed = currentSpeed();
    double timeRemaining = remainingTime();

    QString info;
    if (running) {
        QString remaining;

        if (bytesTotal > 0) {
            remaining = DownloadManager::timeString(timeRemaining);
        }

        info = QString(tr("%1 of %2 (%3/sec) - %4"))
            .arg(DownloadManager::dataString(m_bytesReceived))
            .arg(bytesTotal > 0 ? DownloadManager::dataString(bytesTotal) : tr("?"))
            .arg(DownloadManager::dataString((int)speed))
            .arg(remaining);
    } else {
        if (bytesTotal <= 0 || m_bytesReceived == bytesTotal)
            info = DownloadManager::dataString(m_bytesReceived);
        else
            info = tr("%1 of %2 - Download Complete")
                .arg(DownloadManager::dataString(m_bytesReceived))
                .arg(DownloadManager::dataString(bytesTotal));
    }
    downloadInfoLabel->setText(info);
}

bool DownloadItem::downloading() const
{
    return (m_download && m_download->state() == QWebEngineDownloadRequest::DownloadInProgress);
}

bool DownloadItem::downloadedSuccessfully() const
{
    if (m_download)
        return (m_download->state() == QWebEngineDownloadRequest::DownloadCompleted);
    return (stopButton->isHidden() && tryAgainButton->isHidden());
}

void DownloadItem::finished()
{
    if (m_finishedDownloading)
        return;
    m_finishedDownloading = true;
    if (m_download) {
        m_bytesReceived = m_download->receivedBytes();
        // A completed download should never arrive user-executable.
        removeExecutableBit(m_outputFileName);
    }
    progressBar->hide();
    stopButton->setEnabled(false);
    stopButton->hide();
    openButton->setEnabled(true);
    updateInfoLabel();
    emit statusChanged();
    emit downloadFinished();
}

/*!
    DownloadManager is a Dialog that contains a list of DownloadItems

    It is a basic download manager.  It only downloads the file, doesn't do BitTorrent,
    extract zipped files or anything fancy.
  */
DownloadManager *DownloadManager::instance()
{
    // Lazily created top-level dialog.  Not parented on qApp: a QWidget
    // cannot take a non-widget parent, and a static QPointer keeps the
    // lookup cheap without a leak-on-purpose flag.
    static QPointer<DownloadManager> manager;
    if (!manager)
        manager = new DownloadManager();
    return manager;
}

DownloadManager::DownloadManager(QWidget *parent)
    : QDialog(parent)
    , m_autoSaver(new AutoSaver(this))
    , m_model(new DownloadModel(this, this))
    , m_iconProvider()
    , m_retryPage(nullptr)
    , m_retryPageOtr(nullptr)
    , m_removePolicy(Never)
    , m_requestFileNameNext(false)
{
    setupUi(this);

    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    QString defaultLocation = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (defaultLocation.isEmpty())
        defaultLocation = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    setDownloadDirectory(settings.value(QLatin1String("downloadDirectory"), defaultLocation).toString());

    downloadsView->setShowGrid(false);
    downloadsView->verticalHeader()->hide();
    downloadsView->horizontalHeader()->hide();
    downloadsView->setAlternatingRowColors(true);
    downloadsView->horizontalHeader()->setStretchLastSection(true);
    downloadsView->setModel(m_model);
    connect(cleanupButton, &QPushButton::clicked, this, &DownloadManager::cleanup);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &DownloadManager::close);
    load();
}

DownloadManager::~DownloadManager()
{
    m_autoSaver->changeOccurred();
    m_autoSaver->saveIfNeccessary();
}

void DownloadManager::installOnProfile(QWebEngineProfile *profile)
{
    if (!profile)
        return;
    connect(profile, &QWebEngineProfile::downloadRequested,
            this, &DownloadManager::handleDownloadRequested,
            Qt::UniqueConnection);
}

QWebEnginePage *DownloadManager::retryPage(bool offTheRecord)
{
    QWebEnginePage *&page = offTheRecord ? m_retryPageOtr : m_retryPage;
    if (!page) {
        page = offTheRecord
            // A default-constructed page lives on its own off-the-record
            // profile.
            ? new QWebEnginePage(this)
            // QWebEngineProfile::defaultProfile() is itself off-the-record
            // in Qt6 — normal retries go through Arora's persistent
            // profile (MIG11).
            : new QWebEnginePage(BrowserProfile::normalProfile(), this);
        installOnProfile(page->profile());
    }
    return page;
}

int DownloadManager::activeDownloads() const
{
    int count = 0;
    for (int i = 0; i < m_downloads.count(); ++i) {
        if (m_downloads.at(i)->downloading())
            ++count;
    }
    return count;
}

bool DownloadManager::hasActiveDownloadForPage(QWebEnginePage *page) const
{
    if (!page)
        return false;
    for (DownloadItem *item : m_downloads) {
        if (item->downloading() && item->m_download
            && item->m_download->page() == page)
            return true;
    }
    return false;
}

bool DownloadManager::allowQuit()
{
    if (activeDownloads() >= 1) {
        int choice = QMessageBox::warning(this, QString(),
                                        tr("There are %1 downloads in progress\n"
                                           "Do you want to quit anyway?").arg(activeDownloads()),
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::No);
        if (choice == QMessageBox::No) {
            show();
            return false;
        }
    }
    return true;
}

bool DownloadManager::externalDownload(const QUrl &url)
{
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    if (!settings.value(QLatin1String("external"), false).toBool())
        return false;

    QString program = settings.value(QLatin1String("externalPath")).toString();
    if (program.isEmpty())
        return false;

    // The url leaves the process as a raw argv argument to the
    // configured handler — restrict it to real remote-download
    // schemes.  file: would leak local paths, data:/blob: carry page
    // content the handler cannot resolve anyway, and javascript: must
    // never reach an argv (SEC09).
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http")
        && scheme != QLatin1String("https")
        && scheme != QLatin1String("ftp")) {
        qWarning() << "DownloadManager: refusing to hand a" << scheme
                   << "url to the external download handler";
        return false;
    }

    // Split program at every space not inside double quotes
    static const QRegularExpression regex(QLatin1String("\"([^\"]+)\"|([^ ]+)"));
    QStringList args;
    QRegularExpressionMatchIterator it = regex.globalMatch(program);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        args << match.captured(1) + match.captured(2);
    }
    if (args.isEmpty())
        return false;

    // startDetached takes an argv list — no shell is involved and the
    // url travels as exactly one argument, so a crafted url cannot
    // inject extra arguments or shell metacharacters.
    return QProcess::startDetached(args.takeFirst(), args << QString::fromUtf8(url.toEncoded()));
}

void DownloadManager::download(QWebEnginePage *page, const QUrl &url, bool requestFileName)
{
    if (!page || url.isEmpty())
        return;
    if (externalDownload(url))
        return;

    // Consumed by handleDownloadRequested() when the profile reports
    // the request for this download.
    m_requestFileNameNext = requestFileName;
    page->download(url);
}

void DownloadManager::handleDownloadRequested(QWebEngineDownloadRequest *download)
{
    if (!download || download->url().isEmpty())
        return;

#ifdef DOWNLOADMANAGER_DEBUG
    qDebug() << "DownloadManager::" << __FUNCTION__ << download->url()
             << "requestFileName" << m_requestFileNameNext;
#endif

    // A "Try Again" click re-issues the download through a page; attach
    // the fresh request to the item that is waiting for it.
    for (int i = 0; i < m_downloads.count(); ++i) {
        DownloadItem *item = m_downloads.at(i);
        if (item->m_awaitingRetry && item->m_url == download->url()) {
            item->m_awaitingRetry = false;
            item->attach(download);
            updateRow(item);
            updateActiveItemCount();
            m_requestFileNameNext = false;
            return;
        }
    }

    if (externalDownload(download->url())) {
        download->cancel();
        return;
    }

    DownloadItem *item = new DownloadItem(download, m_requestFileNameNext, this);
    m_requestFileNameNext = false;
    addItem(item);

    if (item->m_canceledByUser)
        return;

    if (!isVisible())
        show();

    activateWindow();
    raise();
}

void DownloadManager::addItem(DownloadItem *item)
{
    connect(item, &DownloadItem::statusChanged, this, [this]() { updateRow(); });
    connect(item, &DownloadItem::downloadFinished, this, &DownloadManager::finished);
    int row = m_downloads.count();
    m_model->beginInsertRows(QModelIndex(), row, row);
    m_downloads.append(item);
    m_model->endInsertRows();
    updateItemCount();
    downloadsView->setIndexWidget(m_model->index(row, 0), item);
    QIcon icon = style()->standardIcon(QStyle::SP_FileIcon);
    item->fileIcon->setPixmap(icon.pixmap(48, 48));
    downloadsView->setRowHeight(row, item->sizeHint().height());
    updateRow(item); //incase download finishes before the constructor returns
    updateActiveItemCount();
}

void DownloadManager::updateActiveItemCount()
{
    int acCount = activeDownloads();
    if (acCount > 0) {
        setWindowTitle(tr("Downloading %1").arg(acCount));
    } else {
        setWindowTitle(tr("Downloads"));
    }
}

void DownloadManager::finished()
{
    updateActiveItemCount();
    if (isVisible()) {
        QApplication::alert(this);
    }
}


void DownloadManager::updateRow()
{
    if (DownloadItem *item = qobject_cast<DownloadItem*>(sender()))
        updateRow(item);
}

void DownloadManager::updateRow(DownloadItem *item)
{
    int row = m_downloads.indexOf(item);
    if (-1 == row)
        return;
    if (m_iconProvider.isNull())
        m_iconProvider.reset(new QFileIconProvider);
    QIcon icon = m_iconProvider->icon(QFileInfo(item->m_outputFileName));
    if (icon.isNull())
        icon = style()->standardIcon(QStyle::SP_FileIcon);
    item->fileIcon->setPixmap(icon.pixmap(48, 48));

    int oldHeight = downloadsView->rowHeight(row);
    downloadsView->setRowHeight(row, qMax(oldHeight, item->minimumSizeHint().height()));

    bool remove = false;
    if (!item->downloading() && item->m_offTheRecord)
        remove = true;

    if (item->downloadedSuccessfully()
        && removePolicy() == DownloadManager::SuccessFullDownload) {
        remove = true;
    }
    if (remove)
        m_model->removeRow(row);

    cleanupButton->setEnabled(m_downloads.count() - activeDownloads() > 0);
}

DownloadManager::RemovePolicy DownloadManager::removePolicy() const
{
    return m_removePolicy;
}

void DownloadManager::setRemovePolicy(RemovePolicy policy)
{
    if (policy == m_removePolicy)
        return;
    m_removePolicy = policy;
    m_autoSaver->changeOccurred();
}

void DownloadManager::save() const
{
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    QMetaEnum removePolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("RemovePolicy"));
    settings.setValue(QLatin1String("removeDownloadsPolicy"), QLatin1String(removePolicyEnum.valueToKey(m_removePolicy)));
    settings.setValue(QLatin1String("size"), size());
    if (m_removePolicy == Exit)
        return;

    int saved = 0;
    for (int i = 0; i < m_downloads.count(); ++i) {
        // Off-the-record downloads leave no record: an in-flight
        // private download's url and target path must never reach
        // QSettings (SEC07).
        if (m_downloads[i]->m_offTheRecord)
            continue;
        QString key = QString(QLatin1String("download_%1_")).arg(saved++);
        settings.setValue(key + QLatin1String("url"), m_downloads[i]->m_url);
        settings.setValue(key + QLatin1String("location"), m_downloads[i]->m_outputFileName);
        settings.setValue(key + QLatin1String("done"), m_downloads[i]->downloadedSuccessfully());
    }
    int i = saved;
    QString key = QString(QLatin1String("download_%1_")).arg(i);
    while (settings.contains(key + QLatin1String("url"))) {
        settings.remove(key + QLatin1String("url"));
        settings.remove(key + QLatin1String("location"));
        settings.remove(key + QLatin1String("done"));
        key = QString(QLatin1String("download_%1_")).arg(++i);
    }
}

void DownloadManager::load()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    QSize size = settings.value(QLatin1String("size")).toSize();
    if (size.isValid())
        resize(size);
    QByteArray value = settings.value(QLatin1String("removeDownloadsPolicy"), QLatin1String("Never")).toByteArray();
    QMetaEnum removePolicyEnum = staticMetaObject.enumerator(staticMetaObject.indexOfEnumerator("RemovePolicy"));
    m_removePolicy = removePolicyEnum.keyToValue(value) == -1 ?
                        Never :
                        static_cast<RemovePolicy>(removePolicyEnum.keyToValue(value));

    int i = 0;
    QString key = QString(QLatin1String("download_%1_")).arg(i);
    while (settings.contains(key + QLatin1String("url"))) {
        QUrl url = settings.value(key + QLatin1String("url")).toUrl();
        QString fileName = settings.value(key + QLatin1String("location")).toString();
        bool done = settings.value(key + QLatin1String("done"), true).toBool();
        if (!url.isEmpty() && !fileName.isEmpty()) {
            DownloadItem *item = new DownloadItem(nullptr, false, this);
            item->m_outputFileName = fileName;
            item->fileNameLabel->setText(QFileInfo(item->m_outputFileName).fileName());
            item->setAccessibleName(item->fileNameLabel->text());
            item->m_url = url;
            item->stopButton->setVisible(false);
            item->stopButton->setEnabled(false);
            item->tryAgainButton->setVisible(!done);
            item->tryAgainButton->setEnabled(!done);
            item->progressBar->setVisible(false);
            addItem(item);
        }
        key = QString(QLatin1String("download_%1_")).arg(++i);
    }
    cleanupButton->setEnabled(m_downloads.count() - activeDownloads() > 0);
    updateActiveItemCount();
}

void DownloadManager::cleanup()
{
    if (m_downloads.isEmpty())
        return;
    m_model->removeRows(0, m_downloads.count());
    updateItemCount();
    updateActiveItemCount();
    if (m_downloads.isEmpty())
        m_iconProvider.reset();
    m_autoSaver->changeOccurred();
}

void DownloadManager::updateItemCount()
{
    int count = m_downloads.count();
    itemCount->setText(tr("%n Download(s)", "", count));
}

void DownloadManager::setDownloadDirectory(QString directory)
{
    m_downloadDirectory = std::move(directory);
    if (!m_downloadDirectory.isEmpty())
        m_downloadDirectory += QLatin1Char('/');
}

QString DownloadManager::downloadDirectory()
{
    return m_downloadDirectory;
}

QString DownloadManager::timeString(double timeRemaining)
{
    QString remaining;

    if (timeRemaining > 60) {
        timeRemaining = timeRemaining / 60;
        timeRemaining = floor(timeRemaining);
        remaining = tr("%n minutes remaining", "", int(timeRemaining));
    }
    else {
        timeRemaining = floor(timeRemaining);
        remaining = tr("%n seconds remaining", "", int(timeRemaining));
    }

    return remaining;
}

QString DownloadManager::dataString(qint64 size)
{
    QString unit;
    double newSize;

    if (size < 1024) {
        newSize = size;
        unit = tr("bytes");
    } else if (size < 1024 * 1024) {
        newSize = (double)size / (double)1024;
        unit = tr("kB");
    } else if (size < 1024 * 1024 * 1024) {
        newSize = (double)size / (double)(1024 * 1024);
        unit = tr("MB");
    } else {
        newSize = (double)size / (double)(1024 * 1024 * 1024);
        unit = tr("GB");
    }

    return QString(QLatin1String("%1 %2")).arg(newSize, 0, 'f', 1).arg(unit);
}

DownloadModel::DownloadModel(DownloadManager *downloadManager, QObject *parent)
    : QAbstractListModel(parent)
    , m_downloadManager(downloadManager)
{
}

QVariant DownloadModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0 || index.row() >= rowCount(index.parent()))
        return QVariant();
    DownloadItem *item = m_downloadManager->m_downloads.at(index.row());
    switch (role) {
    case FileNameRole:
        return item->fileNameLabel->text();
    case OutputFileRole:
        return item->m_outputFileName;
    case SourceUrlRole:
        return item->m_url;
    case CompletedRole:
        return item->downloadedSuccessfully();
    default:
        break;
    }
    if (role == Qt::ToolTipRole)
        if (!m_downloadManager->m_downloads.at(index.row())->downloadedSuccessfully())
            return m_downloadManager->m_downloads.at(index.row())->downloadInfoLabel->text();
    // The row is rendered by an index widget, so DisplayRole stays
    // empty; assistive tools still get the file name and status.
    if (role == Qt::AccessibleTextRole) {
        DownloadItem *item = m_downloadManager->m_downloads.at(index.row());
        const QString info = item->downloadInfoLabel->text();
        return info.isEmpty() ? item->fileNameLabel->text()
                              : QString(item->fileNameLabel->text() + QLatin1String(", ") + info);
    }
    if (role == Qt::AccessibleDescriptionRole) {
        DownloadItem *item = m_downloadManager->m_downloads.at(index.row());
        return tr("Download %1 to %2").arg(item->m_url.toString(),
                                           item->m_outputFileName);
    }
    return QVariant();
}

int DownloadModel::rowCount(const QModelIndex &parent) const
{
    return (parent.isValid()) ? 0 : m_downloadManager->m_downloads.count();
}

bool DownloadModel::removeRows(int row, int count, const QModelIndex &parent)
{
    if (parent.isValid())
        return false;

    int lastRow = row + count - 1;
    for (int i = lastRow; i >= row; --i) {
        DownloadItem *item = m_downloadManager->m_downloads.at(i);
        if (item->downloadedSuccessfully()
            || item->tryAgainButton->isEnabled()) {
            // Removing the row also deletes the half-written file an
            // interrupted or cancelled download left behind (SEC01).
            if (!item->downloadedSuccessfully())
                item->removePartialFile();
            beginRemoveRows(parent, i, i);
            m_downloadManager->m_downloads.takeAt(i)->deleteLater();
            endRemoveRows();
        }
    }
    m_downloadManager->m_autoSaver->changeOccurred();
    m_downloadManager->updateItemCount();
    return true;
}

Qt::ItemFlags DownloadModel::flags(const QModelIndex &index) const
{
    if (index.row() < 0 || index.row() >= rowCount(index.parent()))
        return Qt::ItemFlags();

    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);

    DownloadItem *item = m_downloadManager->m_downloads.at(index.row());
    if (item->downloadedSuccessfully())
        return defaultFlags | Qt::ItemIsDragEnabled;

    return defaultFlags;
}

QMimeData *DownloadModel::mimeData(const QModelIndexList &indexes) const
{
    QMimeData *mimeData = new QMimeData();
    QList<QUrl> urls;
    for (const QModelIndex &index : indexes) {
        if (!index.isValid())
            continue;
        DownloadItem *item = m_downloadManager->m_downloads.at(index.row());
        urls.append(QUrl::fromLocalFile(QFileInfo(item->m_outputFileName).absoluteFilePath()));
    }
    mimeData->setUrls(urls);
    return mimeData;
}
