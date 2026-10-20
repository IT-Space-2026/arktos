/*
 * Copyright (c) 2026, Arktos Browser - IT-Space
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

#ifdef ARKTOS_RUSTCORE
#include <qendian.h>

#include <rustcore.h>
#endif

#include "qtest_arktos.h"

class tst_QrCode : public QObject
{
    Q_OBJECT

private slots:
    void encodeMatchesReferenceMatrix();
    void rustMatchesCppCorpus();
    void imageRendersModules();
    void imageHonorsMinSize();
    void deterministic();
    void oversizedPayloadReturnsNull();
    void dialogShowsCodeAndUrl();
    void showForUrlPopsDialog();
};

// Ground truth produced by the independent Python port of Nayuki's
// qrcodegen for "https://arktos.example/clean?id=42" at ECC level M —
// a 29x29 module matrix.  If the vendored C++ encoder ever regresses
// (masking, Reed-Solomon ECC, bit layout), this fails.
static const char *const kReferenceMatrix[] = {
    "11111110010001001111101111111",
    "10000010110000111011101000001",
    "10111010000001101000001011101",
    "10111010001111110011001011101",
    "10111010101000100011001011101",
    "10000010000101000001001000001",
    "11111110101010101010101111111",
    "00000000000111101111100000000",
    "10101010000100111101100010010",
    "10010101010010110000111001001",
    "10100011011001001000110010111",
    "11110100100000000101001000010",
    "10011111011000000100011101011",
    "10101001000111000100101001001",
    "00101011010000101000001101011",
    "10011001000111111111101111010",
    "11011010100110100101101001011",
    "00000000010000100100111001101",
    "10111011000101000010010000011",
    "01000100100010011111011101010",
    "10110010111000010100111110000",
    "00000000101111000001100010111",
    "11111110001010101101101011011",
    "10000010010001111100100011011",
    "10111010110010101110111110001",
    "10111010001000101011000010111",
    "10111010100100000100000111001",
    "10000010011110010110100000010",
    "11111110101111111101111010011",
};

static const char *const kReferenceText =
    "https://arktos.example/clean?id=42";

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

// QRC01: rc_qr_encode must reproduce the vendored encoder's matrix
// bit-for-bit — qrcodegen-rs is the same code lineage, so segmenting,
// ECC boosting and mask choice all agree.  The corpus spans the
// mode/size space: numeric, alphanumeric, byte-mode UTF-8, a WIFI:
// payload, near-capacity and over-capacity inputs at every ECC level.
void tst_QrCode::rustMatchesCppCorpus()
{
#ifdef ARKTOS_RUSTCORE
    const QStringList corpus = {
        QString(),
        QStringLiteral("x"),
        QString::fromLatin1(kReferenceText),
        QStringLiteral("01234567890123456789012345"),
        QStringLiteral("HELLO WORLD $%*+-./:"),
        QStringLiteral("WIFI:T:WPA;S:HomeNet;P:s3cret;;"),
        QStringLiteral("https://arktos.example/x?a=1&b=two%20words#frag"),
        QString::fromUtf8("Grüße, 世界, 𝕏"),
        QString(2900, QLatin1Char('a')),
        QString(4000, QLatin1Char('z')),
    };
    for (const QString &text : corpus) {
        const QByteArray utf8 = text.toUtf8();
        for (int e = 0; e < 4; ++e) {
            RcBuffer buf = { nullptr, 0 };
            const RcStatus st =
                rc_qr_encode(utf8.constData(), e, &buf);

            qrcodegen::QrCode cpp = qrcodegen::QrCode::encodeText(
                "", qrcodegen::QrCode::Ecc::LOW);
            try {
                cpp = qrcodegen::QrCode::encodeText(
                    utf8.constData(), qrcodegen::QrCode::Ecc(e));
            } catch (const std::length_error &) {
                // Over-capacity: the Rust side must refuse the same
                // payloads the C++ encoder throws on.
                QVERIFY2(st != RC_OK,
                         qPrintable(QStringLiteral(
                             "rust accepted an over-capacity payload "
                             "(ecc %1)").arg(e)));
                if (buf.data)
                    rc_buffer_free(buf);
                continue;
            }

            QCOMPARE(st, RC_OK);
            QVERIFY(buf.data);
            QVERIFY(buf.len >= 4);
            const quint32 n = qFromLittleEndian<quint32>(buf.data);
            QCOMPARE(int(n), cpp.getSize());
            QCOMPARE(buf.len, size_t(4 + n * n));
            for (int y = 0; y < int(n); ++y)
                for (int x = 0; x < int(n); ++x)
                    QCOMPARE(bool(buf.data[4 + y * n + x]),
                             cpp.getModule(x, y));
            rc_buffer_free(buf);
        }
    }
#else
    QSKIP("rustcore build only");
#endif
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
