/*
 * Copyright 2026 Arktos Browser - IT-Space
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
 */

#include "synctabpresence.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "rustcorebridge.h"
#include "tabwidget.h"

#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qmap.h>
#include <qtimer.h>

SyncTabPresence::SyncTabPresence(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    m_timer->setInterval(1500);
    connect(m_timer, &QTimer::timeout,
            this, &SyncTabPresence::publishNow);
}

SyncTabPresence *SyncTabPresence::instance()
{
    static SyncTabPresence *s_instance = new SyncTabPresence;
    return s_instance;
}

void SyncTabPresence::watchWindow(BrowserMainWindow *window)
{
    if (!window || !window->tabWidget())
        return;
    TabWidget *tabs = window->tabWidget();
    // Anything that can change what a peer should see funnels into
    // the shared debounce: add/close/move, title churn, load settle
    // and the window going away entirely.
    connect(tabs, &TabWidget::tabsChanged,
            this, &SyncTabPresence::schedule);
    connect(tabs, &TabWidget::setCurrentTitle,
            this, &SyncTabPresence::schedule);
    connect(tabs, &TabWidget::loadFinished,
            this, &SyncTabPresence::schedule);
    connect(tabs, &TabWidget::lastTabClosed,
            this, &SyncTabPresence::schedule);
    connect(window, &QObject::destroyed,
            this, &SyncTabPresence::schedule);
    schedule();
}

void SyncTabPresence::schedule()
{
    m_timer->start();
}

bool SyncTabPresence::scopeHasMembers(
    const QString &scope,
    const QList<QPair<QString, QStringList> > &members) const
{
    for (const auto &m : members) {
        // A member with no scope rows is device-global (pre-SYNC03
        // pairings); a scoped one sees only its listed groups.
        if (m.second.isEmpty() || m.second.contains(scope))
            return true;
    }
    return false;
}

void SyncTabPresence::publishNow()
{
    BrowserApplication *app = BrowserApplication::instance();
    if (!app || BrowserApplication::isPrivate())
        return;
    RustCoreBridge *bridge = RustCoreBridge::serving();
    if (!bridge)
        return;
    // A completed pairing lands as a "sync" store change — republish
    // so a device that was idle at pair time still advertises its
    // tabs without waiting for the next local edit.
    if (!m_bridgeHooked) {
        m_bridgeHooked = true;
        connect(bridge, &RustCoreBridge::storeChanged,
                this, [this](const QString &topic) {
            if (topic == QLatin1String("sync"))
                schedule();
        });
    }

    QJsonObject err;
    const QJsonValue mv = bridge->mwQuery(
        AcMw::Sync, AcMw::SyncMembersList, QJsonObject(), &err);
    QList<QPair<QString, QStringList> > members;
    for (const QJsonValue &m
         : mv.toObject().value(QStringLiteral("members")).toArray()) {
        const QJsonObject mo = m.toObject();
        QStringList scopes;
        for (const QJsonValue &s
             : mo.value(QStringLiteral("scopes")).toArray())
            scopes << s.toString();
        members.append(qMakePair(
            mo.value(QStringLiteral("device_id")).toString(), scopes));
    }

    // The doc is per profile scope: each window contributes one
    // "windows" entry to ITS persona's group.
    QMap<QString, QJsonArray> byScope;
    const QList<BrowserMainWindow*> windows = app->mainWindows();
    for (BrowserMainWindow *window : windows) {
        if (!window || !window->tabWidget())
            continue;
        const QJsonObject w = window->tabWidget()->presenceWindowJson();
        if (w.value(QStringLiteral("tabs")).toArray().isEmpty())
            continue;
        byScope[window->profileId()].append(w);
    }

    // Publish each changed scope that has a paired member — plus a
    // wipe for a scope we last advertised but whose windows are all
    // gone (the empty array is how "everything closed" propagates).
    QStringList scopes = byScope.keys();
    for (const QString &scope : m_lastDocs.keys()) {
        if (!scopes.contains(scope))
            scopes.append(scope);
    }
    for (const QString &scope : scopes) {
        const QJsonArray wins = byScope.value(scope);
        const QByteArray canon =
            QJsonDocument(wins).toJson(QJsonDocument::Compact);
        if (m_lastDocs.value(scope) == canon)
            continue;
        if (wins.isEmpty() && !m_lastDocs.contains(scope))
            continue;   // never advertised — nothing to retract
        if (!scopeHasMembers(scope, members))
            continue;   // no paired member can see this group yet
        const QJsonValue r = bridge->mwCommand(
            AcMw::Sync, AcMw::SyncTabsPublish,
            QJsonObject{{QStringLiteral("profile"), scope},
                        {QStringLiteral("windows"), wins}},
            &err);
        if (r.toObject().value(QStringLiteral("ok")).toBool())
            m_lastDocs.insert(scope, canon);
    }
}


SyncTabsMenu::SyncTabsMenu(const QString &profileId, QWidget *parent)
    : QMenu(parent)
    , m_profileId(profileId)
{
    setTitle(tr("Tabs from Other Devices"));
    connect(this, &QMenu::aboutToShow, this, &SyncTabsMenu::rebuild);
}

void SyncTabsMenu::rebuild()
{
    clear();
    RustCoreBridge *bridge = RustCoreBridge::serving();
    if (!bridge) {
        addAction(tr("Sync is not available"))->setEnabled(false);
        return;
    }
    QJsonObject err;
    const QJsonValue v = bridge->mwQuery(
        AcMw::Sync, AcMw::SyncTabsPeers,
        QJsonObject{{QStringLiteral("profile"), m_profileId}}, &err);
    const QJsonArray devices =
        v.toObject().value(QStringLiteral("devices")).toArray();
    if (devices.isEmpty()) {
        addAction(tr("No tabs on other devices"))->setEnabled(false);
        return;
    }
    for (const QJsonValue &dv : devices) {
        const QJsonObject dev = dv.toObject();
        const QString id =
            dev.value(QStringLiteral("device_id")).toString();
        QMenu *devMenu = addMenu(
            tr("Tabs on %1")
                .arg(id.left(16) + QLatin1String("…")));
        const QJsonArray wins =
            dev.value(QStringLiteral("windows")).toArray();
        const bool multiWindow = wins.count() > 1;
        int windowNo = 0;
        bool any = false;
        for (const QJsonValue &wv : wins) {
            ++windowNo;
            const QJsonArray tabs =
                wv.toObject().value(QStringLiteral("tabs")).toArray();
            if (multiWindow && !tabs.isEmpty())
                devMenu->addAction(tr("Window %1").arg(windowNo))
                    ->setEnabled(false);
            for (const QJsonValue &tv : tabs) {
                const QJsonObject tab = tv.toObject();
                const QString url =
                    tab.value(QStringLiteral("url")).toString();
                if (url.isEmpty())
                    continue;
                QString title =
                    tab.value(QStringLiteral("title")).toString();
                if (title.isEmpty())
                    title = url;
                QAction *a = devMenu->addAction(title);
                a->setToolTip(url);
                const QUrl target = QUrl::fromEncoded(url.toUtf8());
                connect(a, &QAction::triggered,
                        this, [this, target]() {
                    emit openUrl(target);
                });
                any = true;
            }
        }
        if (!any)
            devMenu->addAction(tr("(no open tabs)"))->setEnabled(false);
    }
}
