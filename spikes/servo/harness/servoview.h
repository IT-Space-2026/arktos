#ifndef SERVOVIEW_H
#define SERVOVIEW_H

#include <QImage>
#include <QUrl>
#include <QWidget>

#include "servo_embed.h"

// QWidget hosting one libservo WebView via the servo-embed C ABI.
// SoftwareRenderingContext blits an RgbaImage into a QImage; the GL
// (surfman/WindowRenderingContext) path is the production follow-up.
class ServoView : public QWidget
{
    Q_OBJECT

public:
    explicit ServoView(QWidget *parent = nullptr);
    ~ServoView() override;

    bool isReady() const { return m_instance != nullptr; }
    void load(const QUrl &url);
    void pump();          // one se_spin — GUI thread
    void grabFrame();     // pull the last painted frame into m_image

    unsigned long requestCount() const;
    QString requestLog() const;
    QImage frame() const { return m_image; }
    QUrl url() const { return m_url; }
    QString title() const { return m_title; }

    void setUrl(const QUrl &url) { m_url = url; }
    void setTitle(const QString &title) { m_title = title; }

signals:
    void urlChanged(const QUrl &url);
    void titleChanged(const QString &title);
    void loadComplete();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    SeInstance m_instance = nullptr;
    QImage m_image;
    QUrl m_url;
    QString m_title;
};

#endif // SERVOVIEW_H
