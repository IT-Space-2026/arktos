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

#ifndef PROFILEAVATARDIALOG_H
#define PROFILEAVATARDIALOG_H

#include <qdialog.h>
#include <qimage.h>
#include <qpoint.h>
#include <qrect.h>
#include <qwidget.h>

class QDialogButtonBox;
class QLabel;
class QPushButton;
class QSlider;

// PROF02: square avatar crop viewport (Firefox/Vivaldi-style).  The
// zoom factor multiplies the cover-fit scale, so the image ALWAYS
// fills the viewport — zoom 1.0 is exactly cover, 4.0 is the cap —
// and pan offsets are clamped so the crop can never leave the image.
// A circular mask overlay previews how the nav-bar button renders the
// avatar.
class ProfileAvatarCropWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ProfileAvatarCropWidget(QWidget *parent = nullptr);

    static constexpr int ViewportSize = 200;
    static constexpr qreal MaxZoom = 4.0;

    bool setImage(const QImage &image);
    bool hasImage() const;

    qreal zoom() const;
    void setZoom(qreal zoom);

    // Translation of the scaled image's top-left corner, in widget
    // pixels — always clamped so the scaled image covers the square.
    QPointF panOffset() const;
    void setPanOffset(const QPointF &offset);

    // Image-space rect currently inside the viewport.
    QRectF cropRect() const;
    QSizeF scaledImageSize() const;

    // The square crop rendered at <size> px — what gets saved.
    QImage renderCrop(int size) const;

signals:
    void cropChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    qreal coverScale() const;
    qreal scale() const;
    void clampOffset();

    QImage m_image;
    qreal m_zoom;
    QPointF m_offset;
    QPoint m_dragStart;
    QPointF m_offsetStart;
    bool m_dragging;
};

// PROF02: the avatar editor.  'Change Picture...' (ProfileAvatarActions
// or PROF04's menu) opens a QFileDialog, then this dialog — the pick is
// never applied instantly.  Save writes the crop through
// ProfileIdentity::setAvatarImage(); decode failures surface on the
// in-dialog banner, never as a crash.
class ProfileAvatarDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProfileAvatarDialog(QWidget *parent = nullptr);

    // Decodes path through a size-capped QImageReader; on failure the
    // error banner is shown and false returned.
    bool loadImage(const QString &path);

    ProfileAvatarCropWidget *cropWidget() const;
    bool hasError() const;
    QString errorText() const;

    // PROF04 seam: file dialog -> editor -> save, all modal on parent.
    // Returns true when a new avatar was saved.
    static bool changePicture(QWidget *parent);
    static QString imageFilter();

public slots:
    void accept() override;

private slots:
    void chooseImage();

private:
    void showError(const QString &text);
    void clearError();

    ProfileAvatarCropWidget *m_crop;
    QLabel *m_errorBanner;
    QSlider *m_zoomSlider;
    QDialogButtonBox *m_buttons;
};

#endif // PROFILEAVATARDIALOG_H
