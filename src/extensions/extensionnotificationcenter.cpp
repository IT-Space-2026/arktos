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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "extensionnotificationcenter.h"

#include "extensionbackgroundhost.h"
#include "extensionshimbackend.h"

#include <qfileinfo.h>
#include <qjsonarray.h>
#include <quuid.h>

#if QT_CONFIG(dbus)
#include <qdbusconnection.h>
#include <qdbusinterface.h>
#include <qdbuspendingcall.h>
#include <qdbuspendingreply.h>
#include <qvariant.h>
#endif

namespace {

QJsonObject okReply(const QJsonValue &result = QJsonValue())
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    if (!result.isUndefined())
        o.insert(QStringLiteral("result"), result);
    return o;
}

QJsonObject errReply(const QString &code)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    return o;
}

// freedesktop body/summary strings pass through daemon markup when
// the daemon advertises body-markup — extension text is never
// markup, so it is always entity-escaped.
QString plainText(const QString &text)
{
    return text.toHtmlEscaped();
}

} // namespace

ExtensionNotificationCenter *ExtensionNotificationCenter::instance()
{
    static ExtensionNotificationCenter *center =
        new ExtensionNotificationCenter;
    return center;
}

ExtensionNotificationCenter::ExtensionNotificationCenter(
        QObject *parent)
    : QObject(parent)
{
}

void ExtensionNotificationCenter::claimSink(ExtensionShimBackend *sink)
{
    if (sink)
        m_sinks.insert(sink->extensionId(), sink);
}

void ExtensionNotificationCenter::noteTo(const QString &extensionId,
                                         const QString &text)
{
    if (ExtensionShimBackend *sink = m_sinks.value(extensionId))
        sink->recordNote(text);
}

ExtensionNotificationCenter::Record *ExtensionNotificationCenter::find(
        const QString &extensionId, const QString &notificationId)
{
    auto it = m_records.find(extensionId);
    if (it == m_records.end())
        return nullptr;
    auto rit = it->find(notificationId);
    return rit == it->end() ? nullptr : &rit.value();
}

// -------------------------------------------------------------------
// Validation — Chrome's create contract: 'type' is one of
// basic/image/list/progress, 'title' carries the heading, 'list'
// notifications carry items[], buttons cap at two.  iconUrl/imageUrl
// resolve to bundle-local files (chrome-extension://, relative, or
// bare path); remote, escaping or missing resources are dropped with
// a recorded note rather than blocking the notification.
// -------------------------------------------------------------------

QString ExtensionNotificationCenter::resolveResource(
        ExtensionShimBackend *sink, const QString &url) const
{
    if (url.isEmpty())
        return QString();
    QString path = url;
    const QString scheme = QStringLiteral("chrome-extension://")
        + sink->extensionId() + QLatin1Char('/');
    if (path.startsWith(scheme))
        path = path.mid(scheme.size());
    else if (path.startsWith(QLatin1String("chrome-extension://")))
        return QStringLiteral("[[foreign]]");
    else if (path.contains(QLatin1String("://")))
        return QStringLiteral("[[remote]]");
    const QString root =
        QFileInfo(sink->extensionPath()).canonicalFilePath();
    const QString resolved =
        QFileInfo(root + QLatin1Char('/') + path).canonicalFilePath();
    if (resolved.isEmpty()
            || !(resolved == root
                 || resolved.startsWith(root + QLatin1Char('/'))))
        return QStringLiteral("[[bad-path]]");
    return resolved;
}

QString ExtensionNotificationCenter::validate(
        ExtensionShimBackend *sink, const QJsonObject &options,
        Record *out, QStringList *notes) const
{
    const QString type =
        options.value(QStringLiteral("type")).toString();
    if (type != QLatin1String("basic")
            && type != QLatin1String("image")
            && type != QLatin1String("list")
            && type != QLatin1String("progress"))
        return QStringLiteral("invalid-argument");
    if (options.value(QStringLiteral("title")).toString().isEmpty())
        return QStringLiteral("invalid-argument");
    if (type == QLatin1String("list")
            && !options.value(QStringLiteral("items")).isArray())
        return QStringLiteral("invalid-argument");
    if (options.value(QStringLiteral("buttons")).toArray().size() > 2)
        return QStringLiteral("invalid-argument");   // Chrome's cap

    out->options = options;
    out->iconPath = resolveResource(
        sink, options.value(QStringLiteral("iconUrl")).toString());
    if (out->iconPath.startsWith(QLatin1String("[["))) {
        notes->append(QStringLiteral("iconUrl not presented (%1)")
                          .arg(out->iconPath.mid(2,
                              out->iconPath.size() - 4)));
        out->iconPath.clear();
    }
    if (type == QLatin1String("image")) {
        QString url =
            options.value(QStringLiteral("imageUrl")).toString();
        if (url.isEmpty())
            url = options.value(QStringLiteral("iconUrl")).toString();
        out->imagePath = resolveResource(sink, url);
        if (out->imagePath.startsWith(QLatin1String("[["))) {
            notes->append(QStringLiteral(
                "imageUrl not presented (%1)")
                    .arg(out->imagePath.mid(2,
                        out->imagePath.size() - 4)));
            out->imagePath.clear();
        }
    }
    return QString();
}

