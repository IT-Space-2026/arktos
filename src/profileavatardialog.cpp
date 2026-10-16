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

#include "profileavatardialog.h"

#include "profileidentity.h"

#include <qdialogbuttonbox.h>
#include <qevent.h>
#include <qfiledialog.h>
#include <qfileinfo.h>
#include <qimagereader.h>
#include <qlabel.h>
#include <qlayout.h>
#include <QtMath>
#include <qpainter.h>
#include <qpainterpath.h>
#include <qpushbutton.h>
#include <qslider.h>

// Decode bounds for untrusted picks: a multi-GB file or a
// decompression-bomb pixel count must never reach the editor.
static const qint64 MaxAvatarFileBytes = 64 * 1024 * 1024;
static const int MaxAvatarDimension = 8192;

ProfileAvatarCropWidget::ProfileAvatarCropWidget(QWidget *parent)
    : QWidget(parent)
    , m_zoom(1.0)
    , m_dragging(false)
{
    setFixedSize(ViewportSize, ViewportSize);
    setCursor(Qt::OpenHandCursor);
}

bool ProfileAvatarCropWidget::setImage(const QImage &image)
{
    if (image.isNull())
        return false;
    m_image = image;
    m_zoom = 1.0;
    // Center-crop by default: the scaled image is centered, which for
    // a non-square source crops the longer edge symmetrically.
    const qreal s = scale();
    m_offset = QPointF((width() - m_image.width() * s) / 2.0,
                       (height() - m_image.height() * s) / 2.0);
    clampOffset();
    update();
    emit cropChanged();
    return true;
}

bool ProfileAvatarCropWidget::hasImage() const
{
    return !m_image.isNull();
}

qreal ProfileAvatarCropWidget::zoom() const
{
    return m_zoom;
}

void ProfileAvatarCropWidget::setZoom(qreal zoom)
{
    const qreal clamped = qBound(qreal(1.0), zoom, MaxZoom);
    if (clamped == m_zoom || !hasImage())
        return;
    // Keep the image point at the viewport center stationary, the way
    // Firefox's cropper zooms around the middle of the crop.
    const QPointF center = (QPointF(width(), height()) / 2.0 - m_offset)
                           / scale();
    m_zoom = clamped;
    m_offset = QPointF(width(), height()) / 2.0 - center * scale();
    clampOffset();
    update();
    emit cropChanged();
}

QPointF ProfileAvatarCropWidget::panOffset() const
{
    return m_offset;
}

void ProfileAvatarCropWidget::setPanOffset(const QPointF &offset)
{
    m_offset = offset;
    clampOffset();
    update();
    emit cropChanged();
}

QRectF ProfileAvatarCropWidget::cropRect() const
{
    const qreal s = scale();
    if (s <= 0.0)
        return QRectF();
    return QRectF(-m_offset.x() / s, -m_offset.y() / s,
                  width() / s, height() / s);
}

QSizeF ProfileAvatarCropWidget::scaledImageSize() const
{
    return m_image.size() * scale();
}

QImage ProfileAvatarCropWidget::renderCrop(int size) const
{
    if (!hasImage() || size <= 0)
        return QImage();
    const QRectF source = cropRect()
        .intersected(QRectF(QPointF(0, 0), QSizeF(m_image.size())));
    QImage out(size, size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRectF(0, 0, size, size), m_image, source);
    return out;
}

qreal ProfileAvatarCropWidget::coverScale() const
{
    if (!hasImage())
        return 1.0;
    return qMax(width() / qreal(m_image.width()),
                height() / qreal(m_image.height()));
}

qreal ProfileAvatarCropWidget::scale() const
{
    return coverScale() * m_zoom;
}

void ProfileAvatarCropWidget::clampOffset()
{
    if (!hasImage()) {
        m_offset = QPointF();
        return;
    }
    // Cover-fit plus zoom >= 1 guarantees scaled >= viewport on both
    // axes, so each limit below is <= 0: the image edge can never be
    // pulled inside the viewport (no empty margins).
    const qreal minX = width() - m_image.width() * scale();
    const qreal minY = height() - m_image.height() * scale();
    m_offset.setX(qBound(minX, m_offset.x(), qreal(0.0)));
    m_offset.setY(qBound(minY, m_offset.y(), qreal(0.0)));
}

void ProfileAvatarCropWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (!hasImage()) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, tr("No image"));
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.save();
    painter.translate(m_offset);
    painter.scale(scale(), scale());
    painter.drawImage(0, 0, m_image);
    painter.restore();

    // Circular mask preview — the nav-bar button clips the avatar to
    // a circle, so dim everything outside the inscribed one.
    QPainterPath outside;
    outside.addRect(QRectF(rect()));
    QPainterPath hole;
    hole.addEllipse(QRectF(rect()));
    painter.fillPath(outside.subtracted(hole), QColor(0, 0, 0, 140));
    painter.setPen(QPen(Qt::white, 2));
    painter.drawEllipse(QRectF(rect()).adjusted(1, 1, -1, -1));
}

void ProfileAvatarCropWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && hasImage()) {
        m_dragging = true;
        m_dragStart = event->pos();
        m_offsetStart = m_offset;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }
}

void ProfileAvatarCropWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        setPanOffset(m_offsetStart + QPointF(event->pos() - m_dragStart));
        event->accept();
    }
}

void ProfileAvatarCropWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        event->accept();
    }
}

