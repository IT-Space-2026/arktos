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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "webpermissionmanager.h"

#include <qapplication.h>
#include <qcheckbox.h>
#include <qmessagebox.h>
#include <qpushbutton.h>
#include <qmetaobject.h>
#include <qsettings.h>
#include <qwebenginepage.h>

WebPermissionManager::WebPermissionManager(QObject *parent)
    : QObject(parent)
    , m_inPrompt(false)
{
}

WebPermissionManager *WebPermissionManager::instance()
{
    static WebPermissionManager *manager = nullptr;
    if (!manager)
        manager = new WebPermissionManager(qApp);
    return manager;
}

// Capture hardware, desktop/screen capture, clipboard reads and font
// enumeration never reach the user: they are denied before the store
// or the prompt is consulted.
bool WebPermissionManager::isDenied(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::MediaAudioCapture:
    case QWebEnginePermission::PermissionType::MediaVideoCapture:
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
    case QWebEnginePermission::PermissionType::ClipboardReadWrite:
    case QWebEnginePermission::PermissionType::LocalFontsAccess:
    case QWebEnginePermission::PermissionType::Unsupported:
        return true;
    default:
        return false;
    }
}

bool WebPermissionManager::isPromptable(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::Notifications:
    case QWebEnginePermission::PermissionType::Geolocation:
    case QWebEnginePermission::PermissionType::MouseLock:
        return true;
    default:
        return false;
    }
}

static QMetaEnum permissionTypeEnum()
{
    return QWebEnginePermission::staticMetaObject.enumerator(
        QWebEnginePermission::staticMetaObject.indexOfEnumerator("PermissionType"));
}

QString WebPermissionManager::typeName(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::MediaAudioCapture:
        return tr("Audio capture (microphone)");
    case QWebEnginePermission::PermissionType::MediaVideoCapture:
        return tr("Video capture (camera)");
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
        return tr("Audio and video capture");
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
        return tr("Screen capture");
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
        return tr("Screen and audio capture");
    case QWebEnginePermission::PermissionType::MouseLock:
        return tr("Mouse lock");
    case QWebEnginePermission::PermissionType::Notifications:
        return tr("Notifications");
    case QWebEnginePermission::PermissionType::Geolocation:
        return tr("Geolocation");
    case QWebEnginePermission::PermissionType::ClipboardReadWrite:
        return tr("Clipboard read/write");
    case QWebEnginePermission::PermissionType::LocalFontsAccess:
        return tr("Local fonts");
    case QWebEnginePermission::PermissionType::Unsupported:
        break;
    }
    return tr("Unknown feature");
}

QString WebPermissionManager::typeDescription(QWebEnginePermission::PermissionType type)
{
    switch (type) {
    case QWebEnginePermission::PermissionType::Notifications:
        return tr("show notifications");
    case QWebEnginePermission::PermissionType::Geolocation:
        return tr("know your location");
    case QWebEnginePermission::PermissionType::MouseLock:
        return tr("take control of your mouse pointer");
    default:
        return typeName(type);
    }
}

// QSettings INI keys would split on '/', so the security origin is
// percent-encoded whole ("https%3A%2F%2Fexample.com") under the
// "webpermissions" group; each feature is a key below it.  The origin
// is normalized to bare scheme://host:port first — a permission is
// per security origin, so path/query/credentials must not split one
// site's decisions across multiple keys.
QString WebPermissionManager::keyFor(const QUrl &origin,
        QWebEnginePermission::PermissionType type)
{
    QUrl o = origin;
    o.setUserInfo(QString());
    o.setPath(QString());
    o.setQuery(QString());
    o.setFragment(QString());
    const QMetaEnum metaEnum = permissionTypeEnum();
    const char *key = metaEnum.valueToKey(static_cast<int>(type));
    return QString::fromUtf8(o.toEncoded().toPercentEncoding())
        + QLatin1Char('/') + QLatin1String(key ? key : "Unsupported");
}

QList<WebPermissionManager::Entry> WebPermissionManager::entries() const
{
    QList<Entry> result;
    const QMetaEnum metaEnum = permissionTypeEnum();
    QSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    const QStringList origins = settings.childGroups();
    for (const QString &originKey : origins) {
        settings.beginGroup(originKey);
        const QStringList keys = settings.childKeys();
        for (const QString &key : keys) {
            int value = metaEnum.keyToValue(key.toUtf8().constData());
            if (value == -1)
                continue;
            Entry entry;
            entry.origin = QUrl::fromEncoded(
                QByteArray::fromPercentEncoding(originKey.toUtf8()));
            entry.type = static_cast<QWebEnginePermission::PermissionType>(value);
            entry.granted = settings.value(key).toString() == QLatin1String("grant");
            result.append(entry);
        }
        settings.endGroup();
    }
    return result;
}

void WebPermissionManager::setEntry(const QUrl &origin,
        QWebEnginePermission::PermissionType type, bool granted)
{
    QSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    settings.setValue(keyFor(origin, type),
                      QLatin1String(granted ? "grant" : "deny"));
    settings.endGroup();
    emit changed();
}

