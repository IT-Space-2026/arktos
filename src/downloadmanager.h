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
#include <qwebenginedownloadrequest.h>

class QWebEnginePage;
class QWebEngineProfile;

class DownloadItem : public QWidget, public Ui_DownloadItem
{
    Q_OBJECT

signals:
    void statusChanged();
    void progress(qint64 bytesReceived = 0, qint64 bytesTotal = 0);
    void downloadFinished();

public:
    DownloadItem(QWebEngineDownloadRequest *download = 0, bool requestFileName = false, QWidget *parent = 0);
    bool downloading() const;
    bool downloadedSuccessfully() const;

    qint64 bytesTotal() const;
    qint64 bytesReceived() const;
    double remainingTime() const;
    double currentSpeed() const;

    // Re-attaches the item to a fresh request.  "Try Again" re-issues the
    // download through the page, which produces a new request object.
    void attach(QWebEngineDownloadRequest *download);

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

    QString saveFileName(const QString &directory) const;

    QPointer<QWebEngineDownloadRequest> m_download;
    bool m_requestFileName;
    qint64 m_bytesReceived;
    QElapsedTimer m_downloadTime;
    bool m_finishedDownloading;
    bool m_gettingFileName;
    bool m_canceledFileSelect;
    bool m_awaitingRetry;
    bool m_offTheRecord;
    QElapsedTimer m_lastProgressTime;

    friend class DownloadManager;
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

    DownloadManager(QWidget *parent = 0);
    ~DownloadManager();
    int activeDownloads() const;
    bool allowQuit();

    RemovePolicy removePolicy() const;
    void setRemovePolicy(RemovePolicy policy);

    static QString timeString(double timeRemaining);
    static QString dataString(qint64 size);

    void setDownloadDirectory(const QString &directory);
    QString downloadDirectory();

    // Hooks this manager into the profile's downloadRequested signal.
    // Must be called once for every profile that can produce downloads
    // (default profile and the off-the-record private profile).
    void installOnProfile(QWebEngineProfile *profile);

    // Hidden page used to re-issue downloads whose originating page is
    // already gone (retry of an interrupted or restored download).
    QWebEnginePage *retryPage(bool offTheRecord);

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
    bool externalDownload(const QUrl &url);
    void updateActiveItemCount();

    AutoSaver *m_autoSaver;
    DownloadModel *m_model;
    QFileIconProvider *m_iconProvider;
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
    DownloadModel(DownloadManager *downloadManager, QObject *parent = 0);
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;
    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex());
    Qt::ItemFlags flags(const QModelIndex &index) const;
    QMimeData *mimeData(const QModelIndexList &indexes) const;

private:
    DownloadManager *m_downloadManager;

};

#endif // DOWNLOADMANAGER_H
