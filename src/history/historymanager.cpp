/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
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
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
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

#include "historymanager.h"

#include "autosaver.h"
#include "browserpaths.h"
#include "history.h"
#include "historyparser.h"
#include "startupprofile.h"

#include <algorithm>

#include <qcoreapplication.h>
#include <qdesktopservices.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qpointer.h>
#include <qsettings.h>
#include <qtemporaryfile.h>

#include <qdebug.h>

#ifdef ARORA_RUSTCORE
#include "rustcorebridge.h"

#include <qbuffer.h>
#include <qimage.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qpixmap.h>

#include <rustcore.h>

// Icons are keyed per host (the HIST01 contract); hostless urls fall
// back to the whole url so data:/file: icons still behave.
static QString historyIconKey(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host.isEmpty() ? url.toString() : host;
}
#endif

QString HistoryEntry::userTitle() const
{
    // when there is no title try to generate one from the url
    if (title.isEmpty()) {
        QString page = QFileInfo(QUrl(url).path()).fileName();
        if (!page.isEmpty())
            return page;
        return url;
    }
    return title;
}

static const unsigned int HISTORY_VERSION = HistoryParser::Version;

HistoryManager::HistoryManager(QObject *parent)
    : QObject(parent)
    , m_saveTimer(new AutoSaver(this))
    , m_daysToExpire(30)
    , m_historyModel(nullptr)
    , m_historyFilterModel(nullptr)
    , m_historyTreeModel(nullptr)
{
    m_expiredTimer.setSingleShot(true);
    connect(&m_expiredTimer, &QTimer::timeout,
            this, &HistoryManager::checkForExpired);
    connect(this, &HistoryManager::entryAdded,
            m_saveTimer, &AutoSaver::changeOccurred);
    connect(this, &HistoryManager::entryRemoved,
            m_saveTimer, &AutoSaver::changeOccurred);
#ifdef ARORA_RUSTCORE
    if (QCoreApplication::instance()) {
        // Rust-side writers announce the "history" topic through the
        // queued callback->signal bridge; mark the view dirty so the
        // autosave path notices like it does for local edits.
        connect(RustCoreBridge::instance(), &RustCoreBridge::storeChanged,
                this, [this](const QString &topic) {
            if (topic == QLatin1String("history"))
                m_saveTimer->changeOccurred();
        });
    }
#endif
    load();

    m_historyModel = new HistoryModel(this, this);
    m_historyFilterModel = new HistoryFilterModel(m_historyModel, this);
    m_historyTreeModel = new HistoryTreeModel(m_historyFilterModel, this);
}

HistoryManager *HistoryManager::instance()
{
    static QPointer<HistoryManager> manager;
    if (!manager)
        manager = new HistoryManager(qApp);
    return manager;
}

HistoryManager::~HistoryManager()
{
    // remove history items on application exit
    if (m_daysToExpire == -2)
        clear();
    m_saveTimer->saveIfNeccessary();
}

QList<HistoryEntry> HistoryManager::history() const
{
    return m_history;
}

// HIST01: the favicon store is keyed by host so every entry for a
// site shares its icon, and persisted like the WebKit icon database
// it replaces — one small png per host under <data dir>/icons/.
static QString iconStoreDir()
{
    return BrowserPaths::dataFilePath(QLatin1String("icons"));
}

#ifndef ARORA_RUSTCORE
static QString iconFilePath(const QString &host)
{
    return iconStoreDir() + QLatin1Char('/')
            + QString::fromLatin1(QUrl::toPercentEncoding(host))
            + QLatin1String(".png");
}
#endif

QIcon HistoryManager::icon(const QUrl &url) const
{
#ifdef ARORA_RUSTCORE
    const QString key = historyIconKey(url);
    QIcon icon = m_icons.value(key);
    if (icon.isNull()) {
        rustCoreEnsureDataDir();
        const QByteArray k = key.toUtf8();
        RcBuffer out{};
        if (rc_hist_icon_get(k.constData(), &out) == RC_OK && out.data) {
            QPixmap pixmap;
            if (pixmap.loadFromData(out.data, int(out.len), "PNG"))
                icon = QIcon(pixmap);
            rc_buffer_free(out);
        }
        if (!icon.isNull())
            const_cast<HistoryManager *>(this)->m_icons.insert(key, icon);
    }
#else
    const QString host = url.host().toLower();
    const QString key = host.isEmpty() ? url.toString() : host;
    QIcon icon = m_icons.value(key);
    if (icon.isNull() && !host.isEmpty() && !m_iconMisses.contains(host)) {
        const QPixmap pixmap(iconFilePath(host));
        if (!pixmap.isNull()) {
            icon = QIcon(pixmap);
            m_icons.insert(host, icon);
        } else {
            // remember the miss so lookups don't keep stat()ing
            m_iconMisses.insert(host);
        }
    }
#endif
    if (icon.isNull())
        icon = QIcon(QLatin1String(":graphics/defaulticon.png"));
    return icon;
}