void WebPermissionManager::removeEntry(const QUrl &origin,
        QWebEnginePermission::PermissionType type)
{
    QSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    settings.remove(keyFor(origin, type));
    settings.endGroup();
    m_sessionDecisions.remove(sessionKeyFor(origin, type, false));
    m_sessionDecisions.remove(sessionKeyFor(origin, type, true));
    emit changed();
}

void WebPermissionManager::clearEntries()
{
    QSettings settings;
    settings.remove(QLatin1String("webpermissions"));
    m_sessionDecisions.clear();
    emit changed();
}

QString WebPermissionManager::sessionKeyFor(const QUrl &origin,
        QWebEnginePermission::PermissionType type, bool offTheRecord)
{
    return QLatin1String(offTheRecord ? "otr/" : "")
        + keyFor(origin, type);
}

bool WebPermissionManager::storedDecision(const QUrl &origin,
        QWebEnginePermission::PermissionType type, bool *granted,
        bool offTheRecord) const
{
    const QString key = keyFor(origin, type);
    QSettings settings;
    settings.beginGroup(QLatin1String("webpermissions"));
    if (settings.contains(key)) {
        *granted = settings.value(key).toString() == QLatin1String("grant");
        return true;
    }
    const QString sessionKey = sessionKeyFor(origin, type, offTheRecord);
    if (m_sessionDecisions.contains(sessionKey)) {
        *granted = m_sessionDecisions.value(sessionKey);
        return true;
    }
    return false;
}

void WebPermissionManager::handleRequest(QWidget *parent,
        const QWebEnginePermission &request, bool offTheRecord)
{
    if (!request.isValid())
        return;

    const QWebEnginePermission::PermissionType type = request.permissionType();

    // Never prompt for the dangerous surface: capture devices, screen
    // capture, clipboard, font enumeration and anything unmapped.
    if (isDenied(type) || !isPromptable(type)) {
        request.deny();
        return;
    }

    bool granted = false;
    if (storedDecision(request.origin(), type, &granted, offTheRecord)) {
        if (granted)
            request.grant();
        else
            request.deny();
        return;
    }

    Pending pending;
    pending.parent = parent;
    pending.permission = request;
    pending.offTheRecord = offTheRecord;
    m_pending.append(pending);
    QMetaObject::invokeMethod(this, "showNextPrompt", Qt::QueuedConnection);
}

// Prompts are serialized one modal at a time across every page so a
// redirect chain cannot stack dialogs (same guard as SEC02's external
// protocol prompt).
void WebPermissionManager::showNextPrompt()
{
    // The modal exec() below runs queued slot invocations, so this can
    // be re-entered while a prompt is open; the outer loop drains the
    // queue and stacked dialogs are never shown.
    if (m_inPrompt)
        return;
    m_inPrompt = true;
    while (!m_pending.isEmpty()) {
        const Pending pending = m_pending.takeFirst();
        const QWebEnginePermission request = pending.permission;
        if (!request.isValid())
            continue;

        const QWebEnginePermission::PermissionType type = request.permissionType();

        // The answer may have been stored while this request queued
        // behind an earlier prompt.
        bool granted = false;
        if (storedDecision(request.origin(), type, &granted,
                           pending.offTheRecord)) {
            if (granted)
                request.grant();
            else
                request.deny();
            continue;
        }

        emit promptRequested(request.origin(), type);

        // The percent-encoded origin keeps control characters and
        // embedded newlines from spoofing the dialog text (SEC02).
        QString shownOrigin = QString::fromUtf8(request.origin().toEncoded());
        if (shownOrigin.size() > 128)
            shownOrigin = shownOrigin.left(128) + QLatin1String("…");

        QMessageBox box(QMessageBox::Question, tr("Permission Request"),
            tr("The page at %1 wants to %2.\n\nAllow it?")
                .arg(shownOrigin, typeDescription(type)),
            QMessageBox::NoButton, pending.parent);
        QAbstractButton *denyButton =
            box.addButton(QMessageBox::No);
        denyButton->setText(tr("Deny"));
        QAbstractButton *allowButton =
            box.addButton(QMessageBox::Yes);
        allowButton->setText(tr("Allow"));
        box.setDefaultButton(QMessageBox::No);
        QCheckBox *remember =
            new QCheckBox(tr("Remember this decision"), &box);
        remember->setChecked(true);
        box.setCheckBox(remember);
        box.exec();

        const bool allow = box.clickedButton() == allowButton;
        if (allow)
            request.grant();
        else
            request.deny();

        // Always remember for the session — without it a page looping
        // requestPermission() would spam modals.  Persisting is opt-in
        // via the checkbox and never happens for off-the-record pages.
        m_sessionDecisions.insert(
            sessionKeyFor(request.origin(), type, pending.offTheRecord),
            allow);
        if (remember->isChecked() && !pending.offTheRecord)
            setEntry(request.origin(), type, allow);
    }
    m_inPrompt = false;
}
