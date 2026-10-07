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

#ifndef SCRIPTBLOCKINFOBAR_H
#define SCRIPTBLOCKINFOBAR_H

#include <qframe.h>

class QLabel;

// JSCTL: the slim bar a WebView shows at the top of a page whose
// scripts were blocked — by a per-site Block rule, by the Safer/Safest
// security tier, or by the global enableJavascript preference.  It
// offers the two unblock paths: "Allow once" (session-scoped) and
// "Always allow" (persisted — unless the page is off-the-record, where
// it is session-scoped too).  The page reloads after either choice.
class ScriptBlockInfoBar : public QFrame
{
    Q_OBJECT

public:
    explicit ScriptBlockInfoBar(QWidget *parent = nullptr);
    void setHost(const QString &host);

signals:
    void allowOnce();
    void allowAlways();

private:
    QLabel *m_label;
};

#endif // SCRIPTBLOCKINFOBAR_H