void ProfileAvatarCropWidget::wheelEvent(QWheelEvent *event)
{
    if (!hasImage()) {
        event->ignore();
        return;
    }
    const qreal steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        event->ignore();
        return;
    }
    const qreal zoom = qBound(qreal(1.0),
                              m_zoom * qPow(1.15, steps), MaxZoom);
    if (zoom == m_zoom) {
        event->accept();
        return;
    }
    // Zoom around the cursor: the image point under it stays put.
    const QPointF cursor = event->position();
    const QPointF imagePoint = (cursor - m_offset) / scale();
    m_zoom = zoom;
    m_offset = cursor - imagePoint * scale();
    clampOffset();
    update();
    emit cropChanged();
    event->accept();
}

ProfileAvatarDialog::ProfileAvatarDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Profile Picture"));

    QVBoxLayout *layout = new QVBoxLayout(this);

    m_errorBanner = new QLabel(this);
    m_errorBanner->setWordWrap(true);
    m_errorBanner->setStyleSheet(
        QLatin1String("color: white; background-color: #a02c2c;"
                      " padding: 4px; border-radius: 3px;"));
    m_errorBanner->hide();
    layout->addWidget(m_errorBanner);

    m_crop = new ProfileAvatarCropWidget(this);
    layout->addWidget(m_crop, 0, Qt::AlignHCenter);

    QHBoxLayout *zoomRow = new QHBoxLayout;
    zoomRow->addWidget(new QLabel(tr("Zoom:"), this));
    m_zoomSlider = new QSlider(Qt::Horizontal, this);
    m_zoomSlider->setRange(100, qRound(ProfileAvatarCropWidget::MaxZoom * 100));
    m_zoomSlider->setValue(100);
    zoomRow->addWidget(m_zoomSlider);
    layout->addLayout(zoomRow);

    connect(m_zoomSlider, &QSlider::valueChanged, this,
            [this](int value) { m_crop->setZoom(value / 100.0); });
    // Wheel zoom changes the crop without touching the slider — mirror
    // it back so the two controls never disagree.
    connect(m_crop, &ProfileAvatarCropWidget::cropChanged, this,
            [this]() {
                const int value = qRound(m_crop->zoom() * 100);
                if (m_zoomSlider->value() != value)
                    m_zoomSlider->setValue(value);
            });

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save
                                     | QDialogButtonBox::Cancel, this);
    QPushButton *browse = m_buttons->addButton(tr("Choose Image..."),
                                               QDialogButtonBox::ActionRole);
    connect(browse, &QPushButton::clicked, this,
            &ProfileAvatarDialog::chooseImage);
    connect(m_buttons, &QDialogButtonBox::accepted,
            this, &ProfileAvatarDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected,
            this, &ProfileAvatarDialog::reject);
    layout->addWidget(m_buttons);
}

bool ProfileAvatarDialog::loadImage(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) {
        showError(tr("The file '%1' cannot be read.")
                  .arg(info.fileName()));
        return false;
    }
    if (info.size() > MaxAvatarFileBytes) {
        showError(tr("The image '%1' is too large.")
                  .arg(info.fileName()));
        return false;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize sourceSize = reader.size();
    if (!sourceSize.isValid()) {
        showError(tr("'%1' is not a readable image.")
                  .arg(info.fileName()));
        return false;
    }
    // Cap the decode so an oversized source can't exhaust memory.
    if (sourceSize.width() > MaxAvatarDimension
        || sourceSize.height() > MaxAvatarDimension) {
        reader.setScaledSize(sourceSize.scaled(MaxAvatarDimension,
                                               MaxAvatarDimension,
                                               Qt::KeepAspectRatio));
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        showError(tr("'%1' is corrupt or uses an unsupported format.")
                  .arg(info.fileName()));
        return false;
    }
    clearError();
    m_crop->setImage(image);
    m_zoomSlider->setValue(100);
    return true;
}

ProfileAvatarCropWidget *ProfileAvatarDialog::cropWidget() const
{
    return m_crop;
}

bool ProfileAvatarDialog::hasError() const
{
    return !m_errorBanner->isHidden();
}

QString ProfileAvatarDialog::errorText() const
{
    return m_errorBanner->text();
}

QString ProfileAvatarDialog::imageFilter()
{
    return tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)");
}

bool ProfileAvatarDialog::changePicture(QWidget *parent)
{
    const QString path = QFileDialog::getOpenFileName(
        parent, tr("Choose Profile Picture"), QString(), imageFilter());
    if (path.isEmpty())
        return false;
    ProfileAvatarDialog dialog(parent);
    // A failed decode still opens the dialog — the banner explains why
    // and 'Choose Image...' lets the user pick again.
    dialog.loadImage(path);
    return dialog.exec() == QDialog::Accepted;
}

void ProfileAvatarDialog::chooseImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose Profile Picture"), QString(), imageFilter());
    if (!path.isEmpty())
        loadImage(path);
}

void ProfileAvatarDialog::accept()
{
    if (!m_crop->hasImage()) {
        showError(tr("Choose an image first."));
        return;
    }
    const QImage cropped = m_crop->renderCrop(ProfileIdentity::AvatarSize);
    if (!ProfileIdentity::instance()->setAvatarImage(cropped)) {
        showError(tr("Could not save the profile picture."));
        return;
    }
    QDialog::accept();
}

void ProfileAvatarDialog::showError(const QString &text)
{
    m_errorBanner->setText(text);
    m_errorBanner->show();
}

void ProfileAvatarDialog::clearError()
{
    m_errorBanner->hide();
    m_errorBanner->clear();
}
