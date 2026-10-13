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

#ifndef TORSTATUSBANNER_H
#define TORSTATUSBANNER_H

#include <qframe.h>

class QLabel;
class QPushButton;
class TorManager;

// TOR17: the tor window's honest daemon state, shown as a strip above
// the tab widget.  Until the SOCKS listener is live every page dies
// at the fail-closed proxy — without this banner the window was a row
// of raw ERR_PROXY_CONNECTION_FAILED pages with nothing saying why.
// The strip announces bootstrap progress while the daemon comes up
// and, on failure, the reason plus a Retry action (which simply calls
// TorManager::start() — the fail-closed proxy stays armed the whole
// time; retrying can never leak onto clearnet).
class TorStatusBanner : public QFrame
{
    Q_OBJECT

public:
    explicit TorStatusBanner(QWidget *parent = nullptr);

    // Wires the manager's state signals to the strip; nullptr just
    // leaves it hidden.
    void bind(TorManager *tor);
    void refresh();

private:
    TorManager *m_tor;
    QLabel *m_label;
    QPushButton *m_retry;
};

#endif // TORSTATUSBANNER_H