// -------------------------------------------------------------------
// Dispatch
// -------------------------------------------------------------------

QJsonObject ExtensionNotificationCenter::create(
        ExtensionShimBackend *sink, const QString &notificationId,
        const QJsonObject &options)
{
    if (!sink)
        return errReply(QStringLiteral("backend-gone"));
    claimSink(sink);
    const QString extId = sink->extensionId();
    QString id = notificationId;
    if (id.isEmpty())
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    Record record;
    record.extensionId = extId;
    record.extensionName = sink->extensionName();
    record.notificationId = id;
    QStringList notes;
    const QString bad = validate(sink, options, &record, &notes);
    if (!bad.isEmpty())
        return errReply(bad);

    // Chrome: create() with an existing id replaces it — the daemon
    // gets the surviving handle as replaces_id.
    const quint32 replacesId =
        m_records.value(extId).value(id).platformId;
    dropRecord(extId, id);
    record.platformId = replacesId;
    m_records[extId].insert(id, record);
    const QString why = present(extId, id);
    if (!why.isEmpty())
        notes.append(why);

    QJsonObject result;
    result.insert(QStringLiteral("id"), id);
    if (!notes.isEmpty())
        result.insert(QStringLiteral("note"),
                      notes.join(QStringLiteral("; ")));
    return okReply(result);
}

