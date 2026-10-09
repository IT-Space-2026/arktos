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

#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include "ui_downloads.h"
#include "ui_downloaditem.h"

#include <qelapsedtimer.h>
#include <qpointer.h>
#include <qscopedpointer.h>
#include <qwebenginedownloadrequest.h>

class QWebEnginePage;
class QWebEngineProfile;
#ifdef ARORA_RUSTDL
class RustDownloadEngine;
#endif

class DownloadItem : public QWidget, public Ui_DownloadItem
{
    Q_OBJECT

signals:
    void statusChanged();
    void progress(qint64 bytesReceived = 0, qint64 bytesTotal = 0);
    void downloadFinished();

public:
    DownloadItem(QWebEngineDownloadRequest *download = nullptr, bool requestFileName = false, QWidget *parent = nullptr);
#ifdef ARORA_RUSTDL
    // Card backed by the rustdl accelerated engine instead of a
    // Chromium request (DLACC06).  The engine object is re-parented
    // to the item.
    DownloadItem(RustDownloadEngine *engine, bool requestFileName, QWidget *parent);
#endif
    bool downloading() const;
    bool downloadedSuccessfully() const;

    qint64 bytesTotal() const;
    qint64 bytesReceived() const;
    double remainingTime() const;
    double currentSpeed() const;

    // Re-attaches the item to a fresh request.  "Try Again" re-issues the
    // download through the page, which produces a new request object.
    void attach(QWebEngineDownloadRequest *download);

    // SEC01 hardening helpers — static so autotests can drive them
    // without a live download.
    [[nodiscard]] static QString sanitizeFileName(const QString &suggestedName);
    [[nodiscard]] static bool isDangerousExtension(const QString &fileName);
    [[nodiscard]] static bool isExecutableMimeType(const QString &mimeType);
    static void removeExecutableBit(const QString &path);

    QUrl m_url;
    QString m_outputFileName;

private slots:
    void stop();
    void tryAgain();
    void open();

    void downloadProgressUpdate();
    void downloadStateChanged(QWebEngineDownloadRequest::DownloadState state);
    void finished();

private:
    void getFileName();
    void init();
    void updateInfoLabel();
    bool confirmSafeToSave(const QString &fileName);
    void removePartialFile();

    QString saveFileName(const QString &directory) const;

    // Backend-neutral accessors — the same card drives a Chromium
    // request or the rustdl engine behind these.
    QWebEnginePage *page() const;
    QString mimeType() const;
    QString suggestedFileName() const;
    QWebEngineDownloadRequest::DownloadState currentState() const;
    bool isFinished() const;

    QPointer<QWebEngineDownloadRequest> m_download;
#ifdef ARORA_RUSTDL
    QPointer<RustDownloadEngine> m_engine;
#endif
    bool m_requestFileName;
    qint64 m_bytesReceived;
    QElapsedTimer m_downloadTime;
    bool m_finishedDownloading;
    bool m_gettingFileName;
    bool m_canceledByUser;
    bool m_awaitingRetry;
    bool m_offTheRecord;
    QElapsedTimer m_lastProgressTime;

    friend class DownloadManager;
    friend class DownloadModel;
};

class AutoSaver;
class DownloadModel;
QT_BEGIN_NAMESPACE
class QFileIconProvider;
class QMimeData;
QT_END_NAMESPACE

class DownloadManager : public QDialog, public Ui_DownloadDialog
{
    Q_OBJECT
    Q_PROPERTY(RemovePolicy removePolicy READ removePolicy WRITE setRemovePolicy)

public:
    enum RemovePolicy {
        Never,
        Exit,
        SuccessFullDownload
    };
    Q_ENUM(RemovePolicy)

    DownloadManager(QWidget *parent = nullptr);
    ~DownloadManager();

    // Lazy app singleton — replaces BrowserApplication::downloadManager()
    // while browserapplication.cpp is uncompiled (MIG15 delegates).
    static DownloadManager *instance();

    int activeDownloads() const;
    // SLEEP01: true while any in-flight download was initiated by
    // this page — the sleeping-tabs sweep skips such tabs so a
    // suspend can never orphan a stream.
    bool hasActiveDownloadForPage(QWebEnginePage *page) const;
    bool allowQuit();

    RemovePolicy removePolicy() const;
    void setRemovePolicy(RemovePolicy policy);

    static QString timeString(double timeRemaining);
    static QString dataString(qint64 size);

    void setDownloadDirectory(QString directory);
    QString downloadDirectory();

    // Hooks this manager into the profile's downloadRequested signal.
    // Must be called once for every profile that can produce downloads
    // (default profile and the off-the-record private profile).
    void installOnProfile(QWebEngineProfile *profile);

    // Hidden page used to re-issue downloads whose originating page is
    // already gone (retry of an interrupted or restored download).
    QWebEnginePage *retryPage(bool offTheRecord);

    // SIDE01: read-only model access for the sidebar downloads page —
    // rows still render through the dialog's index widgets; secondary
    // views map the DownloadModel::Roles.
    DownloadModel *model() const { return m_model; }

    // SEC09: hands the url to the user-configured external download
    // handler (Settings > downloadmanager/externalPath).  The url
    // becomes a raw argv argument to another program, so only real
    // remote-download schemes are passed — file:, data:, blob:,
    // javascript: and the arora-* schemes are refused and fall back to
    // the internal download path.  Static so autotests can drive it
    // without a live download.
    static bool externalDownload(const QUrl &url);

public slots:
    // WebEngine downloads can only be initiated from a page; there is no
    // profile-level download() entry point.
    void download(QWebEnginePage *page, const QUrl &url, bool requestFileName = false);
    void handleDownloadRequested(QWebEngineDownloadRequest *download);
    void cleanup();

private slots:
    void save() const;
    void updateRow(DownloadItem *item);
    void updateRow();
    void finished();

private:
    void addItem(DownloadItem *item);
    void updateItemCount();
    void load();
    void updateActiveItemCount();

    AutoSaver *m_autoSaver;
    DownloadModel *m_model;
    QScopedPointer<QFileIconProvider> m_iconProvider;
    QWebEnginePage *m_retryPage;
    QWebEnginePage *m_retryPageOtr;
    QList<DownloadItem*> m_downloads;
    RemovePolicy m_removePolicy;
    QString m_downloadDirectory;
    // Consumed by the next downloadRequested for a download issued
    // through download() with requestFileName set.
    bool m_requestFileNameNext;

    friend class DownloadModel;
    friend class DownloadItem;
};

class DownloadModel : public QAbstractListModel
{
    friend class DownloadManager;
    Q_OBJECT

public:
    // SIDE01: plain-data roles for views that cannot host the
    // dialog's per-row DownloadItem index widgets (DisplayRole stays
    // empty so the dialog never paints text under its widgets).
    enum Roles {
        FileNameRole = Qt::UserRole + 1,
        OutputFileRole,
        SourceUrlRole,
        CompletedRole
    };

    DownloadModel(DownloadManager *downloadManager, QObject *parent = nullptr);
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
    DownloadManager *m_downloadManager;

};

#endif // DOWNLOADMANAGER_H
