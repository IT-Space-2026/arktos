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

#ifndef QRCODEDIALOG_H
#define QRCODEDIALOG_H

#include <qdialog.h>
#include <qimage.h>
#include <qurl.h>

// POL01: 'Show QR for this page' — a tiny dialog rendering the page
// URL as a scannable QR code (vendored MIT qrcodegen encoder), with
// the URL itself shown selectable underneath.
class QrCodeDialog : public QDialog
{
    Q_OBJECT

public:
    explicit QrCodeDialog(const QUrl &url, QWidget *parent = nullptr);

    // Non-modal share popup: shows a QR dialog for url (deletes itself
    // on close).  URLs too long to encode still get the dialog — it
    // shows the selectable URL with a note in place of the code.
    static void showForUrl(const QUrl &url, QWidget *parent = nullptr);

    // Renders text as a QR code image of at least minPixelSize square
    // pixels (quiet zone included, integer module scale).  Returns a
    // null image when the payload doesn't fit a QR code (~2.9 KB max).
    static QImage imageForText(const QString &text, int minPixelSize = 220);
};

#endif
