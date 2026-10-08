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

#include "popupblocker.h"

#include <qapplication.h>
#include <qsettings.h>
#include <qurl.h>

PopupBlocker::PopupBlocker(QObject *parent)
    : QObject(parent)
{
    const QStringList allowed = QSettings()
        .value(QLatin1String("popupExceptions/allowed")).toStringList();
    for (const QString &host : allowed) {
        const QString normalized = normalizeHost(host);
        if (!normalized.isEmpty() && !m_allowed.contains(normalized))
            m_allowed.append(normalized);
    }
}

PopupBlocker *PopupBlocker::instance()
{
    static PopupBlocker *blocker = new PopupBlocker(qApp);
    return blocker;
}

bool PopupBlocker::isEnabled() const
{
    return QSettings().value(QLatin1String("websettings/blockPopupWindows"),
                             true).toBool();
}

void PopupBlocker::setEnabled(bool enabled)
{
    QSettings().setValue(QLatin1String("websettings/blockPopupWindows"),
                         enabled);
    emit changed();
}

QString PopupBlocker::normalizeHost(const QString &host)
{
    QString normalized = host.toLower();
    while (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    // A leading dot is meaningless in this store — the suffix match
    // already covers "example.com or any subdomain".
    while (normalized.startsWith(QLatin1Char('.')))
        normalized = normalized.mid(1);
    return normalized;
}

// CookieJar-style suffix matching: "example.com" governs
// "example.com" itself and every "www.example.com" under it.
bool PopupBlocker::matchesHost(const QStringList &list, const QString &host)
{
    for (const QString &rule : list) {
        if (host == rule || host.endsWith(QLatin1Char('.') + rule))
            return true;
    }
    return false;
}

bool PopupBlocker::matchesHost(const QSet<QString> &set, const QString &host)
{
    QString parent = host;
    for (;;) {
        if (set.contains(parent))
            return true;
        const int dot = parent.indexOf(QLatin1Char('.'));
        if (dot < 0)
            return false;
        parent = parent.mid(dot + 1);
    }
}

bool PopupBlocker::shouldBlock(const QUrl &openerUrl) const
{
    if (!isEnabled())
        return false;
    const QString scheme = openerUrl.scheme();
    if (scheme != QLatin1String("http")
        && scheme != QLatin1String("https"))
        return false;
    return !isAllowed(openerUrl);
}

bool PopupBlocker::isAllowed(const QUrl &openerUrl) const
{
    return isAllowedHost(openerUrl.host());
}

bool PopupBlocker::isAllowedHost(const QString &host) const
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return false;
    return matchesHost(m_sessionAllowed, normalized)
        || matchesHost(m_allowed, normalized);
}

void PopupBlocker::allowHost(const QString &host, bool persistent)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return;
    if (persistent) {
        if (!m_allowed.contains(normalized)) {
            m_allowed.append(normalized);
            save();
        }
    } else {
        m_sessionAllowed.insert(normalized);
    }
    emit changed();
}

void PopupBlocker::removeAllowedHost(const QString &host)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return;
    const bool removedPersistent = m_allowed.removeAll(normalized) > 0;
    const bool removedSession = m_sessionAllowed.remove(normalized);
    if (removedPersistent)
        save();
    if (removedPersistent || removedSession)
        emit changed();
}

void PopupBlocker::clearAllowedHosts()
{
    if (m_allowed.isEmpty())
        return;
    m_allowed.clear();
    save();
    emit changed();
}

void PopupBlocker::clearSessionHosts()
{
    if (m_sessionAllowed.isEmpty())
        return;
    m_sessionAllowed.clear();
    emit changed();
}

void PopupBlocker::save() const
{
    QSettings settings;
    if (m_allowed.isEmpty())
        settings.remove(QLatin1String("popupExceptions/allowed"));
    else
        settings.setValue(QLatin1String("popupExceptions/allowed"),
                          m_allowed);
}