void HistoryManager::setIcon(const QUrl &url, const QIcon &icon)
{
    if (url.isEmpty() || icon.isNull())
        return;
    const QString host = url.host().toLower();
#ifdef ARORA_RUSTCORE
    const QString key = historyIconKey(url);
    m_icons.insert(key, icon);
    const QImage image = icon.pixmap(QSize(32, 32)).toImage();
    QByteArray png;
    QBuffer buffer(&png);
    if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
        rustCoreEnsureDataDir();
        const QByteArray k = key.toUtf8();
        rc_hist_icon_set(k.constData(),
                         reinterpret_cast<const uint8_t *>(png.constData()),
                         size_t(png.size()));
    }
#else
    const QString key = host.isEmpty() ? url.toString() : host;
    QHash<QString, QIcon>::const_iterator it = m_icons.constFind(key);
    if (it != m_icons.constEnd() && it->cacheKey() == icon.cacheKey())
        return;
    m_icons.insert(key, icon);
    m_iconMisses.remove(key);

    if (!host.isEmpty()) {
        QDir().mkpath(iconStoreDir());
        icon.pixmap(64).save(iconFilePath(host), "PNG");
    }
#endif

    // Entries already in the model share the host icon — refresh the
    // views (menu, dialog, completer) so they repaint immediately.
    for (int i = 0; i < m_history.count(); ++i) {
        const QUrl entryUrl(m_history.at(i).url);
        if (host.isEmpty()
                ? entryUrl == url
                : entryUrl.host().compare(host, Qt::CaseInsensitive) == 0)
            emit entryUpdated(i);
    }
}

void HistoryManager::clearIcons()
{
    m_icons.clear();
    m_iconMisses.clear();
    QDir(iconStoreDir()).removeRecursively();
#ifdef ARORA_RUSTCORE
    rc_hist_icon_clear();
#endif
}

bool HistoryManager::historyContains(const QString &url) const
{
    return m_historyFilterModel->historyContains(url);
}

void HistoryManager::addHistoryEntry(const QString &url)
{
    QUrl cleanUrl(url);
    cleanUrl.setPassword(QString());
    cleanUrl.setHost(cleanUrl.host().toLower());
    HistoryEntry item(atomicString(cleanUrl.toString()), QDateTime::currentDateTime());
    prependHistoryEntry(item);
}

void HistoryManager::setHistory(const QList<HistoryEntry> &history, bool loadedAndSorted)
{
    m_history = history;

    // verify that it is sorted by date
    if (!loadedAndSorted)
        std::sort(m_history.begin(), m_history.end());

    checkForExpired();

#ifdef ARORA_RUSTCORE
    // loadedAndSorted means the list came *from* the store (load());
    // anything else is a wholesale replace the store must mirror.
    if (!loadedAndSorted)
        rustReplaceAll();
#endif

    if (loadedAndSorted) {
        m_lastSavedUrl = m_history.value(0).url;
    } else {
        m_lastSavedUrl.clear();
        m_saveTimer->changeOccurred();
    }
    emit historyReset();
}

HistoryModel *HistoryManager::historyModel() const
{
    return m_historyModel;
}

HistoryFilterModel *HistoryManager::historyFilterModel() const
{
    return m_historyFilterModel;
}

HistoryTreeModel *HistoryManager::historyTreeModel() const
{
    return m_historyTreeModel;
}

void HistoryManager::checkForExpired()
{
    if (m_daysToExpire < 0 || m_history.isEmpty())
        return;

    QDateTime now = QDateTime::currentDateTime();
    int nextTimeout = 0;

    while (!m_history.isEmpty()) {
        QDateTime checkForExpired = m_history.last().dateTime;
        checkForExpired.setDate(checkForExpired.date().addDays(m_daysToExpire));
        if (now.daysTo(checkForExpired) > 7) {
            // check at most in a week to prevent int overflows on the timer
            nextTimeout = 7 * 86400;
        } else {
            nextTimeout = now.secsTo(checkForExpired);
        }
        if (nextTimeout > 0)
            break;
        HistoryEntry item = m_history.takeLast();
        // remove from saved file also
        m_lastSavedUrl.clear();
#ifdef ARORA_RUSTCORE
        rc_hist_remove(item.url.toUtf8().constData(),
                       item.title.toUtf8().constData(),
                       item.dateTime.toMSecsSinceEpoch());
#endif
        emit entryRemoved(item);
    }

    if (nextTimeout > 0)
        m_expiredTimer.start(nextTimeout * 1000);
}

