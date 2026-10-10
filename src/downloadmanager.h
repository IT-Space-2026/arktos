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

#include "ui_downloaditem.h"

#include "engineinterface.h"

#include <qabstractitemmodel.h>
#include <qdatetime.h>
#include <qelapsedtimer.h>
#include <qpointer.h>
#include <qscopedpointer.h>
#include <qvector.h>

class DownloadManager;
class QWebEnginePage;
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
    DownloadItem(Engine::DownloadRequest *download = nullptr, bool requestFileName = false, QWidget *parent = nullptr);
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

    // DOWN01 detail card — collapsed shows the classic compact row,
    // expanded adds source/destination, timestamps, size, a live speed
    // sparkline and Restart/Show-in-Folder actions.
    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);
    QDateTime startedTime() const { return m_startedTime; }
    QDateTime finishedTime() const { return m_finishedTime; }
    int speedSampleCount() const { return m_speedSamples.count(); }

    // Re-attaches the item to a fresh request.  "Try Again" re-issues the
    // download through the page, which produces a new request object.
    void attach(Engine::DownloadRequest *download);

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
    // Card actions: Restart re-issues the url for any finished or
    // failed download; Show in File Manager reveals the folder.
    void restart();
    void showInFolder();

    void downloadProgressUpdate();
    void downloadStateChanged(Engine::DownloadRequest::State state);
    void finished();

private:
    void getFileName();
    void init();
    void updateInfoLabel();
    void updateDetails();
    void restartDownload();
    void sampleSpeed();
    void freezeSpeedSeries();
    bool confirmSafeToSave(const QString &fileName);
    void removePartialFile();

    QString saveFileName(const QString &directory) const;

    // Backend-neutral accessors — the same card drives a Chromium
    // request or the rustdl engine behind these.
    Engine::Page *page() const;
    QString mimeType() const;
    QString suggestedFileName() const;
    Engine::DownloadRequest::State currentState() const;
    bool isFinished() const;

    QPointer<Engine::DownloadRequest> m_download;
#ifdef ARORA_RUSTDL
    QPointer<RustDownloadEngine> m_engine;
#endif
    // DOWN02: the owning manager, captured at construction — the item
    // is reparented into the sidebar's detail pane while displayed, so
    // a window() lookup can no longer find the manager.
    QPointer<DownloadManager> m_manager;
    bool m_requestFileName;
    qint64 m_bytesReceived;
    QElapsedTimer m_downloadTime;
    bool m_finishedDownloading;
    bool m_gettingFileName;
    bool m_canceledByUser;
    bool m_awaitingRetry;
    bool m_offTheRecord;
    QElapsedTimer m_lastProgressTime;

    // DOWN01 card state.  m_speedSamples is a bounded ring of
    // instantaneous bytes/second readings taken every ~500ms; it is
    // frozen when the transfer reaches a terminal state and
    // m_finishedTime records the wall-clock end.  m_restoredTotalBytes
    // carries the persisted size for items reloaded from QSettings
    // (they have no live backend to query).
    QVector<double> m_speedSamples;
    qint64 m_lastSampleMs;
    qint64 m_lastSampleBytes;
    qint64 m_restoredTotalBytes;
    QDateTime m_startedTime;
    QDateTime m_finishedTime;
    double m_finalSpeed;
    bool m_expanded;

    friend class DownloadManager;
    friend class DownloadModel;
};

class AutoSaver;
class DownloadModel;
QT_BEGIN_NAMESPACE
class QFileIconProvider;
class QMimeData;
QT_END_NAMESPACE

// DOWN02: the separate downloads window is gone — the sidebar's
// Downloads panel is the download surface.  The manager is now a
// never-shown controller widget: items stay widget children of it so
// ownership, findChildren and lifetime keep working whether an item
// is parked here or hosted in a panel's detail pane.
class DownloadManager : public QWidget
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
    bool hasActiveDownloadForPage(Engine::Page *page) const;
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
    void installOnProfile(Engine::Profile *profile);

    // Hidden page used to re-issue downloads whose originating page is
    // already gone (retry of an interrupted or restored download).
    Engine::Page *retryPage(bool offTheRecord);

    // SIDE01: read-only model access for the sidebar downloads page —
    // secondary views map the DownloadModel::Roles.
    DownloadModel *model() const { return m_model; }

    // DOWN02: row -> item for secondary views; the sidebar panel hosts
    // the item's detail card for the selected row.
    DownloadItem *itemAt(int row) const;

    // SEC09: hands the url to the user-configured external download
    // handler (Settings > downloadmanager/externalPath).  The url
    // becomes a raw argv argument to another program, so only real
    // remote-download schemes are passed — file:, data:, blob:,
    // javascript: and the arora-* schemes are refused and fall back to
    // the internal download path.  Static so autotests can drive it
    // without a live download.
    static bool externalDownload(const QUrl &url);

signals:
    // DOWN02: a new download joined the list — download surfaces (the
    // sidebar panel) use it to reveal their Downloads section.
    void itemAdded(DownloadItem *item);

public slots:
    // WebEngine downloads can only be initiated from a page; there is no
    // profile-level download() entry point.
    void download(Engine::Page *page, const QUrl &url, bool requestFileName = false);
    void handleDownloadRequested(Engine::DownloadRequest *download);
    void cleanup();

private slots:
    void save() const;
    void updateRow(DownloadItem *item);
    void updateRow();
    void finished();

private:
    void addItem(DownloadItem *item);
    void load();

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
    // SIDE01: plain-data roles for secondary views (the sidebar
    // panel).  DisplayRole stays empty — rows paint through delegates,
    // never as embedded text on an index widget.
    enum Roles {
        FileNameRole = Qt::UserRole + 1,
        OutputFileRole,
        SourceUrlRole,
        CompletedRole,
        // DOWN02: sort/filter/detail support for the sidebar panel —
        // start time, total size, the item's live status line and
        // whether a transfer is in flight.
        StartedTimeRole,
        SizeRole,
        InfoRole,
        DownloadingRole
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
