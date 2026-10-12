#include "servoview.h"

#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QWheelEvent>

namespace {

ServoView *viewFor(void *userdata)
{
    return static_cast<ServoView *>(userdata);
}

void cbWake(void *userdata)
{
    // Any Servo thread — bounce to the GUI thread.
    ServoView *view = viewFor(userdata);
    QMetaObject::invokeMethod(view, &ServoView::pump, Qt::QueuedConnection);
}

void cbFrameReady(void *userdata)
{
    ServoView *view = viewFor(userdata);
    view->grabFrame();
    view->update();
}

void cbUrlChanged(void *userdata, const char *url)
{
    ServoView *view = viewFor(userdata);
    view->setUrl(QUrl(QString::fromUtf8(url)));
    emit view->urlChanged(QUrl(QString::fromUtf8(url)));
}

void cbTitleChanged(void *userdata, const char *title)
{
    ServoView *view = viewFor(userdata);
    view->setTitle(QString::fromUtf8(title));
    emit view->titleChanged(QString::fromUtf8(title));
}

void cbLoadStatus(void *userdata, int status)
{
    if (status == 2)
        emit viewFor(userdata)->loadComplete();
}

void cbConsole(void *, int level, const char *message)
{
    static const char *names[] = {"log", "debug", "info", "warn", "error"};
    qDebug("servo[%s] %s", names[level], message);
}

void cbNavigation(void *, const char *url)
{
    qDebug("servo navigation request: %s (allowed)", url);
}

void cbResourceLoad(void *, const char *line)
{
    qDebug("servo resource load: %s", line);
}

} // namespace

ServoView::ServoView(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);

    SeCallbacks callbacks = {};
    callbacks.userdata = this;
    callbacks.wake = cbWake;
    callbacks.frame_ready = cbFrameReady;
    callbacks.url_changed = cbUrlChanged;
    callbacks.title_changed = cbTitleChanged;
    callbacks.load_status_changed = cbLoadStatus;
    callbacks.console_message = cbConsole;
    callbacks.navigation_request = cbNavigation;
    callbacks.resource_load = cbResourceLoad;

    const QByteArray configDir =
        qgetenv("ARORA_SERVO_SPIKE_CONFIG").isEmpty()
            ? QByteArray()
            : qgetenv("ARORA_SERVO_SPIKE_CONFIG");
    m_instance = se_init(callbacks, width() ? width() : 800, height() ? height() : 600,
                       configDir.isEmpty() ? nullptr : configDir.constData());
    if (!m_instance)
        qWarning("servo-embed: se_init failed");
}

ServoView::~ServoView()
{
    se_destroy(m_instance);
}

void ServoView::load(const QUrl &url)
{
    if (m_instance)
        se_load(m_instance, url.toEncoded().constData());
}

void ServoView::pump()
{
    if (m_instance)
        se_spin(m_instance);
}

void ServoView::grabFrame()
{
    unsigned width = 0, height = 0;
    if (se_frame_size(m_instance, &width, &height) != 0 || !width || !height)
        return;
    QImage image(width, height, QImage::Format_RGBA8888);
    if (se_frame_copy(m_instance, image.bits(), image.sizeInBytes()) < 0)
        return;
    m_image = image;
}

unsigned long ServoView::requestCount() const
{
    return m_instance ? se_request_count(m_instance) : 0;
}

QString ServoView::requestLog() const
{
    if (!m_instance)
        return QString();
    QByteArray buffer(1 << 20, '\0');
    const long written = se_request_log(m_instance, buffer.data(), buffer.size());
    return written > 0 ? QString::fromUtf8(buffer.constData(), written) : QString();
}

void ServoView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().window());
    if (!m_image.isNull())
        painter.drawImage(QPoint(0, 0), m_image);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(rect().adjusted(4, 4, -4, -4), Qt::AlignBottom | Qt::AlignLeft,
                     m_url.toString());
}

void ServoView::resizeEvent(QResizeEvent *)
{
    if (m_instance)
        se_resize(m_instance, width(), height());
}

void ServoView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_instance)
        se_mouse_move(m_instance, event->position().x(), event->position().y());
}

void ServoView::mousePressEvent(QMouseEvent *event)
{
    if (!m_instance)
        return;
    int button = 0;
    if (event->button() == Qt::MiddleButton)
        button = 1;
    else if (event->button() == Qt::RightButton)
        button = 2;
    se_mouse_button(m_instance, 1, button, event->position().x(), event->position().y());
}

void ServoView::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_instance)
        return;
    int button = 0;
    if (event->button() == Qt::MiddleButton)
        button = 1;
    else if (event->button() == Qt::RightButton)
        button = 2;
    se_mouse_button(m_instance, 0, button, event->position().x(), event->position().y());
}

void ServoView::wheelEvent(QWheelEvent *event)
{
    if (m_instance)
        se_wheel(m_instance, event->pixelDelta().x(), event->pixelDelta().y(),
                 event->position().x(), event->position().y());
}