QJsonObject ExtensionNotificationCenter::update(
        ExtensionShimBackend *sink, const QString &notificationId,
        const QJsonObject &options)
{
    if (!sink)
        return errReply(QStringLiteral("backend-gone"));
    claimSink(sink);
    Record *record = find(sink->extensionId(), notificationId);
    if (!record)
        return okReply(false);
    // Chrome merges the patch over the stored options.
    QJsonObject merged = record->options;
    for (auto it = options.constBegin(); it != options.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    Record next;
    next.extensionId = record->extensionId;
    next.extensionName = record->extensionName;
    next.notificationId = record->notificationId;
    next.platformId = record->platformId;
    next.presented = record->presented;
    next.stubbed = record->stubbed;
    QStringList notes;
    const QString bad = validate(sink, merged, &next, &notes);
    if (!bad.isEmpty())
        return errReply(bad);
    *record = next;
    const QString why = present(record->extensionId,
                                record->notificationId);
    notes += why.isEmpty() ? QStringList() : QStringList{why};
    for (const QString &n : notes)
        noteTo(record->extensionId,
               QStringLiteral("chrome.notifications.update: %1")
                   .arg(n));
    return okReply(true);
}

QJsonObject ExtensionNotificationCenter::clear(
        ExtensionShimBackend *sink, const QString &notificationId)
{
    if (!sink)
        return errReply(QStringLiteral("backend-gone"));
    claimSink(sink);
    const QString extId = sink->extensionId();
    Record *record = find(extId, notificationId);
    if (!record)
        return okReply(false);
    const Record copy = *record;
    dropRecord(extId, notificationId);
    // A programmatic clear answers with onClosed(byUser=false) — the
    // daemon may also echo NotificationClosed for the close we
    // issued; the record is already gone, so the echo is a no-op.
    deliver(copy, QStringLiteral("notifications.onClosed"),
            QJsonArray{copy.notificationId, false});
    return okReply(true);
}

QJsonObject ExtensionNotificationCenter::getAll(
        ExtensionShimBackend *sink) const
{
    QJsonObject all;
    if (!sink)
        return okReply(all);
    const auto records = m_records.value(sink->extensionId());
    for (auto it = records.constBegin(); it != records.constEnd(); ++it)
        all.insert(it.key(), it.value().options);
    return okReply(all);
}

void ExtensionNotificationCenter::dropExtension(
        const QString &extensionId)
{
    const auto records = m_records.value(extensionId);
    for (auto it = records.constBegin(); it != records.constEnd(); ++it)
        closePresented(it.value());
    m_records.remove(extensionId);
    m_sinks.remove(extensionId);
    m_unservedNoted.remove(extensionId);
}

// -------------------------------------------------------------------
// Presentation
// -------------------------------------------------------------------

quint32 ExtensionNotificationCenter::platformIdFor(
        const QString &extensionId, const QString &notificationId)
        const
{
    return m_records.value(extensionId).value(notificationId)
        .platformId;
}

bool ExtensionNotificationCenter::presented(
        const QString &extensionId, const QString &notificationId)
        const
{
    return m_records.value(extensionId).value(notificationId)
        .presented;
}

void ExtensionNotificationCenter::deliver(
        const Record &record, const QString &name,
        const QJsonArray &args)
{
    const QJsonObject payload{
        {QStringLiteral("__args"), args}};
    if (ExtensionBackgroundRuntime::instance()->deliverEvent(
            record.extensionId, name, payload) == 0) {
        // No relay-registered context (a bare backend in tests, or
        // the host mid-respawn): the owning backend's own page is
        // the fallback sink — the event still reaches a context of
        // the extension like Chrome's per-context dispatch.
        if (ExtensionShimBackend *sink =
                m_sinks.value(record.extensionId))
            emit sink->pushEvent(name, payload);
    }
}

void ExtensionNotificationCenter::unserve(const QString &extensionId,
                                        const QString &why)
{
    // The record stands — Chrome's notification object exists even
    // when nothing renders it — but nothing was shown, so no
    // onShown.  One note per extension keeps the health log honest
    // without spamming it.
    if (m_unservedNoted.contains(extensionId))
        return;
    m_unservedNoted.insert(extensionId);
    noteTo(extensionId, QStringLiteral(
        "chrome.notifications: %1 — tracked but not displayed")
            .arg(why));
}

void ExtensionNotificationCenter::dropRecord(
        const QString &extensionId, const QString &notificationId)
{
    auto it = m_records.find(extensionId);
    if (it == m_records.end())
        return;
    auto rit = it->find(notificationId);
    if (rit == it->end())
        return;
    const Record record = rit.value();
    if (record.platformId)
        m_byPlatform.remove(record.platformId);
    closePresented(record);
    it->erase(rit);
    if (it->isEmpty())
        m_records.erase(it);
}

// Answers an empty string when a presentation was dispatched (or
// stubbed), else the reason nothing will ever be shown — the caller
// surfaces it as a note.
QString ExtensionNotificationCenter::present(
        const QString &extensionId, const QString &notificationId)
{
    Record *record = find(extensionId, notificationId);
    if (!record)
        return QStringLiteral("record-gone");

    if (!m_stubChecked) {
        m_stubChecked = true;
        m_stub = qEnvironmentVariableIsSet("ARKTOS_EXTNOTIFY_STUB");
    }
    if (m_stub) {
        // The test presenter: present() behaves like a daemon that
        // always answers — a synthetic platform id, 'presented', and
        // the same onShown dispatch a real Notify reply produces.
        if (record->platformId)
            m_byPlatform.remove(record->platformId);
        record->platformId = m_nextStubId++;
        record->stubbed = true;
        record->presented = true;
        m_byPlatform.insert(record->platformId,
                            {record->extensionId,
                             record->notificationId});
        deliver(*record, QStringLiteral("notifications.onShown"),
                QJsonArray{record->notificationId});
        return QString();
    }

#if QT_CONFIG(dbus)
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        unserve(extensionId, QStringLiteral("no session D-Bus"));
        return QStringLiteral("no session D-Bus");
    }
    if (!m_busWired) {
        m_busWired = true;
        bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                    QStringLiteral("/org/freedesktop/Notifications"),
                    QStringLiteral("org.freedesktop.Notifications"),
                    QStringLiteral("NotificationClosed"),
                    this,
                    SLOT(onNotificationClosed(quint32, quint32)));
        bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                    QStringLiteral("/org/freedesktop/Notifications"),
                    QStringLiteral("org.freedesktop.Notifications"),
                    QStringLiteral("ActionInvoked"),
                    this,
                    SLOT(onActionInvoked(quint32, QString)));
    }

    const QJsonObject &options = record->options;
    QString body = plainText(
        options.value(QStringLiteral("message")).toString());
    // 'list' items render as body lines — Chrome shows them under
    // the message; freedesktop has no list concept.
    const QJsonArray items =
        options.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &v : items) {
        const QJsonObject item = v.toObject();
        const QString title =
            item.value(QStringLiteral("title")).toString();
        const QString message =
            item.value(QStringLiteral("message")).toString();
        body += QStringLiteral("\n") + plainText(title);
        if (!message.isEmpty())
            body += QStringLiteral(" — ") + plainText(message);
    }
    const QString context =
        options.value(QStringLiteral("contextMessage")).toString();
    if (!context.isEmpty())
        body += QStringLiteral("\n") + plainText(context);

    // 'default' with an empty label is the freedesktop body-click
    // convention (the daemon activates it on click, never renders it
    // as a button); Chrome's two buttons ride as btn-N pairs.
    QStringList actions{QStringLiteral("default"), QString()};
    const QJsonArray buttons =
        options.value(QStringLiteral("buttons")).toArray();
    for (int i = 0; i < buttons.size() && i < 2; ++i) {
        actions.append(QStringLiteral("btn-%1").arg(i));
        actions.append(buttons.at(i).toObject()
            .value(QStringLiteral("title")).toString());
    }

    QVariantMap hints;
    const int priority = int(
        options.value(QStringLiteral("priority")).toDouble(0));
    if (priority >= 2)
        hints.insert(QStringLiteral("urgency"), uchar(2));
    else if (priority <= -1)
        hints.insert(QStringLiteral("urgency"), uchar(0));
    if (options.value(QStringLiteral("silent")).toBool())
        hints.insert(QStringLiteral("suppress-sound"), true);
    if (options.value(QStringLiteral("type")).toString()
            == QLatin1String("progress"))
        hints.insert(QStringLiteral("value"), qBound(0, int(
            options.value(QStringLiteral("progress")).toDouble(0)),
            100));
    if (!record->imagePath.isEmpty())
        hints.insert(QStringLiteral("image-path"), record->imagePath);
    if (options.value(QStringLiteral("requireInteraction")).toBool())
        hints.insert(QStringLiteral("resident"), true);

    const int timeout = options.value(
        QStringLiteral("requireInteraction")).toBool() ? 0 : -1;

    QDBusInterface iface(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), bus);
    QDBusPendingCall call = iface.asyncCall(
        QStringLiteral("Notify"), record->extensionName,
        record->platformId, record->iconPath,
        plainText(options.value(QStringLiteral("title")).toString()),
        body, actions, hints, timeout);
    QDBusPendingCallWatcher *watcher =
        new QDBusPendingCallWatcher(call, this);
    const quint32 requestedReplaces = record->platformId;
    const QString extId = extensionId;
    const QString notifId = notificationId;
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, extId, notifId, requestedReplaces](
                QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<quint32> reply = *w;
        Record *record = find(extId, notifId);
        if (reply.isError()) {
            unserve(extId, reply.error().message());
            return;
        }
        const quint32 platformId = reply.value();
        if (!record) {
            // Cleared while the call was in flight — do not leave a
            // zombie banner up.
            closeNotify(platformId);
            return;
        }
        if (requestedReplaces)
            m_byPlatform.remove(requestedReplaces);
        record->platformId = platformId;
        record->presented = true;
        m_byPlatform.insert(platformId, {extId, notifId});
        deliver(*record, QStringLiteral("notifications.onShown"),
                QJsonArray{notifId});
    });
    return QString();
