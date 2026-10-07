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

#include "scriptblockinfobar.h"

#include "aroraicon.h"

#include <qboxlayout.h>
#include <qlabel.h>
#include <qpushbutton.h>
#include <qtoolbutton.h>

ScriptBlockInfoBar::ScriptBlockInfoBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QLatin1String("scriptBlockInfoBar"));
    setFrameStyle(QFrame::StyledPanel | QFrame::Raised);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 2, 8, 2);
    layout->setSpacing(6);

    m_label = new QLabel(this);
    // The host is web-derived — keep it literal (SEC10).
    m_label->setTextFormat(Qt::PlainText);
    layout->addWidget(m_label, 1);

    QPushButton *allowOnce =
        new QPushButton(tr("Allow Once"), this);
    allowOnce->setObjectName(QLatin1String("scriptAllowOnce"));
    allowOnce->setToolTip(
        tr("Run JavaScript on this site until the browser closes."));
    connect(allowOnce, &QPushButton::clicked,
            this, &ScriptBlockInfoBar::allowOnce);
    layout->addWidget(allowOnce);

    QPushButton *allowAlways =
        new QPushButton(tr("Always Allow"), this);
    allowAlways->setObjectName(QLatin1String("scriptAllowAlways"));
    allowAlways->setToolTip(
        tr("Always run JavaScript on this site — remembered across "
           "sessions."));
    connect(allowAlways, &QPushButton::clicked,
            this, &ScriptBlockInfoBar::allowAlways);
    layout->addWidget(allowAlways);

    QToolButton *close = new QToolButton(this);
    close->setObjectName(QLatin1String("scriptBlockInfoBarClose"));
    close->setAutoRaise(true);
    close->setIcon(AroraIcon::get(QLatin1String("window-close")));
    close->setAccessibleName(tr("Close"));
    close->setToolTip(tr("Hide this notice"));
    connect(close, &QToolButton::clicked, this, &QFrame::hide);
    layout->addWidget(close);
}

void ScriptBlockInfoBar::setHost(const QString &host)
{
    m_label->setText(tr("Scripts blocked on %1").arg(host));
}
