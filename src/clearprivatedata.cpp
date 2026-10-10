/*
 * Copyright 2008-2009 Jason A. Donenfeld <Jason@zx2c4.com>
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

#include "clearprivatedata.h"

#include "browserapplication.h"
#include "browserprofile.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "historymanager.h"
#include "networkaccessmanager.h"
#include "toolbarsearch.h"

#include <qabstractnetworkcache.h>
#include <qapplication.h>
#include <qcheckbox.h>
#include <qdatetime.h>
#include <qdialogbuttonbox.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlist.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebengineview.h>

#if defined(ARORA_RUSTCORE)
#include "rustcore.h"
#include "rustcorebridge.h"

// The category enum and the RC_CLEAR_* bits are one vocabulary
// maintained in two places — pin the values together.
static_assert(ClearPrivateData::ClearHistory == RC_CLEAR_HISTORY);
static_assert(ClearPrivateData::ClearDownloads == RC_CLEAR_DOWNLOADS);
static_assert(ClearPrivateData::ClearSearch == RC_CLEAR_SEARCH);
static_assert(ClearPrivateData::ClearCookies == RC_CLEAR_COOKIES);
static_assert(ClearPrivateData::ClearSiteData == RC_CLEAR_SITEDATA);
static_assert(ClearPrivateData::ClearCache == RC_CLEAR_CACHE);
static_assert(ClearPrivateData::ClearIcons == RC_CLEAR_ICONS);
static_assert(ClearPrivateData::ClearSession == RC_CLEAR_SESSION);
static_assert(ClearPrivateData::ClearFullWipe == RC_CLEAR_FULL_WIPE);
#endif

ClearPrivateData::ClearPrivateData(QWidget *parent)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint)
{
    setWindowTitle(tr("Clear Private Data"));

    QVBoxLayout *layout = new QVBoxLayout();
    layout->addWidget(new QLabel(tr("Clear the following items:")));

    // UIP01: indent the item list under the heading and pin the button
    // box to the bottom edge.
    QVBoxLayout *itemLayout = new QVBoxLayout();
    itemLayout->setContentsMargins(16, 0, 0, 0);
    layout->addLayout(itemLayout);

    QSettings settings;
    settings.beginGroup(QLatin1String("clearprivatedata"));

    m_browsingHistory = new QCheckBox(tr("&Browsing History"));
    m_browsingHistory->setChecked(settings.value(QLatin1String("browsingHistory"), true).toBool());
    itemLayout->addWidget(m_browsingHistory);

    m_downloadHistory = new QCheckBox(tr("&Download History"));
    m_downloadHistory->setChecked(settings.value(QLatin1String("downloadHistory"), true).toBool());
    itemLayout->addWidget(m_downloadHistory);

    m_searchHistory = new QCheckBox(tr("&Search History"));
    m_searchHistory->setChecked(settings.value(QLatin1String("searchHistory"), true).toBool());
    itemLayout->addWidget(m_searchHistory);

    m_cookies = new QCheckBox(tr("&Cookies"));
    m_cookies->setChecked(settings.value(QLatin1String("cookies"), true).toBool());
    itemLayout->addWidget(m_cookies);

    // DOM storage — the data trackers actually use.  The web cache
    // checkbox cannot cover it: clearHttpCache() never touches
    // localStorage/IndexedDB/service workers.
    m_siteData = new QCheckBox(tr("Site &Data"));
    m_siteData->setToolTip(tr("localStorage, IndexedDB, service workers and other site databases"));
    m_siteData->setChecked(settings.value(QLatin1String("siteData"), true).toBool());
    itemLayout->addWidget(m_siteData);

    // The web cache lives inside the profile now (Chromium's http
    // cache), so this stays enabled even when the app-side NAM disk
    // cache is off.
    m_cache = new QCheckBox(tr("C&ached Web Pages"));
    m_cache->setChecked(settings.value(QLatin1String("cache"), true).toBool());
    itemLayout->addWidget(m_cache);

    m_favIcons = new QCheckBox(tr("Website &Icons"));
    m_favIcons->setChecked(settings.value(QLatin1String("favIcons"), true).toBool());
    itemLayout->addWidget(m_favIcons);

    settings.endGroup();

    QPushButton *acceptButton = new QPushButton(tr("Clear &Private Data"));
    acceptButton->setDefault(true);
    QPushButton *rejectButton = new QPushButton(tr("&Cancel"));
    QDialogButtonBox *buttonBox = new QDialogButtonBox;
    buttonBox->addButton(acceptButton, QDialogButtonBox::AcceptRole);
    buttonBox->addButton(rejectButton, QDialogButtonBox::RejectRole);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &ClearPrivateData::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &ClearPrivateData::reject);
    layout->addStretch(1);
    layout->addWidget(buttonBox);

    setLayout(layout);
    acceptButton->setFocus(Qt::OtherFocusReason);
}

// One operation row in the plan JSON — see rustcore.h's CLR01
// section for the vocabulary.  `scope` ("persistent"/"otr") selects
// the profile set the op runs against; `sinceSecs` is the requested
// window (0 = whole store); `ranged` marks the stores that can honor
// a window.  A ranged request on a ranged-incapable store is flagged
// "degraded" and executes as a full wipe — over-clearing is the
// honest direction for a privacy wipe.
static QJsonObject clearPlanOp(const QString &name, const QString &scope,
                               qint64 sinceSecs, bool ranged)
{
    QJsonObject op;
    op.insert(QLatin1String("op"), name);
    op.insert(QLatin1String("since_secs"), sinceSecs);
    op.insert(QLatin1String("ranged"), ranged);
    if (!scope.isEmpty())
        op.insert(QLatin1String("scope"), scope);
    if (sinceSecs > 0 && !ranged)
        op.insert(QLatin1String("degraded"), true);
    return op;
}

// The window a period name maps to, or -1 for an unrecognized name —
// the same vocabulary rustcore's parse_period() accepts.
static qint64 clearPlanPeriodSecs(const QString &period)
{
    const QString p = period.trimmed().toLower();
    if (p.isEmpty() || p == QLatin1String("all")
        || p == QLatin1String("everything"))
        return 0;
    if (p == QLatin1String("hour") || p == QLatin1String("last-hour"))
        return 3600;
    if (p == QLatin1String("day") || p == QLatin1String("today")
        || p == QLatin1String("last-day"))
        return 86400;
    if (p == QLatin1String("week") || p == QLatin1String("last-week"))
        return 604800;
    return -1;
}

QJsonArray ClearPrivateData::plan(const QString &period, quint32 categories, bool otr)
{
#if defined(ARORA_RUSTCORE)
    // CLR01: the decision lives in rustcore — the Qt transliteration
    // below is byte-identical (the autotest corpus proves it) and
    // serves as the no-rust implementation, the FFI-failure fallback
    // and the parity-test seam.
    const QByteArray p = period.toUtf8();
    if (char *out = rc_clearplan(p.constData(), categories, otr ? 1 : 0)) {
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(out));
        rc_string_free(out);
        if (doc.isObject())
            return doc.object().value(QLatin1String("ops")).toArray();
    }
    // Unknown period or an FFI hiccup: rc_clearplan returned NULL —
    // fall through; planQt() maps an unknown period to an empty plan
    // the same way the crate's InvalidArgument maps to no work.
#endif
    return planQt(period, categories, otr);
}

QJsonArray ClearPrivateData::planQt(const QString &period, quint32 categories, bool otr)
{
    const qint64 sinceSecs = clearPlanPeriodSecs(period);
    if (sinceSecs < 0)
        return {};

    QJsonArray ops;

    if (otr) {
        // The off-the-record plan touches only state the private
        // context owns: its in-memory visited links, cookies, DOM
        // storage and cache, plus the private download rows.  The
        // persistent stores an OTR session never wrote — history,
        // icons, search history, sessions, container trees, the disk
        // cache — get no op, so a private-window clear can never
        // reach the profile it shares the process with.
        const QString otrScope = QLatin1String("otr");
        if (categories & ClearHistory)
            ops.append(clearPlanOp(QLatin1String("visited_links.clear"), otrScope, sinceSecs, false));
        if (categories & ClearDownloads)
            ops.append(clearPlanOp(QLatin1String("downloads.clear_otr"), QString(), sinceSecs, false));
        if (categories & ClearCookies)
            ops.append(clearPlanOp(QLatin1String("cookies.clear"), otrScope, sinceSecs, false));
        if (categories & ClearSiteData)
            ops.append(clearPlanOp(QLatin1String("sitedata.page_sweep"), otrScope, sinceSecs, false));
        if (categories & ClearCache)
            ops.append(clearPlanOp(QLatin1String("cache.clear"), otrScope, sinceSecs, false));
        return ops;
    }

    const QString persistent = QLatin1String("persistent");

    // CONT05: registered containers whose profile was never
    // materialized have no live profile to sweep — their on-disk
    // trees are wiped directly, gated by the same category flags.
    if (categories & (ClearCookies | ClearSiteData | ClearCache | ClearHistory)) {
        QJsonObject c = clearPlanOp(QLatin1String("containers.unmaterialized"), QString(), sinceSecs, false);
        c.insert(QLatin1String("cookies"), bool(categories & ClearCookies));
        c.insert(QLatin1String("sitedata"), bool(categories & ClearSiteData));
        c.insert(QLatin1String("cache"), bool(categories & ClearCache));
        c.insert(QLatin1String("visited_links"), bool(categories & ClearHistory));
        ops.append(c);
    }
    if (categories & ClearHistory) {
        // The visit log is the one ranged-capable store.
        ops.append(clearPlanOp(QLatin1String("history.clear"), QString(), sinceSecs, true));
        // Chromium's visited-link database — all-or-nothing.
        ops.append(clearPlanOp(QLatin1String("visited_links.clear"), persistent, sinceSecs, false));
    }
    if (categories & ClearDownloads)
        ops.append(clearPlanOp(QLatin1String("downloads.clear"), QString(), sinceSecs, false));
    if (categories & ClearSearch)
        ops.append(clearPlanOp(QLatin1String("search.clear"), QString(), sinceSecs, false));
    if (categories & ClearCookies)
        ops.append(clearPlanOp(QLatin1String("cookies.clear"), persistent, sinceSecs, false));
    if (categories & ClearSiteData) {
        // Live origins must be emptied before the deferred disk
        // wipe, or in-memory storage re-flushes over the clear —
        // the op order is load-bearing.
        ops.append(clearPlanOp(QLatin1String("sitedata.page_sweep"), persistent, sinceSecs, false));
        ops.append(clearPlanOp(QLatin1String("sitedata.defer_wipe"), persistent, sinceSecs, false));
    }
    if (categories & ClearCache) {
        ops.append(clearPlanOp(QLatin1String("cache.clear"), persistent, sinceSecs, false));
        // The app-side fetch cache (opensearch, adblock lists).
        ops.append(clearPlanOp(QLatin1String("nam_cache.clear"), QString(), sinceSecs, false));
    }
    if (categories & ClearIcons)
        ops.append(clearPlanOp(QLatin1String("icons.clear"), QString(), sinceSecs, false));
    if (categories & ClearSession)
        // The saved session blob is itself browsing history.
        ops.append(clearPlanOp(QLatin1String("session.clear"), QString(), sinceSecs, false));
    if (categories & ClearFullWipe) {
        // PRIV01: the exit wipe additionally schedules the Network/
        // tree + cache dirs for removal at next start (the async
        // profile deletes may not flush before exit) and removes
        // whole trees of unmaterialized containers.
        ops.append(clearPlanOp(QLatin1String("storage.defer_full_wipe"), persistent, sinceSecs, false));
        ops.append(clearPlanOp(QLatin1String("containers.wipe_unmaterialized"), QString(), sinceSecs, false));
    }
    return ops;
}

// Resolves the profile set a scoped op runs against.  "persistent"
// is the normal profile plus every materialized container profile —
// the off-the-record profile is deliberately absent so a persistent
// clear never reaches into a live private session.  "otr" is the
// private context's profile, but only when it already exists — a
// clear must not conjure a profile into being just to empty it.
static QList<QWebEngineProfile*> clearPlanProfiles(const QString &scope)
{
    QList<QWebEngineProfile*> profiles;
    if (scope == QLatin1String("otr")) {
        QWebEngineProfile *otrProfile = nullptr;
        if (BrowserApplication::isTorMode())
            otrProfile = BrowserProfile::torProfile();
        else
            otrProfile = BrowserProfile::privateProfileIfCreated();
        if (otrProfile)
            profiles.append(otrProfile);
        return profiles;
    }
    profiles.append(BrowserProfile::normalProfile());
    profiles.append(ContainerManager::instance()->createdProfiles());
    return profiles;
}

// The live DOM-storage sweep shared by both scopes: Chromium caches
// live origins' storage in the browser process, so every open page
// of the scope's profiles is emptied first — a cleared area cannot
// be re-flushed.  (HARD01: deleting the trees under a running
// browser wedges its storage services, which is why the disk wipe
// is a separate deferred op.)
static void clearPlanSiteDataSweep(const QString &scope)
{
    const QString wipeScript = QStringLiteral(
        "try{localStorage.clear()}catch(e){}"
        "try{sessionStorage.clear()}catch(e){}"
        "try{if(window.indexedDB&&indexedDB.databases)"
        "indexedDB.databases().then(function(dbs){dbs.forEach("
        "function(db){indexedDB.deleteDatabase(db.name)})})}catch(e){}"
        "try{if(navigator.serviceWorker&&navigator.serviceWorker.getRegistrations)"
        "navigator.serviceWorker.getRegistrations().then(function(rs){"
        "rs.forEach(function(r){r.unregister()})})}catch(e){}"
        "try{if(window.caches&&caches.keys)"
        "caches.keys().then(function(ns){ns.forEach(function(n){caches.delete(n)})})}catch(e){}");
    const QList<QWebEngineProfile*> profiles = clearPlanProfiles(scope);
    const QSet<QWebEngineProfile*> profileSet(profiles.begin(), profiles.end());
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        QWebEngineView *view = qobject_cast<QWebEngineView*>(widget);
        if (view && view->page() && profileSet.contains(view->page()->profile()))
            view->page()->runJavaScript(wipeScript);
    }
}

void ClearPrivateData::executePlan(const QJsonArray &ops)
{
    for (const QJsonValue &value : ops) {
        const QJsonObject op = value.toObject();
        const QString name = op.value(QLatin1String("op")).toString();
        const QString scope = op.value(QLatin1String("scope")).toString();
        const qint64 sinceSecs = op.value(QLatin1String("since_secs")).toInteger();
        const bool ranged = op.value(QLatin1String("ranged")).toBool();

        if (name == QLatin1String("containers.unmaterialized")) {
            // CONT05: disk trees of never-materialized containers.
            ContainerManager::instance()->clearUnmaterializedStorage(
                op.value(QLatin1String("cookies")).toBool(),
                op.value(QLatin1String("sitedata")).toBool(),
                op.value(QLatin1String("cache")).toBool(),
                op.value(QLatin1String("visited_links")).toBool());
        } else if (name == QLatin1String("history.clear")) {
            // The visit log is the one ranged-capable store — a
            // windowed request deletes only rows at/after the cutoff.
            if (ranged && sinceSecs > 0) {
                HistoryManager::instance()->clearSince(
                    QDateTime::currentMSecsSinceEpoch() - sinceSecs * 1000);
            } else {
                HistoryManager::instance()->clear();
            }
        } else if (name == QLatin1String("visited_links.clear")) {
            // Chromium keeps its own visited-link database (the
            // :visited styling and omnibox history) — clear it too
            // or links would keep rendering as visited.
            for (QWebEngineProfile *profile : clearPlanProfiles(scope))
                profile->clearAllVisitedLinks();
        } else if (name == QLatin1String("downloads.clear")) {
            DownloadManager::instance()->cleanup();
            DownloadManager::instance()->hide();
        } else if (name == QLatin1String("downloads.clear_otr")) {
            DownloadManager::instance()->cleanupOffTheRecord();
        } else if (name == QLatin1String("search.clear")) {
            // Every live ToolbarSearch clears its own recent-search
            // list.
            const QWidgetList widgets = qApp->allWidgets();
            for (QWidget *widget : widgets) {
                if (ToolbarSearch *search = qobject_cast<ToolbarSearch*>(widget))
                    search->clear();
            }
        } else if (name == QLatin1String("cookies.clear")) {
            for (QWebEngineProfile *profile : clearPlanProfiles(scope))
                CookieJar::instance(profile)->clear();
        } else if (name == QLatin1String("sitedata.page_sweep")) {
            clearPlanSiteDataSweep(scope);
        } else if (name == QLatin1String("sitedata.defer_wipe")) {
            // The on-disk storage trees are removed at the next
            // profile start — deleting them under the live browser
            // wedges Chromium's storage services.
            for (QWebEngineProfile *profile : clearPlanProfiles(scope)) {
                if (!profile->isOffTheRecord())
                    BrowserProfile::clearSiteStorage(profile);
            }
        } else if (name == QLatin1String("cache.clear")) {
            for (QWebEngineProfile *profile : clearPlanProfiles(scope))
                profile->clearHttpCache();
        } else if (name == QLatin1String("nam_cache.clear")) {
            // The app-side fetch cache (opensearch, adblock lists).
            if (QAbstractNetworkCache *cache = NetworkAccessManager::instance()->cache())
                cache->clear();
        } else if (name == QLatin1String("icons.clear")) {
            // The app-side icon store lives on the HistoryManager.
            HistoryManager::instance()->clearIcons();
        } else if (name == QLatin1String("session.clear")) {
            // The saved session blob is browsing history — leaving
            // it behind would undo the wipe.
            QSettings settings;
            settings.beginGroup(QLatin1String("sessions"));
            settings.remove(QLatin1String("lastSession"));
            settings.endGroup();
#if defined(ARORA_RUSTCORE)
            rustCoreEnsureDataDir();
            rc_session_clear();
#endif
        } else if (name == QLatin1String("storage.defer_full_wipe")) {
            // PRIV01 sentinel: additionally removes the Network/ tree
            // (cookie database, HSTS) and cache dirs at next start.
            for (QWebEngineProfile *profile : clearPlanProfiles(scope)) {
                if (!profile->isOffTheRecord())
                    BrowserProfile::clearAllStorageOnNextStart(profile);
            }
        } else if (name == QLatin1String("containers.wipe_unmaterialized")) {
            ContainerManager::instance()->wipeUnmaterializedStorage();
        } else {
            // Forward compatibility: a newer plan on an older
            // executor skips unknown ops rather than guessing.
            qWarning() << "ClearPrivateData: unknown plan op" << name;
        }
    }
}

void ClearPrivateData::accept()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("clearprivatedata"));

    settings.setValue(QLatin1String("browsingHistory"), m_browsingHistory->isChecked());
    settings.setValue(QLatin1String("downloadHistory"), m_downloadHistory->isChecked());
    settings.setValue(QLatin1String("searchHistory"), m_searchHistory->isChecked());
    settings.setValue(QLatin1String("cookies"), m_cookies->isChecked());
    settings.setValue(QLatin1String("siteData"), m_siteData->isChecked());
    settings.setValue(QLatin1String("cache"), m_cache->isChecked());
    settings.setValue(QLatin1String("favIcons"), m_favIcons->isChecked());

    settings.endGroup();

    quint32 categories = 0;
    if (m_browsingHistory->isChecked())
        categories |= ClearHistory;
    if (m_downloadHistory->isChecked())
        categories |= ClearDownloads;
    if (m_searchHistory->isChecked())
        categories |= ClearSearch;
    if (m_cookies->isChecked())
        categories |= ClearCookies;
    if (m_siteData->isChecked())
        categories |= ClearSiteData;
    if (m_cache->isChecked())
        categories |= ClearCache;
    if (m_favIcons->isChecked())
        categories |= ClearIcons;

    // CLR01: the "which stores get wiped" decision is data produced
    // by the planner (rustcore when built in) and executed op-by-op;
    // a private session scopes the plan to its own in-memory state —
    // the persistent profile's stores are absent from it entirely.
    executePlan(plan(QLatin1String("all"), categories,
                     BrowserApplication::isPrivate()));
    QDialog::accept();
}
