/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2008 Arora Browser
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
#include "profilesettings.h"

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
#include <qmessagebox.h>
#include <qpointer.h>
#include <qsettings.h>
#include <qtemporaryfile.h>

#include <qdebug.h>

#ifdef ARKTOS_RUSTCORE
#include "rustcorebridge.h"

#include <qbuffer.h>
#include <qimage.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qpixmap.h>

#include <arktos_core.h>

// Icons are keyed per host (the HIST01 contract); hostless urls fall
// back to the whole url so data:/file: icons still behave.
static QString historyIconKey(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host.isEmpty() ? url.toString() : host;
}

// MB06 envelope dispatch — every store op is one framed round trip
// through ac_mw_dispatch when the bridge serves; the payload's
// "profile" binds the scope per call, so no AcHistory handle is
// minted (the per-op open happens inside the op, with the same
// re-assert the facade handle used to do).  A null bridge keeps the
// legacy rc_* spelling over the histAnchor() re-bind — what
// ensureRustStore() used to hand the global store.
static void histAnchor(const QString &profileId)
{
    const QByteArray dbPath = BrowserPaths::profileDataFilePath(
        profileId, QLatin1String("history.db")).toUtf8();
    rc_hist_open(dbPath.constData());
}
static void histAdd(const QString &profileId, const QString &url,
                    const QString &title, int64_t ts)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(
            AcMw::History, AcMw::HistoryAdd,
            QJsonObject{{QStringLiteral("profile"), profileId},
                        {QStringLiteral("url"), url},
                        {QStringLiteral("title"), title},
                        {QStringLiteral("ts_ms"), qint64(ts)}});
        return;
    }
    histAnchor(profileId);
    rc_hist_add(url.toUtf8().constData(), title.toUtf8().constData(), ts);
}
static void histUpdateTitle(const QString &profileId, const QString &url,
                            const QString &title)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(
            AcMw::History, AcMw::HistoryUpdateTitle,
            QJsonObject{{QStringLiteral("profile"), profileId},
                        {QStringLiteral("url"), url},
                        {QStringLiteral("title"), title}});
        return;
    }
    histAnchor(profileId);
    rc_hist_update_title(url.toUtf8().constData(),
                         title.toUtf8().constData());
}
static void histRemove(const QString &profileId, const QString &url,
                       const QString &title, int64_t ts)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(
            AcMw::History, AcMw::HistoryRemove,
            QJsonObject{{QStringLiteral("profile"), profileId},
                        {QStringLiteral("url"), url},
                        {QStringLiteral("title"), title},
                        {QStringLiteral("ts_ms"), qint64(ts)}});
        return;
    }
    histAnchor(profileId);
    rc_hist_remove(url.toUtf8().constData(), title.toUtf8().constData(),
                   ts);
}
static void histClear(const QString &profileId)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(
            AcMw::History, AcMw::HistoryClear,
            QJsonObject{{QStringLiteral("profile"), profileId}});
        return;
    }
    histAnchor(profileId);
    rc_hist_clear();
}
static void histClearSince(const QString &profileId, int64_t sinceMs)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(
            AcMw::History, AcMw::HistoryClearSince,
            QJsonObject{{QStringLiteral("profile"), profileId},
                        {QStringLiteral("since_ms"), qint64(sinceMs)}});
        return;
    }
    histAnchor(profileId);
    rc_hist_clear_since(sinceMs);
}
static void histExpire(const QString &profileId, int64_t cutoffMs)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(
            AcMw::History, AcMw::HistoryExpire,
            QJsonObject{{QStringLiteral("profile"), profileId},
                        {QStringLiteral("cutoff_ms"), qint64(cutoffMs)}});
        return;
    }
    histAnchor(profileId);
    rc_hist_expire_older_than(cutoffMs, nullptr);
}
// RC16: the favicon store moved out of history.db into the per-
// profile icon cache (cache/icons/ — disposable by contract).
// MB07: same envelope dispatch as the history ops above — one framed
// round trip per call, the payload's "profile" binding the scope so
// no AcIcons mints.  A null bridge keeps the rc_icons_* spelling
// over the rustCoreEnsureDataDirFor() anchor.  Both spellings take
// the PAGE url and derive the historyIconKey() rule inside the
// store — the two lanes can never drift apart.
static bool iconPut(const QString &profileId, const QByteArray &url,
                    const QByteArray &png)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        return bridge
            ->mwCommand(AcMw::Icons, AcMw::IconsPut,
                        QJsonObject{{QLatin1String("profile"), profileId},
                                    {QLatin1String("url"),
                                     QString::fromUtf8(url)},
                                    {QLatin1String("png_b64"),
                                     QString::fromLatin1(png.toBase64())}})
            .isObject();
    }
    rustCoreEnsureDataDirFor(profileId);
    return rc_icons_put(url.constData(),
                        reinterpret_cast<const uint8_t *>(png.constData()),
                        size_t(png.size())) == RC_OK;
}
static bool iconGet(const QString &profileId, const QByteArray &url,
                    QByteArray *png)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        const QByteArray b64 = bridge
            ->mwQuery(AcMw::Icons, AcMw::IconsGet,
                      QJsonObject{{QLatin1String("profile"), profileId},
                                  {QLatin1String("url"),
                                   QString::fromUtf8(url)}})
            .toObject()
            .value(QLatin1String("png_b64"))
            .toString()
            .toLatin1();
        if (b64.isEmpty())
            return false;
        *png = QByteArray::fromBase64(b64);
        return true;
    }
    rustCoreEnsureDataDirFor(profileId);
    RcBuffer out{};
    if (rc_icons_get(url.constData(), &out) != RC_OK || !out.data)
        return false;
    *png = QByteArray(reinterpret_cast<const char *>(out.data),
                      int(out.len));
    rc_buffer_free(out);
    return true;
}
static void iconClear(const QString &profileId)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(AcMw::Icons, AcMw::IconsClear,
                          QJsonObject{{QLatin1String("profile"),
                                       profileId}});
        return;
    }
    rustCoreEnsureDataDirFor(profileId);
    rc_icons_clear();
}
static void iconSweep(const QString &profileId, int64_t cutoffMs)
{
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        bridge->mwCommand(AcMw::Icons, AcMw::IconsSweep,
                          QJsonObject{{QLatin1String("profile"), profileId},
                                      {QLatin1String("cutoff_ms"),
                                       qint64(cutoffMs)}});
        return;
    }
    rustCoreEnsureDataDirFor(profileId);
    rc_icons_sweep(cutoffMs, nullptr);
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

