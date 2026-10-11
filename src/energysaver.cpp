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

#include "energysaver.h"

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qsettings.h>
#include <qtimer.h>

// The threshold the "Turn On at 20% Battery or Lower" mode is named
// after.
static const int LowBatteryPercent = 20;
// sysfs attribute reads cost microseconds — a slow cadence still
// tracks AC plug/unplug and suspend/resume promptly enough for a UI
// cap, and picks up hot-plugged batteries too.
static const int PollIntervalMs = 30000;

QString EnergySaver::s_powerSupplyRoot =
    QLatin1String("/sys/class/power_supply");

EnergySaver::EnergySaver(QObject *parent)
    : QObject(parent)
    , m_mode(Never)
    , m_hasBattery(false)
    , m_onBattery(false)
    , m_batteryPercent(-1)
    , m_active(false)
    , m_pollTimer(nullptr)
{
    readPowerSupply();
    loadSettings();
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(PollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout,
            this, &EnergySaver::refreshBatteryState);
    m_pollTimer->start();
}

EnergySaver *EnergySaver::instance()
{
    static EnergySaver *saver = new EnergySaver(qApp);
    return saver;
}

EnergySaver::Mode EnergySaver::mode() const
{
    return m_mode;
}

bool EnergySaver::active() const
{
    return m_active;
}

bool EnergySaver::hasBattery() const
{
    return m_hasBattery;
}

bool EnergySaver::onBattery() const
{
    return m_onBattery;
}

int EnergySaver::batteryPercent() const
{
    return m_batteryPercent;
}

void EnergySaver::loadSettings()
{
    const int stored = QSettings().value(
        QLatin1String("performance/energySaver"), int(Never)).toInt();
    m_mode = (stored >= int(Never) && stored <= int(Always))
        ? Mode(stored) : Never;
    recompute();
}

void EnergySaver::refreshBatteryState()
{
    readPowerSupply();
    recompute();
}

void EnergySaver::setPowerSupplyRootForTesting(const QString &path)
{
    s_powerSupplyRoot = path.isEmpty()
        ? QLatin1String("/sys/class/power_supply") : path;
    instance()->refreshBatteryState();
}

static QString readPowerSupplyAttr(const QString &dir,
                                   const QString &name)
{
    QFile file(dir + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll()).trimmed();
}

// Scans every supply in the root: "Battery" entries give status +
// capacity, "Mains" entries give the AC-online flag.  On battery ==
// discharging, or a mains adapter that reports offline; when no mains
// supply exists at all the battery's own status decides.
void EnergySaver::readPowerSupply()
{
    bool hasBattery = false;
    bool discharging = false;
    bool mainsSeen = false;
    bool mainsOnline = false;
    int percent = -1;

    const QStringList entries =
        QDir(s_powerSupplyRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        const QString dir = QDir(s_powerSupplyRoot).absoluteFilePath(name);
        const QString type = readPowerSupplyAttr(dir, QLatin1String("type"));
        if (type == QLatin1String("Battery")) {
            hasBattery = true;
            if (readPowerSupplyAttr(dir, QLatin1String("status"))
                    == QLatin1String("Discharging"))
                discharging = true;
            bool ok = false;
            const int capacity = readPowerSupplyAttr(
                dir, QLatin1String("capacity")).toInt(&ok);
            // Multi-battery machines: the lowest charge drives the
            // threshold mode.
            if (ok && (percent < 0 || capacity < percent))
                percent = capacity;
        } else if (type == QLatin1String("Mains")) {
            mainsSeen = true;
            if (readPowerSupplyAttr(dir, QLatin1String("online"))
                    == QLatin1String("1"))
                mainsOnline = true;
        }
    }

    const bool onBattery = hasBattery
        && (mainsSeen ? !mainsOnline : discharging);
    if (hasBattery == m_hasBattery && onBattery == m_onBattery
        && percent == m_batteryPercent)
        return;
    m_hasBattery = hasBattery;
    m_onBattery = onBattery;
    m_batteryPercent = percent;
    emit batteryStateChanged();
}

void EnergySaver::recompute()
{
    bool active = false;
    switch (m_mode) {
    case Always:
        active = true;
        break;
    case OnBattery:
        active = m_hasBattery && m_onBattery;
        break;
    case BelowThreshold:
        // Literal reading of the label: charge at or under 20%
        // engages the saver regardless of the charger state.
        active = m_hasBattery && m_batteryPercent >= 0
            && m_batteryPercent <= LowBatteryPercent;
        break;
    case Never:
        break;
    }
    // No battery: the conditioned modes honestly degrade to Never.
    if (active == m_active)
        return;
    m_active = active;
    emit activeChanged(m_active);
}
