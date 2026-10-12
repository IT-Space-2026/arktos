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
#ifndef DOWNLOADGRAPH_H
#define DOWNLOADGRAPH_H

#include <qvector.h>
#include <qwidget.h>

// DOWN01: speed-over-time sparkline for a DownloadItem's detail card.
// Fed instantaneous bytes/second samples at a ~2 Hz cadence; paints a
// filled area graph scaled to the peak sample, or a flat baseline when
// there is no data.  Palette-only colors so it reads correctly under
// dark schemes (UIP01).
class DownloadGraph : public QWidget
{
    Q_OBJECT

public:
    explicit DownloadGraph(QWidget *parent = nullptr);

    void setSamples(const QVector<double> &bytesPerSecond);
    int sampleCount() const { return m_samples.count(); }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<double> m_samples;
};

#endif // DOWNLOADGRAPH_H
