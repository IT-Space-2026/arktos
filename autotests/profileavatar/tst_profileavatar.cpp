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

#include <QtTest/QtTest>
#include "qtest_arora.h"

#include <profileavataractions.h>
#include <profileavatardialog.h>
#include <profileidentity.h>

#include <qaction.h>
#include <qfileinfo.h>
#include <qimagereader.h>
#include <qpainter.h>
#include <qsettings.h>
#include <qslider.h>
#include <qtemporarydir.h>

// PROF02: the avatar pipeline — ProfileIdentity storage, the crop
// widget's cover/clamp invariants, the editor dialog's error path and
// the ProfileAvatarActions seam PROF04 will wire into the menu.
class tst_ProfileAvatar : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void namePersistence();
    void coverConstraint_data();
    void coverConstraint();
    void centerCropDefault();
    void panClamps();
    void dragPans();
    void wheelZooms();
    void saveWrites128Png();
    void loadRemoveRoundTrip();
    void corruptFileShowsError();
    void dialogAcceptSaves();
    void sliderDrivesZoom();
    void actionsSeam();
    void cropFollowsPan();

private:
    QTemporaryDir m_settingsDir;
    QTemporaryDir m_imageDir;
};

// 800x400, left half red / right half blue — the two tones let the
// crop-content leg tell which side of the source a crop came from.
static QImage twoToneLandscape()
{
    QImage image(800, 400, QImage::Format_RGB32);
    image.fill(Qt::red);
    QPainter painter(&image);
    painter.fillRect(400, 0, 400, 400, Qt::blue);
    return image;
}

static QImage portrait()
{
    QImage image(400, 800, QImage::Format_RGB32);
    image.fill(Qt::green);
    return image;
}

// The crop widget's invariant: the scaled image always covers the
// whole square viewport — no empty margins at any zoom or pan.
static bool coversViewport(const ProfileAvatarCropWidget &crop)
{
    const QPointF offset = crop.panOffset();
    const QSizeF scaled = crop.scaledImageSize();
    return offset.x() <= 0.0
        && offset.y() <= 0.0
        && offset.x() + scaled.width() >= crop.width() - 0.01
        && offset.y() + scaled.height() >= crop.height() - 0.01;
}

static QString writeImage(const QString &dirPath, const QString &name,
                          const QImage &image)
{
    const QString path = QDir(dirPath).filePath(name);
    image.save(path, "PNG");
    return path;
}

void tst_ProfileAvatar::initTestCase()
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDir.path());
}

void tst_ProfileAvatar::init()
{
    ProfileIdentity *identity = ProfileIdentity::instance();
    identity->removeAvatar();
    identity->setName(QStringLiteral("Profile"));
}

