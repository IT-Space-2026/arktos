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
#include "themepicker.h"

#include "browsertheme.h"

#include <qpainter.h>
#include <qpainterpath.h>

// The tile's miniature is the color scheme itself: a small document
// swatch — a toolbar strip plus a few text lines — painted in the
// scheme's own colors.  Fixed colors, not the widget's palette: the
// preview must still show Light while the chrome currently wears
// Dark.  The dark swatch uses the app's real forced palette so the
// tile previews exactly what a Dark commit installs.
static void paintThemePreview(QPainter *p, const QRect &preview,
                              const QPalette &pal, int positionIndex)
{
    const QPalette darkPal = BrowserTheme::darkPalette();
    const QColor lightBase(253, 253, 253);
    const QColor lightBar(233, 235, 238);
    const QColor lightText(60, 64, 67);
    const QColor darkBase = darkPal.color(QPalette::Base);
    const QColor darkBar = darkPal.color(QPalette::Window);
    const QColor darkText = darkPal.color(QPalette::Text);

    const QRect inner = preview.adjusted(2, 2, -2, -2);
    QPainterPath clip;
    clip.addRoundedRect(inner, 3, 3);
    p->save();
    p->setClipPath(clip);
    p->setPen(Qt::NoPen);

    const auto paintDoc = [p](const QRect &r, const QColor &base,
                              const QColor &bar, const QColor &text) {
        p->setBrush(base);
        p->drawRect(r);
        p->setBrush(bar);
        p->drawRect(QRect(r.left(), r.top(), r.width(), 8));
        p->setBrush(text);
        for (int i = 0; i < 3; ++i)
            p->drawRect(QRect(r.left() + 4, r.top() + 12 + i * 6,
                              qMax(4, r.width() - 9 - i * 6), 2));
    };

    if (positionIndex == 0) {
        // Auto follows the desktop — half light, half dark.
        const int split = inner.center().x();
        paintDoc(QRect(inner.left(), inner.top(),
                       split - inner.left() + 1, inner.height()),
                 lightBase, lightBar, lightText);
        paintDoc(QRect(split + 1, inner.top(),
                       inner.right() - split, inner.height()),
                 darkBase, darkBar, darkText);
        p->setPen(QPen(pal.color(QPalette::Mid), 1));
        p->drawLine(QPoint(split, inner.top()),
                    QPoint(split, inner.bottom()));
    } else if (positionIndex == 2) {
        paintDoc(inner, darkBase, darkBar, darkText);
    } else {
        paintDoc(inner, lightBase, lightBar, lightText);
    }
    p->restore();
}

ThemePicker::ThemePicker(QWidget *parent)
    : PositionPicker({ tr("Light"), tr("Dark"), tr("Auto") },
                     { 1, 2, 0 },
                     tr("Window theme (Day/Night)"),
                     &paintThemePreview,
                     parent)
{
}
