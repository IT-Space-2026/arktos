/*
 * Copyright (c) 2026, The Arora Authors
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
 */

// POL01: the vendored qrcodegen encoder + QrCodeDialog rendering for
// 'Show QR for this page'.

#include <QtTest/QtTest>
#include <QtGui/QtGui>

#include <qlabel.h>
#include <qlineedit.h>
#include <qpixmap.h>
#include <qpointer.h>

#include <qrcodedialog.h>
#include <qrcodegen.hpp>

#include "qtest_arora.h"
#include "qtry.h"

class tst_QrCode : public QObject
{
    Q_OBJECT

private slots:
    void encodeMatchesReferenceMatrix();
    void imageRendersModules();
    void imageHonorsMinSize();
    void deterministic();
    void oversizedPayloadReturnsNull();
    void dialogShowsCodeAndUrl();
    void showForUrlPopsDialog();
};

// Ground truth produced by the independent Python port of Nayuki's
// qrcodegen for "https://arora.example/clean?id=42" at ECC level M —
// a 29x29 module matrix.  If the vendored C++ encoder ever regresses
// (masking, Reed-Solomon ECC, bit layout), this fails.
static const char *const kReferenceMatrix[] = {
    "11111110011010011011101111111",
    "10000010000000000110101000001",
    "10111010111110101001101011101",
    "10111010100100001101001011101",
    "10111010101101110111001011101",
    "10000010100111111000101000001",
    "11111110101010101010101111111",
    "00000000101001010010100000000",
    "10111110010000110100001111100",
    "01000001001010011011111110001",
    "00000110110110000000010000000",
    "01001101000000101000100011010",
    "00111111011010000101010101100",
    "01011000001101110011111110001",
    "01000011110011110010001101100",
    "10101000111001000000011100010",
    "00111010111010111100100001100",
    "11011100100000000111111110101",
    "10000111111010010100100000100",
    "10100100101000111010100010010",
    "10011011101110011111111110111",
    "00000000111111100100100011111",
    "11111110010011111101101011100",
    "10000010101011011001100010000",
    "10111010100100101100111110110",
    "10111010110000000111000101111",
    "10111010111110110010001111110",
    "10000010000000111000111111010",
    "11111110111100010101010010100",
};

static const char *const kReferenceText =
    "https://arora.example/clean?id=42";

void tst_QrCode::encodeMatchesReferenceMatrix()
{
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
        kReferenceText, qrcodegen::QrCode::Ecc::MEDIUM);
    QCOMPARE(qr.getSize(), 29);
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            QCOMPARE(qr.getModule(x, y),
                     kReferenceMatrix[y][x] == '1');
}

void tst_QrCode::imageRendersModules()
{
    const QImage image = QrCodeDialog::imageForText(
        QString::fromLatin1(kReferenceText), 220);
    QVERIFY(!image.isNull());

    // 29 modules + 4-module quiet zone each side = 37; scale 6 at 220.
    QCOMPARE(image.width(), image.height());
    QCOMPARE(image.width() % 37, 0);
    const int scale = image.width() / 37;
    QVERIFY(scale >= 1);

    // Quiet zone is white...
    QCOMPARE(image.pixel(0, 0), qRgb(255, 255, 255));
    QCOMPARE(image.pixel(scale * 3, scale * 3), qRgb(255, 255, 255));
    QCOMPARE(image.pixel(image.width() - 1, image.height() - 1),
             qRgb(255, 255, 255));
    // ... and every rendered module mirrors the encoder output.
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
        kReferenceText, qrcodegen::QrCode::Ecc::MEDIUM);
    for (int y = 0; y < qr.getSize(); ++y) {
        for (int x = 0; x < qr.getSize(); ++x) {
            const QRgb pixel = image.pixel(
                (x + 4) * scale + scale / 2,
                (y + 4) * scale + scale / 2);
            QCOMPARE(pixel == qRgb(0, 0, 0), qr.getModule(x, y));
        }
    }
}

void tst_QrCode::imageHonorsMinSize()
{
    const QImage small = QrCodeDialog::imageForText(
        QStringLiteral("x"), 1);
    QVERIFY(!small.isNull());
    // Version-1 code: 21 modules + 8 quiet zone = 29.
    QCOMPARE(small.width(), 29);

    const QImage large = QrCodeDialog::imageForText(
        QStringLiteral("x"), 290);
    QVERIFY(large.width() >= 290);
}

void tst_QrCode::deterministic()
{
    const QImage a = QrCodeDialog::imageForText(QStringLiteral("same"));
    const QImage b = QrCodeDialog::imageForText(QStringLiteral("same"));
    QCOMPARE(a, b);
}

void tst_QrCode::oversizedPayloadReturnsNull()
{
    // ~2.9 KB is the QR capacity ceiling at ECC M.
    const QString payload(4000, QLatin1Char('a'));
    QVERIFY(QrCodeDialog::imageForText(payload).isNull());
}

void tst_QrCode::dialogShowsCodeAndUrl()
{
    const QUrl url(QString::fromLatin1(kReferenceText));
    QrCodeDialog dialog(url);

    const QList<QLabel *> labels = dialog.findChildren<QLabel *>();
    QVERIFY(!labels.isEmpty());
    QVERIFY(!labels.first()->pixmap().isNull());

    QLineEdit *urlEdit = dialog.findChild<QLineEdit *>();
    QVERIFY(urlEdit);
    QCOMPARE(urlEdit->text(), url.toString());
}

void tst_QrCode::showForUrlPopsDialog()
{
    QWidget anchor;
    QPointer<QrCodeDialog> dialog;
    QrCodeDialog::showForUrl(QUrl(QString::fromLatin1(kReferenceText)),
                             &anchor);

    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget *top : tops) {
            if (QrCodeDialog *d = qobject_cast<QrCodeDialog *>(top)) {
                dialog = d;
                return true;
            }
        }
        return false;
    }(), 5000);
    QVERIFY(dialog);
    dialog->close();
    QTRY_VERIFY_WITH_TIMEOUT(dialog.isNull(), 5000);
}

QTEST_MAIN(tst_QrCode)
#include "tst_qrcode.moc"
