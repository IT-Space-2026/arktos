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

#ifndef TORCIRCUITBUTTON_H
#define TORCIRCUITBUTTON_H

#include <qpointer.h>
#include <qtoolbutton.h>

#include "tormanager.h"

class QMenu;

// TOR08: the nav-toolbar circuit dropdown — a small onion button next
// to the reader-mode button, constructed only in tor windows.  Click
// opens a read-only menu listing the displayed circuit's hops as
// flag-icon rows ("nickname — Country"); relay fingerprint and OR
// address ride the row's tooltip.  Populating re-queries the daemon
// (async — the control query never blocks the UI) and a live
// circuitsChanged repopulates the open menu, so hops/countries land
// as the geoip lookups resolve.  A missing manager greys the button;
// the daemon-off, bootstrapping and failed states read inside the
// dropdown instead.
//
// TOR13: opening the menu kicks the async re-query; the
// circuitsChanged-driven repopulate of an OPEN menu is refresh-only —
// it never re-queries (a reply would re-arm the repopulate forever)
// and it skips the rebuild entirely when the rendered rows would be
// identical, because every clear()+re-add on a visible QMenu re-lays
// out the popup and on some platforms repositions it (the "creeping
// popup" the user reported).
class TorCircuitButton : public QToolButton
{
    Q_OBJECT

public:
    explicit TorCircuitButton(TorManager *manager,
                            QWidget *parent = nullptr);

    // ISO-3166 alpha-2 -> English country name; empty for unknown
    // codes ("ZZ", malformed input).
    static QString countryName(const QString &code);
    // Row icon for a country code: the code's regional-indicator flag
    // glyph rendered through the font fallback chain — a real flag
    // when a color-emoji font is installed, the boxed two-letter code
    // when not.  Unknown/empty codes get a painted "?" badge.
    static QIcon flagIcon(const QString &code, qreal devicePixelRatio = 1.0);
    // One hop's row text ("Guard: ElToro — Germany").  Guard/Middle/
    // Exit roles are only assigned to the canonical 3-hop path —
    // anything else numbers the hops.
    static QString hopText(const TorCircuitHop &hop, int index, int count);

    // Appends the circuit header + one row per hop to menu; returns
    // false when no displayable circuit exists (populateMenu prints
    // the empty state itself).  Static so tests can exercise the row
    // rendering without a live TorManager.
    static bool addCircuitRows(QMenu *menu,
                               const QList<TorCircuit> &circuits,
                               int displayId,
                               qreal devicePixelRatio = 1.0);

private slots:
    void populateMenu();
    void refreshState();

private:
    void changeEvent(QEvent *event) override;
    void syncOpenMenu();
    QIcon makeIcon() const;

    QPointer<TorManager> m_tor;
    QMenu *m_menu;
    // What the menu currently shows (text/tooltips/flags), so a
    // signal-driven repopulate can skip rebuilding a visible menu
    // whose contents would not change.
    QStringList m_menuSignature;
};

#endif // TORCIRCUITBUTTON_H