void HistoryManager::prependHistoryEntry(const HistoryEntry &item)
{
    // Private browsing is a property of the page's profile now: WebPage
    // only feeds urls to the manager when its profile is not
    // off-the-record (the old QWebSettings::PrivateBrowsingEnabled
    // global flag is gone).
    m_history.prepend(item);
#ifdef ARORA_RUSTCORE
    rc_hist_add(item.url.toUtf8().constData(),
                item.title.toUtf8().constData(),
                item.dateTime.toMSecsSinceEpoch());
#endif
    emit entryAdded(item);
    if (m_history.count() == 1)
        checkForExpired();
}

void HistoryManager::updateHistoryEntry(const QUrl &url, const QString &title)
{
    for (int i = 0; i < m_history.count(); ++i) {
        if (url == m_history.at(i).url) {
            m_history[i].title = atomicString(title);
#ifdef ARORA_RUSTCORE
            rc_hist_update_title(url.toString().toUtf8().constData(),
                                 m_history[i].title.toUtf8().constData());
#endif
            m_saveTimer->changeOccurred();
            if (m_lastSavedUrl.isEmpty())
                m_lastSavedUrl = m_history.at(i).url;
            emit entryUpdated(i);
            break;
        }
    }
}

void HistoryManager::removeHistoryEntry(const HistoryEntry &item)
{
    m_lastSavedUrl.clear();
    m_history.removeOne(item);
#ifdef ARORA_RUSTCORE
    rc_hist_remove(item.url.toUtf8().constData(),
                   item.title.toUtf8().constData(),
                   item.dateTime.toMSecsSinceEpoch());
#endif
    emit entryRemoved(item);
}

void HistoryManager::removeHistoryEntry(const QUrl &url, const QString &title)
{
    for (int i = 0; i < m_history.count(); ++i) {
        if (url == m_history.at(i).url
            && (title.isEmpty() || title == m_history.at(i).title)) {
            removeHistoryEntry(m_history.at(i));
            break;
        }
    }
}

int HistoryManager::daysToExpire() const
{
    return m_daysToExpire;
}

void HistoryManager::setDaysToExpire(int limit)
{
    if (m_daysToExpire == limit)
        return;
    m_daysToExpire = limit;
    checkForExpired();
    m_saveTimer->changeOccurred();
}

void HistoryManager::clear()
{
    m_history.clear();
    m_atomicStringHash.clear();
    m_lastSavedUrl.clear();
#ifdef ARORA_RUSTCORE
    rc_hist_clear();
#endif
    m_saveTimer->changeOccurred();
    m_saveTimer->saveIfNeccessary();
    emit historyReset();
    emit historyCleared();
}

void HistoryManager::loadSettings()
{
    // load settings
    QSettings settings;
    settings.beginGroup(QLatin1String("history"));
    m_daysToExpire = settings.value(QLatin1String("historyLimit"), 30).toInt();
}

void HistoryManager::load()
{
    StartupProfile::Scope profileScope("history load");
    loadSettings();

#ifdef ARORA_RUSTCORE
    // The canonical store is <data dir>/history.db (rusqlite); the
    // legacy QDataStream file is imported once, on first run.
    rustCoreEnsureDataDir();
    const QByteArray dbPath =
        BrowserPaths::dataFilePath(QLatin1String("history.db")).toUtf8();
    const bool fresh = rc_hist_exists(dbPath.constData()) == 0;
    if (rc_hist_open(dbPath.constData()) != RC_OK) {
        char *err = rc_last_error_message();
        qWarning() << "HistoryManager: cannot open history.db:"
                   << QString::fromUtf8(err ? err : "");
        rc_string_free(err);
        return;
    }
    if (fresh)
        importLegacyHistory();

    QList<HistoryEntry> list;
    const int64_t count = rc_hist_count();
    list.reserve(count > 0 ? int(count) : 0);
    for (int64_t i = 0; i < count; ++i) {
        char *json = rc_hist_entry_at(i);
        if (!json)
            continue;
        const QJsonObject o =
            QJsonDocument::fromJson(QByteArray(json)).object();
        rc_string_free(json);
        list.append(HistoryEntry(
            atomicString(o.value(QLatin1String("url")).toString()),
            QDateTime::fromMSecsSinceEpoch(
                o.value(QLatin1String("ts")).toInteger()),
            o.value(QLatin1String("title")).toString()));
    }
    setHistory(list, true);
    return;
#else
    QFile historyFile(BrowserPaths::dataFilePath(QLatin1String("history")));

    if (!historyFile.exists())
        return;
    if (!historyFile.open(QFile::ReadOnly)) {
        qWarning() << "Unable to open history file" << historyFile.fileName();
        return;
    }

    QDataStream in(&historyFile);
    const HistoryParser::Result parsed =
            HistoryParser::readEntries(in, m_atomicStringHash);

    setHistory(parsed.entries, true);

    // If we had to sort re-write the whole history sorted
    if (parsed.needToSort) {
        m_lastSavedUrl.clear();
        m_saveTimer->changeOccurred();
    }
#endif
}

