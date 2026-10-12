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

#ifndef ENERGYSAVER_H
#define ENERGYSAVER_H

#include <qobject.h>

class QTimer;

// PREFUI05: owns the Energy Saver mode (Preferences > Performance) and
// the battery state the conditioned modes key on.  Battery info comes
// from sysfs (/sys/class/power_supply) — a handful of tiny file reads
// on a slow poll, so no UPower/D-Bus dependency is introduced.
// Machines without a battery report hasBattery() == false and the
// battery-conditioned modes degrade to Never (the Preferences page
// says so explicitly).
//
// active() is the switch consumers key off — TabWidget pins the idle
// suspend clock to the aggressive preset and freezes loading
// spinners, TabBar suppresses the hover-preview card.  Changes flip
// live via activeChanged, no restart.
class EnergySaver : public QObject
{
    Q_OBJECT

public:
    // Persisted under performance/energySaver — values must stay
    // stable across releases.
    enum Mode {
        Never = 0,           // "Never Save Energy"
        BelowThreshold = 1,  // "Turn On at 20% Battery or Lower"
        OnBattery = 2,       // "Turn On When Running on Battery"
        Always = 3           // "Always Save Energy"
    };

    static EnergySaver *instance();

    Mode mode() const;
    bool active() const;

    bool hasBattery() const;
    bool onBattery() const;
    int batteryPercent() const;   // -1 when the level is unknown

    // Re-read performance/energySaver and re-evaluate — the settings
    // page calls this on save so a mode change applies immediately.
    void loadSettings();
    // Force a battery re-poll now (the slow poll does this on its own).
    void refreshBatteryState();

    // Test seam: point the power-supply scan at a fake sysfs tree; an
    // empty path restores the real root.
    static void setPowerSupplyRootForTesting(const QString &path);

signals:
    void activeChanged(bool active);
    void batteryStateChanged();

private:
    explicit EnergySaver(QObject *parent = nullptr);
    void readPowerSupply();
    void recompute();

    static QString s_powerSupplyRoot;
    Mode m_mode;
    bool m_hasBattery;
    bool m_onBattery;
    int m_batteryPercent;
    bool m_active;
    QTimer *m_pollTimer;
};

#endif // ENERGYSAVER_H
