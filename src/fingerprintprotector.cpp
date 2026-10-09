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

#include "fingerprintprotector.h"

#include "browserapplication.h"
#include "browserprofile.h"
#include "containermanager.h"

#include <qapplication.h>
#include <qfile.h>
#include <QRandomGenerator>
#include <qsettings.h>
#include <qwebengineprofile.h>

namespace {

const int maxExceptionHosts = 512;

}

FingerprintProtector::FingerprintProtector(QObject *parent)
    : QObject(parent)
    , m_seed(QRandomGenerator::global()->generate())
{
    const QStringList persisted = QSettings().value(
        QLatin1String("privacy/fingerprintExceptions")).toStringList();
    for (const QString &host : persisted) {
        const QString normalized = normalizeHost(host);
        if (!normalized.isEmpty() && !m_persistent.contains(normalized))
            m_persistent.append(normalized);
    }
}

FingerprintProtector *FingerprintProtector::instance()
{
    static FingerprintProtector *protector = new FingerprintProtector(qApp);
    return protector;
}

bool FingerprintProtector::isEnabled() const
{
    return QSettings().value(
        QLatin1String("privacy/fingerprintProtection"), false).toBool();
}

bool FingerprintProtector::isActiveForProfile(
        QWebEngineProfile *profile) const
{
    return isEnabled()
        || (profile && profile == BrowserProfile::torProfileIfCreated());
}

QString FingerprintProtector::normalizeHost(QString host)
{
    host = host.toLower();
    while (host.endsWith(QLatin1Char('.')))
        host.chop(1);
    while (host.startsWith(QLatin1Char('.')))
        host = host.mid(1);
    return host;
}

// CookieJar-style suffix matching: "example.com" exempts itself and
// every "www.example.com" under it.
bool FingerprintProtector::isExempt(const QString &host) const
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return false;
    QStringList combined = m_persistent;
    for (const QString &h : m_session) {
        if (!combined.contains(h))
            combined.append(h);
    }
    for (const QString &rule : combined) {
        if (normalized == rule
            || normalized.endsWith(QLatin1Char('.') + rule))
            return true;
    }
    return false;
}

QStringList FingerprintProtector::exceptionHosts() const
{
    QStringList hosts = m_persistent;
    hosts.sort();
    return hosts;
}

void FingerprintProtector::addException(const QString &host,
                                        bool persistent)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return;
    if (m_session.size() < maxExceptionHosts)
        m_session.insert(normalized);
    if (!persistent)
        return;
    if (m_persistent.contains(normalized))
        return;
    m_persistent.append(normalized);
    QSettings settings;
    settings.setValue(QLatin1String("privacy/fingerprintExceptions"),
                      m_persistent);
}

void FingerprintProtector::clearException(const QString &host)
{
    const QString normalized = normalizeHost(host);
    if (normalized.isEmpty())
        return;
    m_session.remove(normalized);
    if (m_persistent.removeAll(normalized) > 0) {
        QSettings settings;
        settings.setValue(QLatin1String("privacy/fingerprintExceptions"),
                          m_persistent);
    }
}

QString FingerprintProtector::scriptSource() const
{
    QFile file(QLatin1String(":fingerprint.js"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QString source = QString::fromUtf8(file.readAll());

    // Hosts are normalized to [a-z0-9.-] at write time — safe to
    // drop into a quoted JS literal verbatim.
    QStringList literals;
    for (const QString &host : m_persistent)
        literals << QLatin1Char('"') + host + QLatin1Char('"');
    for (const QString &host : m_session) {
        if (!m_persistent.contains(host))
            literals << QLatin1Char('"') + host + QLatin1Char('"');
    }
    source.replace(QLatin1String("%EXEMPTIONS%"),
                   QLatin1Char('[') + literals.join(QLatin1Char(','))
                       + QLatin1Char(']'));
    source.replace(QLatin1String("%SEED%"), QString::number(m_seed));
    return source;
}

void FingerprintProtector::reinstallOnProfiles()
{
    BrowserProfile::installFingerprintProtection(
        BrowserProfile::normalProfile());
    if (QWebEngineProfile *otr = BrowserProfile::privateProfileIfCreated())
        BrowserProfile::installFingerprintProtection(otr);
    if (QWebEngineProfile *tor = BrowserProfile::torProfileIfCreated())
        BrowserProfile::installFingerprintProtection(tor);
    // Materialized container profiles re-run the same applySettings
    // path — installFingerprintProtection is part of it.
    ContainerManager::instance()->reapplySettings();
    emit changed();
}