#ifdef ARORA_RUSTCORE
// One-shot migration: the legacy QDataStream "history" file is parsed
// by the existing reader and written into history.db, which becomes
// the canonical store from then on (the old file is left in place).
void HistoryManager::importLegacyHistory()
{
    QFile historyFile(BrowserPaths::dataFilePath(QLatin1String("history")));
    if (!historyFile.exists() || !historyFile.open(QFile::ReadOnly))
        return;
    QDataStream in(&historyFile);
    const HistoryParser::Result parsed =
            HistoryParser::readEntries(in, m_atomicStringHash);
    // Oldest first: the store orders equal timestamps by insert seq.
    for (int i = parsed.entries.count() - 1; i >= 0; --i) {
        const HistoryEntry &e = parsed.entries.at(i);
        rc_hist_add(e.url.toUtf8().constData(),
                    e.title.toUtf8().constData(),
                    e.dateTime.toMSecsSinceEpoch());
    }
}

// Rebuilds the store to mirror m_history exactly — setHistory() is
// the test/import seam that wholesale-replaces the list.
void HistoryManager::rustReplaceAll()
{
    rc_hist_clear();
    for (int i = m_history.count() - 1; i >= 0; --i) {
        const HistoryEntry &e = m_history.at(i);
        rc_hist_add(e.url.toUtf8().constData(),
                    e.title.toUtf8().constData(),
                    e.dateTime.toMSecsSinceEpoch());
    }
}
#endif // ARORA_RUSTCORE

QString HistoryManager::atomicString(const QString &string) {
    QHash<QString, int>::const_iterator it = m_atomicStringHash.constFind(string);
    if (it == m_atomicStringHash.constEnd()) {
        QHash<QString, int>::iterator insertedIterator = m_atomicStringHash.insert(string, 0);
        return insertedIterator.key();
    }
    return it.key();
}

void HistoryManager::save()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("history"));
    settings.setValue(QLatin1String("historyLimit"), m_daysToExpire);

#ifdef ARORA_RUSTCORE
    // history.db writes through on every mutation — nothing pending
    // to flush beyond the settings above.
    return;
#endif

    bool saveAll = m_lastSavedUrl.isEmpty();
    int first = m_history.count() - 1;
    if (!saveAll) {
        // find the first one to save
        for (int i = 0; i < m_history.count(); ++i) {
            if (m_history.at(i).url == m_lastSavedUrl) {
                first = i - 1;
                break;
            }
        }
    }
    if (first == m_history.count() - 1)
        saveAll = true;

    QFile historyFile(BrowserPaths::dataFilePath(QLatin1String("history")));

    // When saving everything use a temporary file to prevent possible data loss.
    QTemporaryFile tempFile;
    tempFile.setAutoRemove(false);
    bool open = false;
    if (saveAll) {
        open = tempFile.open();
    } else {
        open = historyFile.open(QFile::Append);
    }

    if (!open) {
        qWarning() << "Unable to open history file for saving"
                   << (saveAll ? tempFile.fileName() : historyFile.fileName());
        return;
    }

    QDataStream out(saveAll ? &tempFile : &historyFile);
    for (int i = first; i >= 0; --i) {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        HistoryEntry item = m_history.at(i);
        stream << HISTORY_VERSION << item.url << item.dateTime << item.title;
        out << data;
    }
    tempFile.close();

    if (saveAll) {
        if (historyFile.exists() && !historyFile.remove())
            qWarning() << "History: error removing old history." << historyFile.errorString();
        if (!tempFile.rename(historyFile.fileName()))
            qWarning() << "History: error moving new history over old." << tempFile.errorString() << historyFile.fileName();
    }
    m_lastSavedUrl = m_history.value(0).url;
}