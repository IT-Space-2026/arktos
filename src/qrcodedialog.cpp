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

#include "qrcodedialog.h"

#include "qrcodegen.hpp"

#include <qdialogbuttonbox.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qpainter.h>
#include <qpushbutton.h>
#include <qboxlayout.h>

using qrcodegen::QrCode;

// Quiet zone required by the QR spec: 4 modules on every side.
static const int kQuietZoneModules = 4;

QImage QrCodeDialog::imageForText(const QString &text, int minPixelSize)
{
    try {
        const QrCode qr = QrCode::encodeText(
            text.toUtf8().constData(), QrCode::Ecc::MEDIUM);

        const int modules = qr.getSize() + 2 * kQuietZoneModules;
        const int scale = qMax(1, (minPixelSize + modules - 1) / modules);
        QImage image(modules * scale, modules * scale,
                     QImage::Format_RGB32);
        image.fill(Qt::white);

        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        for (int y = 0; y < qr.getSize(); ++y) {
            for (int x = 0; x < qr.getSize(); ++x) {
                if (qr.getModule(x, y))
                    painter.drawRect((x + kQuietZoneModules) * scale,
                                     (y + kQuietZoneModules) * scale,
                                     scale, scale);
            }
        }
        painter.end();
        return image;
    } catch (const std::length_error &) {
        // Payload exceeds QR capacity (~2.9 KB at any level).
        return QImage();
    }
}

QrCodeDialog::QrCodeDialog(const QUrl &url, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("QR Code for This Page"));

    QVBoxLayout *layout = new QVBoxLayout(this);

    const QImage code = imageForText(url.toString());
    QLabel *codeLabel = new QLabel(this);
    codeLabel->setAlignment(Qt::AlignCenter);
    if (code.isNull()) {
        codeLabel->setText(tr("This URL is too long to encode\n"
                              "as a QR code."));
    } else {
        codeLabel->setPixmap(QPixmap::fromImage(code));
    }
    layout->addWidget(codeLabel, 0, Qt::AlignCenter);

    // Read-only edit so the user can also copy the plain link.
    QLineEdit *urlEdit = new QLineEdit(url.toString(), this);
    urlEdit->setReadOnly(true);
    urlEdit->selectAll();
    layout->addWidget(urlEdit);

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
    layout->addWidget(buttons);
}

void QrCodeDialog::showForUrl(const QUrl &url, QWidget *parent)
{
    QrCodeDialog *dialog = new QrCodeDialog(url, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}
