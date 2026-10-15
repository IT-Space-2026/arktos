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

#ifndef TORCIRCUITPANEL_H
#define TORCIRCUITPANEL_H

#include <qframe.h>
#include <qstringlist.h>

#include "tormanager.h"

class QLabel;
class QPushButton;
class TorChainView;

// TOR15: the body of the tor circuit popup — Tor Browser's panel
// layout: a "Circuit for <site>" header, a vertical node chain
// (This browser -> guard -> middle -> exit -> the site) painted as
// connector dots on a line, then a "New Tor circuit for this site"
// button with the "Your guard node may not change" hint under it.
// The non-circuit states (daemon off, connecting, failed, dead
// control link) render in the same surface as an info line.
//
// HONESTY: the chain shown is Arora's shared display circuit — the
// busiest-BUILT circuit the daemon carries
// (TorManager::displayCircuitId).  This process does not implement
// Tor Browser's stream isolation by URL-bar domain, so the hops are
// NOT a per-site circuit; the header is titled by the current tab's
// host for orientation but the path is the shared one.
class TorCircuitPanel : public QFrame
{
    Q_OBJECT

public:
    explicit TorCircuitPanel(QWidget *parent = nullptr);

    // One rendered node of the vertical chain.
    struct Node {
        enum Kind { Browser, Hop, Site } kind;
        QString title;        // "This browser", "Germany (guard)",
                              // or the site's host
        QStringList details;  // OR-address / fingerprint lines
        QString countryCode;  // flag glyph source (hop nodes only)
    };

    // Swap the rendered content.  setCircuit draws the chain;
    // setInfo draws a single wrapping info line instead.
    void setCircuit(const QString &siteHost,
                    const QList<Node> &nodes);
    void setInfo(const QString &siteHost, const QString &text);
    // enabled state plus WHY it is disabled — a greyed button with no
    // explanation reads as a dead control (TOR13), so the reason rides
    // the button's tooltip.
    void setNewCircuitEnabled(bool enabled,
                              const QString &disabledReason = QString());
    void setStatusText(const QString &text);

    QString titleText() const;
    // Everything the panel renders, one entry per line — the
    // introspection surface tests and smoke harnesses assert on.
    QStringList contentLines() const;
    int hopCount() const;

    // Row-model helpers — static so tests can exercise the text
    // building without a live daemon.
    static QString roleName(int index, int count);
    static QString shortFingerprint(const QString &fingerprint);
    static Node hopNode(const TorCircuitHop &hop, int index, int count);
    static QList<Node> nodesForCircuit(const TorCircuit &circuit,
                                       const QString &siteHost);

signals:
    void newCircuitRequested();

private:
    QLabel *m_title;
    TorChainView *m_chain;
    QLabel *m_info;
    QPushButton *m_newCircuit;
    QLabel *m_status;
    QLabel *m_note;
    QList<Node> m_nodes;
    QString m_infoText;
    bool m_showingCircuit;
};

#endif // TORCIRCUITPANEL_H
