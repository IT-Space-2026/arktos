/*
 * Copyright 2026 The Arktos Authors
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

#ifndef EXTENSIONNOTIFICATIONCENTER_H
#define EXTENSIONNOTIFICATIONCENTER_H

#include <qhash.h>
#include <qjsonobject.h>
#include <qobject.h>
#include <qpair.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>

class ExtensionShimBackend;

// ER12 — chrome.notifications, served by the hosted runtime.  Qt's
// extension embedder never registered the notifications delegate, so
// the shim's create/update/clear/getAll land here: a per-extension
// record registry (Chrome keeps notifications browser-side — they
// outlive a service-worker respawn and die with the extension) plus
// presentation over org.freedesktop.Notifications on the session bus
// — the platform surface Arktos already uses for D-Bus integration
// (downloadmanager's FileManager1 call).
//
// Daemon signals (NotificationClosed / ActionInvoked) arrive on the
// slots below and dispatch back into the extension's shim contexts
// via ExtensionBackgroundRuntime::deliverEvent: the conventional
// 'default' action is the body click (onClicked), 'btn-N' actions are
// the two Chrome-capped buttons (onButtonClicked), and a close maps
// its freedesktop reason onto Chrome's byUser flag.
//
// Honesty model: when no notification service is reachable the
// record is still created — getAll/update/clear semantics hold
// exactly like Chrome's browser-side objects — but nothing claims it
// was shown: no onShown, and a one-time shim note per extension says
// the banner never left the process.  ARKTOS_EXTNOTIFY_STUB=1 selects
// the headless presenter the autotest leg drives: records present
// synthetically, nothing reaches the desktop.
class ExtensionNotificationCenter : public QObject
{
    Q_OBJECT

public:
    static ExtensionNotificationCenter *instance();

    // chrome.notifications.* dispatch — invoked from
    // ExtensionShimBackend::invokeNotifications after the
    // 'notifications' manifest-permission gate.  'sink' is the
    // calling backend: the most recent caller owns the extension's
    // note sink; a respawned host inherits the records on its next
    // call.
    QJsonObject create(ExtensionShimBackend *sink,
                       const QString &notificationId,
                       const QJsonObject &options);
    QJsonObject update(ExtensionShimBackend *sink,
                       const QString &notificationId,
                       const QJsonObject &options);
    QJsonObject clear(ExtensionShimBackend *sink,
                      const QString &notificationId);
    QJsonObject getAll(ExtensionShimBackend *sink) const;
    // Extension unload/disable — Chrome dismisses an extension's
    // notifications with it: records drop silently (no context left
    // to hear onClosed) and presented banners are closed.
    void dropExtension(const QString &extensionId);

    // Diagnostics/test surface: the platform handle a record was
    // posted under (0 = never presented / no service) and whether a
    // presentation acknowledged it.
    quint32 platformIdFor(const QString &extensionId,
                          const QString &notificationId) const;
    bool presented(const QString &extensionId,
                   const QString &notificationId) const;

public slots:
    // The freedesktop NotificationClosed/ActionInvoked signals land
    // here — public slots so a test leg can drive the identical
    // dispatch a daemon signal would.
    void onActionInvoked(quint32 platformId, const QString &actionKey);
    void onNotificationClosed(quint32 platformId, quint32 reason);

private:
    explicit ExtensionNotificationCenter(QObject *parent = nullptr);

    struct Record {
        QString extensionId;
        QString extensionName;
        QString notificationId;
        QJsonObject options;        // what getAll returns
        QString iconPath;           // resolved bundle-local icon
        QString imagePath;          // 'image' type — resolved image
        quint32 platformId = 0;     // daemon (or stub) handle; 0 none
        bool presented = false;     // Notify acknowledged / stub
        bool stubbed = false;       // platformId is synthetic
    };

    void claimSink(ExtensionShimBackend *sink);
    // Normalize+validate a create/update options object into Record's
    // presentation fields; on failure answers the typed error code.
    QString validate(ExtensionShimBackend *sink,
                     const QJsonObject &options, Record *out,
                     QStringList *notes) const;
    QString resolveResource(ExtensionShimBackend *sink,
                            const QString &url) const;
    // Dispatches the platform presentation (freedesktop Notify, or
    // the ARKTOS_EXTNOTIFY_STUB test presenter); answers the reason
    // nothing will be shown, empty on dispatch.
    QString present(const QString &extensionId,
                    const QString &notificationId);
    void unserve(const QString &extensionId, const QString &why);
    void closePresented(const Record &record);
    void deliver(const Record &record, const QString &name,
                 const QJsonArray &args);
    void dropRecord(const QString &extensionId,
                    const QString &notificationId);
    Record *find(const QString &extensionId,
                 const QString &notificationId);
    void noteTo(const QString &extensionId, const QString &text);
#if QT_CONFIG(dbus)
    void closeNotify(quint32 platformId);
#endif

    QHash<QString, QHash<QString, Record>> m_records;
    QHash<quint32, QPair<QString, QString>> m_byPlatform;
    QHash<QString, QPointer<ExtensionShimBackend>> m_sinks;
    QSet<QString> m_unservedNoted; // ext already told "no service"
    quint32 m_nextStubId = 0x80000000u;
    bool m_stubChecked = false;
    bool m_stub = false;
    bool m_busWired = false;
};

#endif // EXTENSIONNOTIFICATIONCENTER_H