void tst_ProfileAvatar::namePersistence()
{
    ProfileIdentity *identity = ProfileIdentity::instance();
    QSignalSpy spy(identity, &ProfileIdentity::nameChanged);

    identity->setName(QStringLiteral("  Work  "));
    QCOMPARE(identity->name(), QStringLiteral("Work"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(QSettings().value(QLatin1String("profile/name")).toString(),
             QStringLiteral("Work"));

    // An empty name falls back to the default rather than persisting
    // a blank label.
    identity->setName(QStringLiteral("   "));
    QCOMPARE(identity->name(), QStringLiteral("Profile"));
    QCOMPARE(spy.count(), 2);
}

void tst_ProfileAvatar::coverConstraint_data()
{
    QTest::addColumn<QImage>("image");
    QTest::newRow("landscape") << twoToneLandscape();
    QTest::newRow("portrait") << portrait();
    QTest::newRow("square") << QImage(500, 500, QImage::Format_RGB32);
}

void tst_ProfileAvatar::coverConstraint()
{
    QFETCH(QImage, image);
    ProfileAvatarCropWidget crop;
    QVERIFY(crop.setImage(image));

    // Zoom floor: cover-fit exactly — the smaller scaled axis equals
    // the viewport edge.
    crop.setZoom(1.0);
    QCOMPARE(crop.zoom(), 1.0);
    const QSizeF cover = crop.scaledImageSize();
    QVERIFY(qAbs(qMin(cover.width(), cover.height())
                 - crop.width()) < 0.01);
    QVERIFY(coversViewport(crop));

    // Below the floor clamps back to cover — margins stay impossible.
    crop.setZoom(0.1);
    QCOMPARE(crop.zoom(), 1.0);
    QVERIFY(coversViewport(crop));

    // Zoom ceiling.
    crop.setZoom(100.0);
    QCOMPARE(crop.zoom(), ProfileAvatarCropWidget::MaxZoom);
    QVERIFY(coversViewport(crop));
}

void tst_ProfileAvatar::centerCropDefault()
{
    ProfileAvatarCropWidget crop;
    QVERIFY(crop.setImage(twoToneLandscape()));
    // cover = 200/400 = 0.5 -> scaled 400x200, centered => (-100, 0);
    // the visible image-space crop is the centered 400x400 square.
    QCOMPARE(crop.panOffset(), QPointF(-100, 0));
    QCOMPARE(crop.cropRect(), QRectF(200, 0, 400, 400));
}

void tst_ProfileAvatar::panClamps()
{
    ProfileAvatarCropWidget crop;
    QVERIFY(crop.setImage(portrait()));           // scaled 200x400

    // Positive offsets would open a left/top margin.
    crop.setPanOffset(QPointF(1e6, 1e6));
    QCOMPARE(crop.panOffset(), QPointF(0, 0));

    // Extreme negative offsets stop at the far image edge.
    crop.setPanOffset(QPointF(-1e6, -1e6));
    QCOMPARE(crop.panOffset(), QPointF(0, -200));
    QVERIFY(coversViewport(crop));

    // An in-range offset is taken verbatim.
    crop.setPanOffset(QPointF(0, -123));
    QCOMPARE(crop.panOffset(), QPointF(0, -123));

    // Landscape: x clamps to [-200, 0], y is pinned to 0.
    QVERIFY(crop.setImage(twoToneLandscape()));
    crop.setPanOffset(QPointF(-1e6, -1e6));
    QCOMPARE(crop.panOffset(), QPointF(-200, 0));
}

void tst_ProfileAvatar::dragPans()
{
    ProfileAvatarCropWidget crop;
    QVERIFY(crop.setImage(portrait()));           // offset starts (0,-100)

    const QPointF global(100, 100);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(100, 100), global,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&crop, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(150, 60), global,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&crop, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(150, 60), global,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&crop, &release);

    // Drag +50x / -40y: x clamps to 0 (image is exactly one viewport
    // wide), y lands at -140.
    QCOMPARE(crop.panOffset(), QPointF(0, -140));
    QVERIFY(coversViewport(crop));
}

void tst_ProfileAvatar::wheelZooms()
{
    ProfileAvatarCropWidget crop;
    QVERIFY(crop.setImage(twoToneLandscape()));
    QSignalSpy spy(&crop, &ProfileAvatarCropWidget::cropChanged);

    QWheelEvent wheel(QPointF(100, 100), QPointF(100, 100),
                      QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&crop, &wheel);

    QVERIFY(crop.zoom() > 1.0);
    QVERIFY(coversViewport(crop));
    QVERIFY(spy.count() >= 1);
}

void tst_ProfileAvatar::saveWrites128Png()
{
    ProfileIdentity *identity = ProfileIdentity::instance();
    ProfileAvatarCropWidget crop;
    QVERIFY(crop.setImage(twoToneLandscape()));

    QSignalSpy spy(identity, &ProfileIdentity::avatarChanged);
    QVERIFY(identity->setAvatarImage(
        crop.renderCrop(ProfileIdentity::AvatarSize)));
    QCOMPARE(spy.count(), 1);

    const QString path = identity->avatarFilePath();
    QVERIFY(QFile::exists(path));
    QImageReader reader(path);
    QCOMPARE(reader.size(),
             QSize(ProfileIdentity::AvatarSize, ProfileIdentity::AvatarSize));
    QCOMPARE(reader.format(), QByteArray("png"));

    QVERIFY(identity->hasCustomAvatar());
    QCOMPARE(identity->avatar().width(), ProfileIdentity::AvatarSize);
}

void tst_ProfileAvatar::loadRemoveRoundTrip()
{
    ProfileIdentity *identity = ProfileIdentity::instance();
    const QString path = identity->avatarFilePath();
    QSignalSpy spy(identity, &ProfileIdentity::avatarChanged);

    QVERIFY(identity->setAvatarImage(twoToneLandscape()));
    QVERIFY(identity->hasCustomAvatar());
    QCOMPARE(spy.count(), 1);

    QVERIFY(identity->removeAvatar());
    QVERIFY(!QFile::exists(path));
    QVERIFY(!identity->hasCustomAvatar());
    QVERIFY(identity->avatar().isNull());
    QCOMPARE(spy.count(), 2);

    // Removing with nothing to remove is a no-op, not an error.
    QVERIFY(identity->removeAvatar());
    QCOMPARE(spy.count(), 2);

    // A file dropped behind the singleton's back is picked up by an
    // explicit reload (PROF04's external-edit path).
    twoToneLandscape().save(path, "PNG");
    identity->reloadAvatar();
    QVERIFY(identity->hasCustomAvatar());
}

void tst_ProfileAvatar::corruptFileShowsError()
{
    ProfileAvatarDialog dialog;

    const QString garbage = m_imageDir.filePath(
        QStringLiteral("garbage.png"));
    QFile file(garbage);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not a png, just noise");
    file.close();

    QVERIFY(!dialog.loadImage(garbage));
    QVERIFY(dialog.hasError());
    QVERIFY(!dialog.errorText().isEmpty());
    QVERIFY(!dialog.cropWidget()->hasImage());

    QVERIFY(!dialog.loadImage(m_imageDir.filePath(
        QStringLiteral("missing.png"))));
    QVERIFY(dialog.hasError());

    // A good pick after failures clears the banner and loads.
    const QString good = writeImage(m_imageDir.path(),
                                    QStringLiteral("good.png"),
                                    portrait());
    QVERIFY(dialog.loadImage(good));
    QVERIFY(!dialog.hasError());
    QVERIFY(dialog.cropWidget()->hasImage());
}

void tst_ProfileAvatar::dialogAcceptSaves()
{
    ProfileIdentity *identity = ProfileIdentity::instance();
    const QString good = writeImage(m_imageDir.path(),
                                    QStringLiteral("accept.png"),
                                    twoToneLandscape());

    ProfileAvatarDialog dialog;
    QVERIFY(dialog.loadImage(good));
    QSignalSpy spy(identity, &ProfileIdentity::avatarChanged);
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(spy.count(), 1);
    QVERIFY(QFile::exists(identity->avatarFilePath()));
    QImageReader reader(identity->avatarFilePath());
    QCOMPARE(reader.size(),
             QSize(ProfileIdentity::AvatarSize, ProfileIdentity::AvatarSize));

    // Accepting with no image must not save or close — it surfaces
    // the banner instead.
    ProfileAvatarDialog empty;
    empty.accept();
    QVERIFY(empty.hasError());
    QCOMPARE(empty.result(), 0);
}

void tst_ProfileAvatar::sliderDrivesZoom()
{
    ProfileAvatarDialog dialog;
    const QString good = writeImage(m_imageDir.path(),
                                    QStringLiteral("slider.png"),
                                    portrait());
    QVERIFY(dialog.loadImage(good));

    QSlider *slider = dialog.findChild<QSlider*>();
    QVERIFY(slider);
    QCOMPARE(slider->value(), 100);

    slider->setValue(250);
    QCOMPARE(dialog.cropWidget()->zoom(), 2.5);
    QVERIFY(coversViewport(*dialog.cropWidget()));
}

void tst_ProfileAvatar::actionsSeam()
{
    QWidget host;
    ProfileAvatarActions seam(&host);
    QVERIFY(seam.changePictureAction());
    QVERIFY(seam.removePictureAction());

    // Remove is disabled until a custom avatar exists, and tracks the
    // identity signal both ways.
    QVERIFY(!seam.removePictureAction()->isEnabled());
    ProfileIdentity::instance()->setAvatarImage(twoToneLandscape());
    QTRY_VERIFY(seam.removePictureAction()->isEnabled());

    seam.removePictureAction()->trigger();
    QVERIFY(!ProfileIdentity::instance()->hasCustomAvatar());
    QTRY_VERIFY(!seam.removePictureAction()->isEnabled());
}

void tst_ProfileAvatar::cropFollowsPan()
{
    // Left half red, right half blue: panned fully left the saved
    // crop must be the red side — the drag actually selects content.
    ProfileAvatarCropWidget crop;
    QVERIFY(crop.setImage(twoToneLandscape()));

    crop.setPanOffset(QPointF(0, 0));             // left edge
    QImage left = crop.renderCrop(64);
    QVERIFY(qRed(left.pixel(32, 32)) > 200);
    QVERIFY(qBlue(left.pixel(32, 32)) < 60);

    crop.setPanOffset(QPointF(-200, 0));          // right edge
    QImage right = crop.renderCrop(64);
    QVERIFY(qBlue(right.pixel(32, 32)) > 200);
    QVERIFY(qRed(right.pixel(32, 32)) < 60);
}

QTEST_MAIN(tst_ProfileAvatar)
#include "tst_profileavatar.moc"
