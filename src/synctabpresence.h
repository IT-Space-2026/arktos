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

#ifndef SYNCTABPRESENCE_H
#define SYNCTABPRESENCE_H

#include <qhash.h>
#include <qmenu.h>
#include <qobject.h>
#include <qurl.h>

class BrowserMainWindow;
class QTimer;

// SYNC06: publishes this device's open tab set to the sync group as
// an ephemeral per-device presence doc (the "tabs" store — newest
// doc wins, never a merge).  watchWindow() hooks each browser
// window's tab signals into a shared debounce; a pass collects the
// tab set per profile scope and publishes ONLY the scopes that
// changed and have at least one paired member — an unpaired device
// would just be writing to itself.  Off-the-record tabs never
// appear (TabWidget::presenceWindowJson drops them) and a private-
// mode run publishes nothing at all.
class SyncTabPresence : public QObject
{
    Q_OBJECT

public:
    // Leaky app-lifetime singleton, minted on the first watched
    // window — call only when a BrowserApplication exists.
    static SyncTabPresence *instance();

    void watchWindow(BrowserMainWindow *window);
    // Debounced scheduling — every tab-set change funnels here.
    void schedule();

private slots:
    void publishNow();

private:
    explicit SyncTabPresence(QObject *parent = nullptr);

    // Scopes the last pass could publish to (paired members), so a
    // scope whose windows all closed still gets its wipe doc out.
    bool scopeHasMembers(const QString &scope,
                         const QList<QPair<QString, QStringList> > &members) const;

    QTimer *m_timer;
    bool m_bridgeHooked = false;
    // scope -> compact JSON of the last published windows array —
    // the change detector that keeps the op-log free of churn.
    QHash<QString, QByteArray> m_lastDocs;
};

// SYNC06: the read side — the History menu's "Tabs from Other
// Devices" submenu.  Rebuilt on aboutToShow from the tabs_peers
// mw query: one submenu per peer device, one action per published
// tab.  Read-only BY DESIGN — the menu shows a peer's open tabs;
// activating one is an ordinary local navigation (openUrl), never
// a write back into the sync lane.
class SyncTabsMenu : public QMenu
{
    Q_OBJECT

public:
    explicit SyncTabsMenu(const QString &profileId,
                          QWidget *parent = nullptr);

signals:
    void openUrl(const QUrl &url);

private slots:
    void rebuild();

private:
    QString m_profileId;
};

#endif // SYNCTABPRESENCE_H
