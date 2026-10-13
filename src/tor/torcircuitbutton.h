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

#include <functional>

#include "tormanager.h"

class QMenu;
class TorCircuitPanel;

// TOR08: the nav-toolbar circuit dropdown — a small onion button next
// to the reader-mode button, constructed only in tor windows.  Click
// opens the circuit popup; populating re-queries the daemon (async —
// the control query never blocks the UI) and a live circuitsChanged
// repopulates the open menu, so hops/countries land as the geoip
// lookups resolve.  A missing manager greys the button; the daemon-
// off, bootstrapping and failed states read inside the dropdown.
//
// TOR13: opening the menu kicks the async re-query; the
// circuitsChanged-driven repopulate of an OPEN menu is refresh-only —
// it never re-queries (a reply would re-arm the repopulate forever)
// and it skips the rebuild entirely when the rendered content would
// be identical, because re-laying out a visible popup can reposition
// it (the "creeping popup" the user reported).
//
// TOR15: the menu's content is now a QWidgetAction-hosted
// TorCircuitPanel — the Tor-Browser-style vertical chain (This
// browser -> hops -> site) plus a "New Tor circuit for this site"
// button wired to SIGNAL NEWNYM.  The chain honestly renders the
// shared display circuit (TorManager::displayCircuitId) — this
// process has no per-domain stream isolation, so the hops are not a
// per-site path even though the header is titled by the tab's host.
//
// TOR16: a shown menu never resizes itself when hosted content
// changes, so content landing mid-open (first circuit snapshot,
// country resolutions, the NEWNYM status line) used to stay squeezed
// into the open-time footprint.  Changed content now grows the popup
// to the panel's hint (never shrinks — a no-op refresh still touches
// nothing, keeping TOR13's no-creep guarantee), and the chain view
// reserves the standard five-node chain so the size is stable across
// circuit rotations.
class TorCircuitButton : public QToolButton
{
    Q_OBJECT

public:
    explicit TorCircuitButton(TorManager *manager,
                            QWidget *parent = nullptr);

    // Where the "Circuit for <host>" header/site node gets its host
    // — the window wires this to the current tab's url().host().
    void setSiteHostProvider(const std::function<QString()> &provider);

    // ISO-3166 alpha-2 -> English country name; empty for unknown
    // codes ("ZZ", malformed input).
    static QString countryName(const QString &code);
    // Flag glyph for a country code: the code's regional-indicator
    // pair rendered through the font fallback chain — a real flag
    // when a color-emoji font is installed, the boxed two-letter code
    // when not.  Unknown/empty codes get a painted "?" badge.
    static QIcon flagIcon(const QString &code, qreal devicePixelRatio = 1.0);

private slots:
    void populateMenu();
    void refreshState();
    void onNewCircuit();

private:
    void changeEvent(QEvent *event) override;
    void syncOpenMenu();
    void applyPanelContent();
    void growMenuToFit();
    QString currentSiteHost() const;
    QIcon makeIcon() const;

    QPointer<TorManager> m_tor;
    QMenu *m_menu;
    TorCircuitPanel *m_panel;
    std::function<QString()> m_siteHostProvider;
    // Result text shown under the New-circuit button ("requested",
    // "failed", "unavailable"); cleared on each open.
    QString m_statusText;
    // What the panel currently shows, so a signal-driven repopulate
    // can skip updating a visible menu whose content would not
    // change.
    QStringList m_menuSignature;
};

#endif // TORCIRCUITBUTTON_H
