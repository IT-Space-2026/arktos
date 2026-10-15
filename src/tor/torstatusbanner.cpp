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

#include "torstatusbanner.h"

#include "arorastyle.h"
#include "tormanager.h"

#include <qboxlayout.h>
#include <qlabel.h>
#include <qpushbutton.h>

TorStatusBanner::TorStatusBanner(QWidget *parent)
    : QFrame(parent)
    , m_tor(nullptr)
{
    setObjectName(QLatin1String("torStatusBanner"));
    setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    setAccessibleName(tr("Tor connection status"));
    // Only ever hosted inside the tor window — the same fixed violet
    // family the rest of the chrome uses (TOR07/TOR14), built by the
    // central style source (THEME01).
    setStyleSheet(AroraStyle::torBannerStyleSheet());

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(AroraStyle::Spacing::Medium,
                               AroraStyle::Spacing::Small,
                               AroraStyle::Spacing::Medium,
                               AroraStyle::Spacing::Small);
    layout->setSpacing(AroraStyle::Spacing::Medium);

    m_label = new QLabel(this);
    m_label->setObjectName(QLatin1String("torStatusBannerLabel"));
    layout->addWidget(m_label, 1);

    m_retry = new QPushButton(tr("Retry"), this);
    m_retry->setObjectName(QLatin1String("torStatusBannerRetry"));
    m_retry->setAccessibleName(tr("Retry the Tor connection"));
    connect(m_retry, &QPushButton::clicked, this, [this]() {
        // start() is a no-op outside Stopped/Failed and re-arms the
        // whole spawn pipeline; the application proxy stays
        // fail-closed the entire time.
        if (m_tor)
            m_tor->start();
    });
    layout->addWidget(m_retry);

    hide();
}

void TorStatusBanner::bind(TorManager *tor)
{
    m_tor = tor;
    if (!m_tor)
        return;
    connect(m_tor, &TorManager::stateChanged,
            this, [this](TorManager::State) { refresh(); });
    connect(m_tor, &TorManager::bootstrapProgressChanged,
            this, [this](int, const QString &) { refresh(); });
    refresh();
}

void TorStatusBanner::refresh()
{
    if (!m_tor || m_tor->isReady()) {
        hide();
        return;
    }
    QString text;
    bool canRetry = false;
    switch (m_tor->state()) {
    case TorManager::Failed:
        text = tr("Tor is unavailable: %1 — pages cannot load")
            .arg(m_tor->errorString());
        canRetry = true;
        break;
    case TorManager::Stopped:
        text = tr("Tor daemon is not running — pages cannot load");
        canRetry = true;
        break;
    case TorManager::Stopping:
        text = tr("Disconnecting from the Tor network…");
        break;
    default: {
        const int progress = m_tor->bootstrapProgress();
        text = progress >= 0
            ? tr("Connecting to the Tor network — %1%").arg(progress)
            : tr("Connecting to the Tor network…");
        break;
    }
    }
    m_label->setText(text);
    m_retry->setVisible(canRetry);
    show();
}