#else
    const QString why = QStringLiteral("built without D-Bus");
    unserve(extensionId, why);
    return why;
#endif
}

void ExtensionNotificationCenter::closePresented(const Record &record)
{
    if (!record.platformId || record.stubbed)
        return;
#if QT_CONFIG(dbus)
    closeNotify(record.platformId);
#endif
}

#if QT_CONFIG(dbus)
void ExtensionNotificationCenter::closeNotify(quint32 platformId)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    QDBusInterface iface(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), bus);
    iface.asyncCall(QStringLiteral("CloseNotification"), platformId);
}
#endif

// -------------------------------------------------------------------
// Daemon signals -> extension events
// -------------------------------------------------------------------

void ExtensionNotificationCenter::onActionInvoked(
        quint32 platformId, const QString &actionKey)
{
    const auto key = m_byPlatform.value(platformId);
    if (key.first.isEmpty())
        return;
    Record *record = find(key.first, key.second);
    if (!record)
        return;
    if (actionKey == QLatin1String("default")) {
        deliver(*record, QStringLiteral("notifications.onClicked"),
                QJsonArray{record->notificationId});
    } else if (actionKey.startsWith(QLatin1String("btn-"))) {
        deliver(*record,
                QStringLiteral("notifications.onButtonClicked"),
                QJsonArray{record->notificationId,
                           actionKey.mid(4).toInt()});
    }
    // The daemon emits its own NotificationClosed after an action;
    // the record drops through that signal, same as a manual close.
}

void ExtensionNotificationCenter::onNotificationClosed(
        quint32 platformId, quint32 reason)
{
    const auto key = m_byPlatform.value(platformId);
    if (key.first.isEmpty())
        return;
    Record copy;
    if (Record *record = find(key.first, key.second))
        copy = *record;
    dropRecord(key.first, key.second);
    if (copy.notificationId.isEmpty())
        return;
    // freedesktop reasons: 1 expired, 2 dismissed-by-user,
    // 3 CloseNotification, 4+ undefined.  Chrome's byUser is true
    // only for a user dismissal.
    deliver(copy, QStringLiteral("notifications.onClosed"),
            QJsonArray{copy.notificationId, reason == 2});
}
