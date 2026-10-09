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
#include <qdialogbuttonbox.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlist.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebengineview.h>

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

    // The clear claims cover every browsing profile: the normal
    // profile plus any materialized container profiles — clearing
    // "cookies" while containers kept theirs would quietly preserve
    // the very data the user asked to remove (CONT01).
    QList<QWebEngineProfile*> profiles;
    profiles.append(BrowserProfile::normalProfile());
    profiles.append(ContainerManager::instance()->createdProfiles());

    // CONT05: a container whose profile was never materialized this
    // session keeps its cookies/storage/cache on disk untouched by
    // every per-profile clear below — wipe the same classes directly.
    // Nothing holds those trees open, so removal is immediate.
    ContainerManager::instance()->clearUnmaterializedStorage(
        m_cookies->isChecked(), m_siteData->isChecked(),
        m_cache->isChecked(), m_browsingHistory->isChecked());

    if (m_browsingHistory->isChecked()) {
        HistoryManager::instance()->clear();
        // Chromium keeps its own visited-link database (the :visited
        // styling and Omnibox history) — clear it too or links would
        // keep rendering as visited.
        for (QWebEngineProfile *profile : profiles)
            profile->clearAllVisitedLinks();
    }

    if (m_downloadHistory->isChecked()) {
        DownloadManager::instance()->cleanup();
    }

    if (m_searchHistory->isChecked()) {
        // Was a BrowserApplication::mainWindows() loop reaching each
        // window's ToolbarSearch; every live ToolbarSearch clears its
        // own recent-search list.
        const QWidgetList widgets = qApp->allWidgets();
        for (QWidget *widget : widgets) {
            if (ToolbarSearch *search = qobject_cast<ToolbarSearch*>(widget))
                search->clear();
        }
    }

    if (m_cookies->isChecked()) {
        for (QWebEngineProfile *profile : profiles)
            CookieJar::instance(profile)->clear();
    }

    if (m_siteData->isChecked()) {
        // Chromium caches live origins' DOM storage in the browser
        // process, so sweep every open page of this profile first —
        // an emptied storage area cannot be re-flushed to disk.
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
        const QSet<QWebEngineProfile*> profileSet(profiles.begin(), profiles.end());
        const QWidgetList widgets = qApp->allWidgets();
        for (QWidget *widget : widgets) {
            QWebEngineView *view = qobject_cast<QWebEngineView*>(widget);
            if (view && view->page() && profileSet.contains(view->page()->profile()))
                view->page()->runJavaScript(wipeScript);
        }
        // Then schedule the on-disk storage trees for removal at the
        // next profile start — deleting them under the live browser
        // wedges Chromium's storage services (see browserprofile.cpp).
        for (QWebEngineProfile *profile : profiles)
            BrowserProfile::clearSiteStorage(profile);
    }

    if (m_cache->isChecked()) {
        for (QWebEngineProfile *profile : profiles)
            profile->clearHttpCache();
        // The app-side fetch cache (opensearch, adblock lists) too.
        if (QAbstractNetworkCache *cache = NetworkAccessManager::instance()->cache())
            cache->clear();
    }

    if (m_favIcons->isChecked()) {
        // QWebSettings::clearIconDatabase() is gone; the app-side icon
        // store lives on the HistoryManager now.
        HistoryManager::instance()->clearIcons();
    }
    QDialog::accept();
}