HistoryManager::HistoryManager(QObject *parent,
                               const QString &profileId)
    : QObject(parent)
    , m_profileId(BrowserPaths::isProfileId(profileId)
                      ? profileId : BrowserPaths::activeProfileId())
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
#ifdef ARKTOS_RUSTCORE
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
    return instanceFor(BrowserPaths::activeProfileId());
}

static QHash<QString, QPointer<HistoryManager>> &historyManagers()
{
    static QHash<QString, QPointer<HistoryManager>> managers;
    return managers;
}

HistoryManager *HistoryManager::instanceFor(const QString &profileId)
{
    const QString id = BrowserPaths::isProfileId(profileId)
            ? profileId : BrowserPaths::activeProfileId();
    QPointer<HistoryManager> &manager = historyManagers()[id];
    if (!manager)
        manager = new HistoryManager(qApp, id);
    return manager;
}

void HistoryManager::releaseFor(const QString &profileId)
{
    // Strict id check — no ambient-persona aliasing; releasing the
    // wrong persona's store is worse than leaking it.
    if (!BrowserPaths::isProfileId(profileId))
        return;
    delete historyManagers().take(profileId);
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
// it replaces — one small png per host under <profile dir>/icons/.
static QString iconStoreDir(const QString &profileId)
{
    return BrowserPaths::profileDataFilePath(
        profileId, QLatin1String("icons"));
}

#ifndef ARKTOS_RUSTCORE
static QString iconFilePath(const QString &profileId, const QString &host)
{
    return iconStoreDir(profileId) + QLatin1Char('/')
            + QString::fromLatin1(QUrl::toPercentEncoding(host))
            + QLatin1String(".png");
}
#endif

QIcon HistoryManager::icon(const QUrl &url) const
{
#ifdef ARKTOS_RUSTCORE
    const QString key = historyIconKey(url);
    QIcon icon = m_icons.value(key);
    if (icon.isNull()) {
        const QByteArray u = url.toString().toUtf8();
        QByteArray png;
        if (iconGet(m_profileId, u, &png)) {
            QPixmap pixmap;
            if (pixmap.loadFromData(png, "PNG"))
                icon = QIcon(pixmap);
        }
        if (!icon.isNull())
            const_cast<HistoryManager *>(this)->m_icons.insert(key, icon);
    }
#else
    const QString host = url.host().toLower();
    const QString key = host.isEmpty() ? url.toString() : host;
    QIcon icon = m_icons.value(key);
    if (icon.isNull() && !host.isEmpty() && !m_iconMisses.contains(host)) {
        const QPixmap pixmap(iconFilePath(m_profileId, host));
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
#ifdef ARKTOS_RUSTCORE
    const QString key = historyIconKey(url);
    m_icons.insert(key, icon);
    const QImage image = icon.pixmap(QSize(32, 32)).toImage();
    QByteArray png;
    QBuffer buffer(&png);
    if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
        const QByteArray u = url.toString().toUtf8();
        iconPut(m_profileId, u, png);
    }
#else
    const QString key = host.isEmpty() ? url.toString() : host;
    QHash<QString, QIcon>::const_iterator it = m_icons.constFind(key);
    if (it != m_icons.constEnd() && it->cacheKey() == icon.cacheKey())
        return;
    m_icons.insert(key, icon);
    m_iconMisses.remove(key);

    if (!host.isEmpty()) {
        QDir().mkpath(iconStoreDir(m_profileId));
        icon.pixmap(64).save(iconFilePath(m_profileId, host), "PNG");
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
    // The pre-rustcore icons/ dir stays cleared on both paths — the
    // RC16 import leaves it in place and a clearIconDatabase wipe
    // should not leave the old copy behind.
    QDir(iconStoreDir(m_profileId)).removeRecursively();
#ifdef ARKTOS_RUSTCORE
    iconClear(m_profileId);
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
    m_urlNormCounts.clear();
    for (const HistoryEntry &item : m_history)
        noteHistoryUrlAdded(item.url);

    // verify that it is sorted by date
    if (!loadedAndSorted)
        std::sort(m_history.begin(), m_history.end());

    checkForExpired();

#ifdef ARKTOS_RUSTCORE
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
#ifdef ARKTOS_RUSTCORE
    // The bulk-expiry boundary matching the per-item check below: an
    // entry is expired when (its date + daysToExpire at its own
    // time-of-day) is less than a full second ahead of now —
    // secsTo truncates — i.e. visited_at <= this ms boundary (the
    // two forms can differ by an hour only inside a DST transition).
    // A ranged delete also clears duplicate raw rows a per-entry
    // remove would leave behind.
    const qint64 expireCutoffMs =
        QDateTime(now.date().addDays(-m_daysToExpire), now.time())
            .toMSecsSinceEpoch() + 999;
    bool storeExpired = false;
#endif

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
        noteHistoryUrlRemoved(item.url);
        // remove from saved file also
        m_lastSavedUrl.clear();
#ifdef ARKTOS_RUSTCORE
        storeExpired = true;
#endif
        emit entryRemoved(item);
    }
#ifdef ARKTOS_RUSTCORE
    if (storeExpired)
        histExpire(m_profileId, expireCutoffMs);
    // RC16: the icon cache ages on the same retention boundary — a
    // host whose last visit expired loses its cached PNG (the cache
    // is disposable; a live page re-records it on the next
    // iconChanged).
    iconSweep(m_profileId, expireCutoffMs);
#endif

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
    noteHistoryUrlAdded(item.url);
#ifdef ARKTOS_RUSTCORE
    histAdd(m_profileId, item.url, item.title,
            item.dateTime.toMSecsSinceEpoch());
#endif
    emit entryAdded(item);
    if (m_history.count() == 1)
        checkForExpired();
}

void HistoryManager::noteHistoryUrlAdded(const QString &url)
{
    ++m_urlNormCounts[QUrl(url).toString()];
}

void HistoryManager::noteHistoryUrlRemoved(const QString &url)
{
    auto it = m_urlNormCounts.find(QUrl(url).toString());
    if (it != m_urlNormCounts.end() && --it.value() <= 0)
        m_urlNormCounts.erase(it);
}

void HistoryManager::updateHistoryEntry(const QUrl &url, const QString &title)
{
    // CA11/P7: a url with no recorded entry cannot match — skip the
    // O(history) scan (the common case for OTR pages and
    // title-before-add races).  Equal QUrls stringify identically,
    // so the normalized-key miss is exact under the scan's ==.
    if (!m_urlNormCounts.contains(url.toString()))
        return;
    for (int i = 0; i < m_history.count(); ++i) {
        if (url == m_history.at(i).url) {
            m_history[i].title = atomicString(title);
#ifdef ARKTOS_RUSTCORE
            histUpdateTitle(m_profileId, url.toString(),
                            m_history[i].title);
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
    if (m_history.removeOne(item))
        noteHistoryUrlRemoved(item.url);
#ifdef ARKTOS_RUSTCORE
    histRemove(m_profileId, item.url, item.title,
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

bool HistoryManager::confirmClear(QWidget *parent)
{
    if (QMessageBox::question(parent, tr("Clear History"),
            tr("Do you want to clear the history?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return false;
    clear();
    return true;
}

void HistoryManager::clear()
{
    m_history.clear();
    m_atomicStringHash.clear();
    m_urlNormCounts.clear();
    m_lastSavedUrl.clear();
#ifdef ARKTOS_RUSTCORE
    histClear(m_profileId);
#endif
    m_saveTimer->changeOccurred();
    m_saveTimer->saveIfNeccessary();
    emit historyReset();
    emit historyCleared();
}

void HistoryManager::clearSince(qint64 sinceMs)
{
    // The ranged leg of a clear-private-data plan (CLR01): rows at or
    // after the cutoff leave the mirror and the backing store; icons
    // are per-host and stay, matching clear() vs clearIcons().
    m_lastSavedUrl.clear();
    for (int i = m_history.count() - 1; i >= 0; --i) {
        if (m_history.at(i).dateTime.toMSecsSinceEpoch() >= sinceMs) {
            const HistoryEntry item = m_history.takeAt(i);
            noteHistoryUrlRemoved(item.url);
            emit entryRemoved(item);
        }
    }
#ifdef ARKTOS_RUSTCORE
    histClearSince(m_profileId, sinceMs);
#endif
    m_saveTimer->changeOccurred();
    m_saveTimer->saveIfNeccessary();
    emit historyReset();
}

void HistoryManager::loadSettings()
{
    // load settings
    ProfileSettings settings(m_profileId);
    settings.beginGroup(QLatin1String("history"));
    m_daysToExpire = settings.value(QLatin1String("historyLimit"), 30).toInt();
    m_savedDaysToExpire = m_daysToExpire;
}

void HistoryManager::load()
{
    StartupProfile::Scope profileScope("history load");
    loadSettings();

#ifdef ARKTOS_RUSTCORE
    // The canonical store is <profile dir>/history.db (rusqlite); the
    // legacy QDataStream file is imported once, on first run.
    QList<HistoryEntry> list;
    if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
        // MB06: the probe and the batch read ride the envelope — one
        // history.exists path stat, then one history.query answering
        // the whole deduped listing (newest first, the order the flat
        // entry_at loop used to walk).  The probe never opens the
        // database; the query is the open.
        QJsonObject probeErr;
        const QJsonValue probe = bridge->mwQuery(
            AcMw::History, AcMw::HistoryExists,
            QJsonObject{{QStringLiteral("profile"), m_profileId}},
            &probeErr);
        const bool fresh =
            !probe.toObject().value(QLatin1String("exists")).toBool();
        if (fresh)
            importLegacyHistory();
        QJsonObject readErr;
        const QJsonValue rows = bridge->mwQuery(
            AcMw::History, AcMw::HistoryQuery,
            QJsonObject{{QStringLiteral("profile"), m_profileId},
                        {QStringLiteral("spec"), QJsonObject{}}},
            &readErr);
        if (rows.isUndefined()) {
            qWarning() << "HistoryManager: cannot open history.db:"
                       << readErr.value(QLatin1String("message"))
                              .toString();
            return;
        }
        const QJsonArray entries = rows.toArray();
        list.reserve(entries.size());
        for (const QJsonValue &v : entries) {
            const QJsonObject o = v.toObject();
            list.append(HistoryEntry(
                atomicString(o.value(QLatin1String("url")).toString()),
                QDateTime::fromMSecsSinceEpoch(
                    o.value(QLatin1String("ts")).toInteger()),
                o.value(QLatin1String("title")).toString()));
        }
    } else {
        const QByteArray dbPath = BrowserPaths::profileDataFilePath(
            m_profileId, QLatin1String("history.db")).toUtf8();
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
    }
    setHistory(list, true);
    return;
#else
    QFile historyFile(BrowserPaths::profileDataFilePath(
        m_profileId, QLatin1String("history")));

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

#ifdef ARKTOS_RUSTCORE
// One-shot migration: the legacy QDataStream "history" file is parsed
// by the existing reader and written into history.db, which becomes
// the canonical store from then on (the old file is left in place).
void HistoryManager::importLegacyHistory()
{
    QFile historyFile(BrowserPaths::profileDataFilePath(
        m_profileId, QLatin1String("history")));
    if (!historyFile.exists() || !historyFile.open(QFile::ReadOnly))
        return;
    QDataStream in(&historyFile);
    const HistoryParser::Result parsed =
            HistoryParser::readEntries(in, m_atomicStringHash);
    // Oldest first: the store orders equal timestamps by insert seq.
    for (int i = parsed.entries.count() - 1; i >= 0; --i) {
        const HistoryEntry &e = parsed.entries.at(i);
        histAdd(m_profileId, e.url, e.title,
                e.dateTime.toMSecsSinceEpoch());
    }
}

// Rebuilds the store to mirror m_history exactly — setHistory() is
// the test/import seam that wholesale-replaces the list.
void HistoryManager::rustReplaceAll()
{
    histClear(m_profileId);
    for (int i = m_history.count() - 1; i >= 0; --i) {
        const HistoryEntry &e = m_history.at(i);
        histAdd(m_profileId, e.url, e.title,
                e.dateTime.toMSecsSinceEpoch());
    }
}
#endif // ARKTOS_RUSTCORE

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
    // CA11/P9: the limit is quasi-static — only open+write settings
    // when it actually moved, not on every debounced flush.
    if (m_savedDaysToExpire != m_daysToExpire) {
        ProfileSettings settings(m_profileId);
        settings.beginGroup(QLatin1String("history"));
        settings.setValue(QLatin1String("historyLimit"), m_daysToExpire);
        m_savedDaysToExpire = m_daysToExpire;
    }

#ifdef ARKTOS_RUSTCORE
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

    QFile historyFile(BrowserPaths::profileDataFilePath(
        m_profileId, QLatin1String("history")));

    // When saving everything use a temporary file to prevent possible data loss.
    // The temp file must live in the target's directory: a default
    // QTemporaryFile lands on /tmp and /tmp -> data dir is a cross-device
    // rename (EXDEV) inside flatpak (and on any tmpfs-/tmp host), which
    // silently drops the whole history save.
    QTemporaryFile tempFile(historyFile.fileName()
                            + QLatin1String(".XXXXXX"));
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