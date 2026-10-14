/*
 * Copyright 2026 The Arora Authors
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

#ifndef EXTENSIONSHIMBACKEND_H
#define EXTENSIONSHIMBACKEND_H

#include <qhash.h>
#include <qjsonobject.h>
#include <qlist.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>

class QTimer;
class QWebEngineProfile;

// EXT15 — host-side backend behind ChromeBridge::invoke() for the
// chrome.* gapfill shim (src/data/extshim.js).  One instance per
// hosted extension background context.  Every namespace the page
// can reach answers either a real result or a typed
// {ok:false,error:"..."} reply — the shim turns the error into a
// recorded health gap; nothing silently no-ops.
//
// What lives here and why:
//   storage.*    — per-extension JSON store under
//                  <data>/extstorage/<id>/ (local persists, session
//                  is in-memory); the page keeps real chrome.storage
//                  when present, this backs contexts without bindings
//   alarms.*     — QTimer-backed alarms so onAlarm really fires (the
//                  hosted page's binding is present-but-inert)
//   tabs.*       — query/get/create/update over a live
//                  BrowserMainWindow/TabWidget snapshot; ids are
//                  session-stable via the registry below
//   action.*     — title/badge/popup setters feeding the action-
//                  state registry EXT08's button strip reads
//   i18n.*       — _locales/<lang>/messages.json lookup
//   permissions.*— manifest-declared contains/getAll fallback
//   contextMenus.*— per-extension item registry (EXT16 renders)
//   sidePanel.*  — options recorded (Arora has no side-panel surface)
//   notifications.* — typed 'unsupported' (no notification presenter)
//   runtime.*    — context bookkeeping (getViews counts)
//   shim.*       — diagnostics channel: 'note' -> notes(), 'gap' ->
//                  gapReported() -> the host's health surface
class ExtensionShimBackend : public QObject
{
    Q_OBJECT

public:
    ExtensionShimBackend(const QString &extensionId,
                         const QString &extensionName,
                         const QString &extensionPath,
                         QObject *parent = nullptr);
    ~ExtensionShimBackend() override;

    QJsonObject invoke(const QString &ns, const QString &method,
                       const QJsonObject &args);

    QStringList notes() const { return m_notes; }
    QStringList gaps() const { return m_gaps; }

    // The extension's raw manifest.json (cached) — the handshake
    // ships it to the page so synchronous lookups never block.
    QJsonObject manifestJson() const;

    // chrome.action / chrome.browserAction state — the EXT08 action
    // bar (main lane) merges against this registry; until then it is
    // surfaced through ExtensionInfo::action*.
    struct ActionState {
        QString title;
        QString badgeText;
        QString badgeColor;      // css color string as the ext set it
        QString badgeTextColor;
        QString popup;
        bool enabled = true;
    };
    static ActionState actionState(const QString &extensionId);
    // The button strip listens on the runtime's hostChanged pulse;
    // this exists so a badge-only change still repaints.
    static QJsonObject actionStateJson(const QString &extensionId);

    // Session-stable ids for tabs/windows so chrome.tabs ids stay
    // consistent across query+get and (EXT16) event payloads.
    static int tabIdFor(const QObject *view);
    static int windowIdFor(const QObject *window);

signals:
    // Pushed into the hosted page's hostEvent handler — the shim
    // fans it out to registered event listeners ('alarms.onAlarm',
    // EXT16's 'tabs.onUpdated', ...).
    void pushEvent(const QString &name, const QJsonObject &payload);
    void noteRecorded(const QString &message);
    void gapReported(const QString &api, const QString &error);
    void actionStateChanged();

private:
    QJsonObject invokeStorage(const QString &method,
                              const QJsonObject &args);
    QJsonObject invokeAlarms(const QString &method,
                             const QJsonObject &args);
    QJsonObject invokeTabs(const QString &method,
                           const QJsonObject &args);
    QJsonObject invokeAction(const QString &method,
                             const QJsonObject &args);
    QJsonObject invokeI18n(const QString &method,
                           const QJsonObject &args);
    QJsonObject invokePermissions(const QString &method,
                                  const QJsonObject &args);
    QJsonObject invokeContextMenus(const QString &method,
                                   const QJsonObject &args);
    QJsonObject invokeSidePanel(const QString &method,
                                const QJsonObject &args);
    QJsonObject invokeNotifications(const QString &method,
                                    const QJsonObject &args);
    QJsonObject invokeRuntime(const QString &method,
                              const QJsonObject &args);
    QJsonObject invokeShim(const QString &method,
                           const QJsonObject &args);

    static QJsonObject ok(const QJsonValue &result = QJsonValue());
    static QJsonObject err(const QString &code);

    // storage area helpers — 'local' persists under extstorage/, the
    // rest are in-memory for this host's lifetime.
    QJsonObject &storeFor(const QString &area);
    void flushLocalStore();
    QString storageDir() const;
    QJsonObject changesForSet(const QJsonObject &store,
                              const QJsonObject &items) const;
    QJsonObject changesForRemove(const QJsonObject &store,
                                 const QStringList &keys) const;

    QString extensionPath() const { return m_extensionPath; }
    QJsonObject tabObject(class WebView *view, int index,
                        int windowId) const;
    void fireAlarm(const QString &name);

    QString m_extensionId;
    QString m_extensionName;
    QString m_extensionPath;

    QJsonObject m_storeLocal;
    QJsonObject m_storeSession;
    bool m_localLoaded = false;
    bool m_localDirty = false;

    struct Alarm {
        QString name;
        qint64 scheduledTime = 0;   // ms since epoch
        double periodInMinutes = 0;
        QTimer *timer = nullptr;
    };
    QHash<QString, Alarm> m_alarms;
    static QJsonObject alarmJson(const Alarm &alarm);

    QHash<QString, QJsonObject> m_menuItems;
    QJsonObject m_sidePanelOptions;
    QJsonObject m_sidePanelBehavior;

    QStringList m_notes;
    QStringList m_gaps;
    QJsonObject m_manifest;         // invalid until manifestJson()
    bool m_manifestLoaded = false;
};

#endif // EXTENSIONSHIMBACKEND_H
