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

#ifdef ARORA_RUSTCORE
#include <qendian.h>

#include <rustcore.h>
#endif

using qrcodegen::QrCode;

// Quiet zone required by the QR spec: 4 modules on every side.
static const int kQuietZoneModules = 4;

// Paints a row-major 0/1 module matrix into a scaled image with the
// quiet zone — shared by the rustcore matrix and the vendored C++
// encoder's marshaled output.
static QImage paintQrImage(int size, const QByteArray &modules,
                           int minPixelSize)
{
    const int total = size + 2 * kQuietZoneModules;
    const int scale = qMax(1, (minPixelSize + total - 1) / total);
    QImage image(total * scale, total * scale, QImage::Format_RGB32);
    image.fill(Qt::white);

    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (modules.at(y * size + x))
                painter.drawRect((x + kQuietZoneModules) * scale,
                                 (y + kQuietZoneModules) * scale,
                                 scale, scale);
        }
    }
    painter.end();
    return image;
}

QImage QrCodeDialog::imageForText(const QString &text, int minPixelSize)
{
    int size = 0;
    QByteArray modules;

#ifdef ARORA_RUSTCORE
    {
        // QRC01: the rustcore encoder owns the matrix — qrcodegen-rs
        // is the same code lineage as the vendored C++ encoder, so
        // the modules come out identical.
        const QByteArray utf8 = text.toUtf8();
        RcBuffer buf = { nullptr, 0 };
        const RcStatus status = rc_qr_encode(
            utf8.constData(), int(QrCode::Ecc::MEDIUM), &buf);
        if (status == RC_OK && buf.data && buf.len >= 4) {
            const quint32 n = qFromLittleEndian<quint32>(buf.data);
            if (n >= 21 && n <= 177 && quint64(n) * n + 4 == buf.len) {
                size = int(n);
                modules = QByteArray(
                    reinterpret_cast<const char *>(buf.data) + 4,
                    size * size);
            }
        }
        if (buf.data)
            rc_buffer_free(buf);
    }
#endif

    if (size == 0) {
        // No-rust build and FFI-failure fallback: the vendored Nayuki
        // encoder marshaled into the same matrix layout.
        try {
            const QrCode qr = QrCode::encodeText(
                text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
            size = qr.getSize();
            modules.resize(size * size);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    modules[y * size + x] = qr.getModule(x, y) ? 1 : 0;
        } catch (const std::length_error &) {
            // Payload exceeds QR capacity (~2.9 KB at any level).
            return QImage();
        }
    }

    return paintQrImage(size, modules, minPixelSize);
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
