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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "servobackend.h"

#ifdef ARORA_SERVO

#include "browserpaths.h"
#include "engineregistry.h"

#include <qcoreapplication.h>
#include <qdir.h>
#include <qevent.h>
#include <qfileinfo.h>
#include <qlibrary.h>
#include <qmetaobject.h>
#include <qpainter.h>

#include "servo_embed.h"

// ---- dlopen'd servo-embed ABI --------------------------------------
//
// The artifact is an optional out-of-tree build, so the backend never
// links against it — every entry point resolves through QLibrary at
// first use and a missing symbol leaves the whole backend unusable
// (artifactAvailable() == false, chrome hides the option).

namespace {

struct ServoApi {
    SeInstance (*init)(SeCallbacks, unsigned, unsigned, const char *) = nullptr;
    void (*spin)(SeInstance) = nullptr;
    int (*load)(SeInstance, const char *) = nullptr;
    void (*resize)(SeInstance, unsigned, unsigned) = nullptr;
    void (*mouseMove)(SeInstance, float, float) = nullptr;
    void (*mouseButton)(SeInstance, int, int, float, float) = nullptr;
    void (*wheel)(SeInstance, double, double, float, float) = nullptr;
    void (*key)(SeInstance, const char *) = nullptr;
    void (*goBack)(SeInstance) = nullptr;
    void (*setZoom)(SeInstance, float) = nullptr;
    void (*evalJs)(SeInstance, const char *) = nullptr;
    int (*frameSize)(SeInstance, unsigned *, unsigned *) = nullptr;
    long (*frameCopy)(SeInstance, unsigned char *, unsigned long) = nullptr;
    void (*destroy)(SeInstance) = nullptr;
};

ServoApi &api()
{
    static ServoApi instance;
    return instance;
}

QLibrary &library()
{
    static QLibrary lib;
    return lib;
}

bool resolve()
{
    static int state = 0;   // 0 untried, 1 resolved, -1 failed
    if (state != 0)
        return state > 0;
    const QString path = ServoBackend::artifactPath();
    if (path.isEmpty()) {
        state = -1;
        return false;
    }
    library().setFileName(path);
    if (!library().load()) {
        qWarning("servo-embed: %s", qPrintable(library().errorString()));
        state = -1;
        return false;
    }
    bool ok = true;
#define RESOLVE(field, symbol) \
    api().field = reinterpret_cast<decltype(api().field)>( \
        library().resolve(symbol)); \
    ok = ok && api().field;
    RESOLVE(init, "se_init")
    RESOLVE(spin, "se_spin")
    RESOLVE(load, "se_load")
    RESOLVE(resize, "se_resize")
    RESOLVE(mouseMove, "se_mouse_move")
    RESOLVE(mouseButton, "se_mouse_button")
    RESOLVE(wheel, "se_wheel")
    RESOLVE(key, "se_key")
    RESOLVE(goBack, "se_go_back")
    RESOLVE(setZoom, "se_set_zoom")
    RESOLVE(evalJs, "se_eval_js")
    RESOLVE(frameSize, "se_frame_size")
    RESOLVE(frameCopy, "se_frame_copy")
    RESOLVE(destroy, "se_destroy")
#undef RESOLVE
    state = ok ? 1 : -1;
    if (!ok)
        qWarning("servo-embed: artifact at %s is missing symbols",
                 qPrintable(path));
    return ok;
}

} // namespace

// ---- ServoView ------------------------------------------------------

void ServoView::cbWake(void *userdata)
{
    // Any Servo thread — bounce to the GUI thread.
    ServoView *view = static_cast<ServoView *>(userdata);
    QMetaObject::invokeMethod(view, &ServoView::spinOnce,
                              Qt::QueuedConnection);
}

void ServoView::cbFrameReady(void *userdata)
{
    ServoView *view = static_cast<ServoView *>(userdata);
    view->grabFrame();
    view->update();
}

void ServoView::cbUrlChanged(void *userdata, const char *url)
{
    static_cast<ServoView *>(userdata)->setUrl(
        QUrl(QString::fromUtf8(url)));
}

void ServoView::cbTitleChanged(void *userdata, const char *title)
{
    static_cast<ServoView *>(userdata)->setTitle(
        QString::fromUtf8(title));
}

void ServoView::cbLoadStatus(void *userdata, int status)
{
    static_cast<ServoView *>(userdata)->onLoadStatus(status);
}

void ServoView::cbConsole(void *, int level, const char *message)
{
    static const char *names[] = {"log", "debug", "info", "warn", "error"};
    qDebug("servo[%s] %s", names[qBound(0, level, 4)], message);
}

void ServoView::cbNavigation(void *, const char *url)
{
    // Observed only — the shim's delegate allows every navigation.
    // A policy callback needs a shim extension (SERVO-SPIKE.md).
    qDebug("servo navigation request: %s (allowed)", url);
}

void ServoView::cbResourceLoad(void *, const char *) {}

ServoView::ServoView(QWidget *parent)
    : QWidget(parent)
    , m_instance(nullptr)
    , m_loading(false)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAccessibleName(tr("Servo Page"));

    if (!resolve()) {
        qWarning("servo-embed: unavailable — ServoView inert");
        return;
    }

    SeCallbacks callbacks = {};
    callbacks.userdata = this;
    callbacks.wake = &ServoView::cbWake;
    callbacks.frame_ready = &ServoView::cbFrameReady;
    callbacks.url_changed = &ServoView::cbUrlChanged;
    callbacks.title_changed = &ServoView::cbTitleChanged;
    callbacks.load_status_changed = &ServoView::cbLoadStatus;
    callbacks.console_message = &ServoView::cbConsole;
    callbacks.navigation_request = &ServoView::cbNavigation;
    callbacks.resource_load = &ServoView::cbResourceLoad;

    const QByteArray configDir =
        ServoBackend::configDir().toUtf8();
    m_instance = api().init(callbacks,
                            width() > 0 ? width() : 800,
                            height() > 0 ? height() : 600,
                            configDir.constData());
    if (!m_instance)
        qWarning("servo-embed: se_init failed");
}

ServoView::~ServoView()
{
    if (m_instance && resolve())
        api().destroy(m_instance);
}

bool ServoView::isReady() const
{
    return m_instance != nullptr;
}

void ServoView::load(const QUrl &url)
{
    if (m_instance)
        api().load(m_instance, url.toEncoded().constData());
}

void ServoView::spinOnce()
{
    if (m_instance)
        api().spin(m_instance);
}

void ServoView::goBack()
{
    if (m_instance)
        api().goBack(m_instance);
}

void ServoView::setZoom(qreal zoom)
{
    if (m_instance)
        api().setZoom(m_instance, static_cast<float>(zoom));
}

void ServoView::evaluateJavaScript(const QString &source)
{
    if (m_instance)
        api().evalJs(m_instance, source.toUtf8().constData());
}

void ServoView::grabFrame()
{
    if (!m_instance)
        return;
    unsigned w = 0, h = 0;
    if (api().frameSize(m_instance, &w, &h) != 0 || !w || !h)
        return;
    QImage image(w, h, QImage::Format_RGBA8888);
    if (api().frameCopy(m_instance, image.bits(), image.sizeInBytes()) < 0)
        return;
    m_image = image;
}

void ServoView::setUrl(const QUrl &url)
{
    m_url = url;
    emit urlChanged(url);
}

void ServoView::setTitle(const QString &title)
{
    m_title = title;
    emit titleChanged(title);
}

void ServoView::onLoadStatus(int status)
{
    // SeCallbacks: 0 Started, 1 HeadParsed, 2 Complete — the only
    // progress signal the shim surfaces, so the bar interpolates.
    if (status == 0) {
        m_loading = true;
        emit loadStarted();
        emit loadProgress(20);
    } else if (status == 1) {
        emit loadProgress(60);
    } else if (status == 2) {
        m_loading = false;
        emit loadProgress(100);
        emit loadFinished(true);
    }
}


void ServoView::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().window());
    if (!m_image.isNull())
        painter.drawImage(QPoint(0, 0), m_image);
    if (!m_instance) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect().adjusted(12, 12, -12, -12),
                         Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap,
                         tr("Servo engine unavailable — the "
                            "servo-embed artifact could not be loaded."));
    }
}

void ServoView::resizeEvent(QResizeEvent *)
{
    if (m_instance)
        api().resize(m_instance, width(), height());
}

void ServoView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_instance)
        api().mouseMove(m_instance,
                        event->position().x(), event->position().y());
}

static int servoButton(Qt::MouseButton button)
{
    if (button == Qt::MiddleButton)
        return 1;
    if (button == Qt::RightButton)
        return 2;
    return 0;
}

void ServoView::mousePressEvent(QMouseEvent *event)
{
    if (m_instance)
        api().mouseButton(m_instance, 1, servoButton(event->button()),
                          event->position().x(), event->position().y());
}

void ServoView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_instance)
        api().mouseButton(m_instance, 0, servoButton(event->button()),
                          event->position().x(), event->position().y());
}

void ServoView::wheelEvent(QWheelEvent *event)
{
    if (!m_instance)
        return;
    const QPoint delta = event->pixelDelta().isNull()
        ? event->angleDelta() / 8
        : event->pixelDelta();
    api().wheel(m_instance, delta.x(), delta.y(),
                event->position().x(), event->position().y());
}

void ServoView::keyPressEvent(QKeyEvent *event)
{
    if (!m_instance) {
        QWidget::keyPressEvent(event);
        return;
    }
    if (!event->text().isEmpty())
        api().key(m_instance, event->text().toUtf8().constData());
    else
        QWidget::keyPressEvent(event);
}

// ---- ServoPage ------------------------------------------------------

ServoPage::ServoPage(Engine::Profile *profile, QObject *parent)
    : Engine::Page(parent)
    , m_view(nullptr)
    , m_current(-1)
    , m_zoom(1.0)
    , m_loading(false)
    , m_lifecycle(LifecycleState::Active)
    , m_profile(profile)
{
}

void ServoPage::attachView(ServoView *view)
{
    m_view = view;
    if (!m_view)
        return;
    connect(m_view, &ServoView::urlChanged,
            this, [this](const QUrl &url) {
        recordNavigation(url);
        emit urlChanged(url);
    });
    connect(m_view, &ServoView::titleChanged,
            this, &ServoPage::titleChanged);
    connect(m_view, &ServoView::loadStarted,
            this, [this]() {
        m_loading = true;
        emit loadStarted();
    });
    connect(m_view, &ServoView::loadProgress,
            this, &ServoPage::loadProgress);
    connect(m_view, &ServoView::loadFinished,
            this, [this](bool ok) {
        m_loading = false;
        emit loadFinished(ok);
    });
    connect(m_view, &ServoView::statusChanged,
            this, [this](const QString &status) {
        emit linkHovered(status);
    });
    if (!m_pendingUrl.isEmpty()) {
        const QUrl url = m_pendingUrl;
        m_pendingUrl = QUrl();
        load(url);
    }
}

void ServoPage::load(const QUrl &url)
{
    if (!url.isValid())
        return;
    if (!m_view) {
        m_pendingUrl = url;
        return;
    }
    m_view->load(url);
}

void ServoPage::stop()
{
    // The shim exposes no stop — the navigation runs to completion.
}

void ServoPage::reload()
{
    const QUrl current = url();
    if (!current.isEmpty())
        load(current);
}

QUrl ServoPage::url() const
{
    if (m_view && !m_view->url().isEmpty())
        return m_view->url();
    return m_pendingUrl;
}

void ServoPage::recordNavigation(const QUrl &url)
{
    if (m_current >= 0 && m_current < m_entries.count()
        && m_entries.at(m_current).url == url)
        return;
    while (m_entries.count() > m_current + 1)
        m_entries.removeLast();
    Engine::HistoryEntry entry;
    entry.url = url;
    m_entries.append(entry);
    m_current = m_entries.count() - 1;
    for (int i = 0; i < m_entries.count(); ++i)
        m_entries[i].index = i;
}

bool ServoPage::canGoBack() const
{
    return m_current > 0;
}

bool ServoPage::canGoForward() const
{
    // se_go_forward does not exist in the shim.
    return false;
}

void ServoPage::back()
{
    if (canGoBack() && m_view) {
        m_view->goBack();
        --m_current;
        if (m_current >= 0)
            emit urlChanged(m_entries.at(m_current).url);
    }
}

void ServoPage::forward()
{
}

int ServoPage::historyCount() const
{
    return m_entries.count();
}

int ServoPage::currentHistoryIndex() const
{
    return m_current;
}

QList<Engine::HistoryEntry> ServoPage::historyItems() const
{
    return m_entries;
}

QList<Engine::HistoryEntry> ServoPage::backItems(int maxItems) const
{
    QList<Engine::HistoryEntry> items;
    for (int i = qMax(0, m_current - maxItems); i < m_current; ++i)
        items.append(m_entries.at(i));
    return items;
}

QList<Engine::HistoryEntry> ServoPage::forwardItems(int) const
{
    return {};
}

void ServoPage::goToHistoryEntry(const Engine::HistoryEntry &entry)
{
    if (entry.index == m_current - 1)
        back();
}

void ServoPage::setZoomFactor(qreal factor)
{
    m_zoom = factor;
    if (m_view)
        m_view->setZoom(factor);
}

qreal ServoPage::zoomFactor() const
{
    return m_zoom;
}

void ServoPage::findText(const QString &, Engine::FindFlags)
{
    // No find-in-page delegate in the shim.
    Engine::FindResult result;
    emit findTextFinished(result);
}

void ServoPage::runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback)
{
    // One-way eval: the shim reports results only via the console
    // channel (JSValue has no marshaling), so callers always get an
    // invalid variant — capabilities().scriptEvaluation stays false.
    if (m_view)
        m_view->evaluateJavaScript(source);
    if (resultCallback)
        resultCallback(QVariant());
}

void ServoPage::runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback)
{
    runJavaScript(source, resultCallback);
}

void ServoPage::toHtml(const std::function<void(const QString &)> &resultCallback)
{
    if (resultCallback)
        resultCallback(QString());
}

QAction *ServoPage::action(Engine::StandardAction)
{
    return nullptr;
}

bool ServoPage::isLoading() const
{
    return m_loading;
}

bool ServoPage::recentlyAudible() const
{
    return false;
}

bool ServoPage::isOffTheRecord() const
{
    return m_profile && m_profile->isOffTheRecord();
}

void ServoPage::setPageAttribute(const QString &name, bool on)
{
    m_attributes.insert(name, on);
}

void ServoPage::setLifecycleState(LifecycleState state)
{
    m_lifecycle = state;
}

Engine::Page::LifecycleState ServoPage::lifecycleState() const
{
    return m_lifecycle;
}

qint64 ServoPage::renderProcessId() const
{
    return -1;
}

Engine::Page *ServoPage::createWindow(Engine::WebWindowType)
{
    return nullptr;
}

void ServoPage::insertScript(const Engine::Script &script)
{
    // Recorded but never injected — the shim has no script-collection
    // API (capabilities().scriptInjection is false).
    m_scripts.insert(script.name, script);
}

void ServoPage::removeScript(const QString &name)
{
    m_scripts.remove(name);
}

QList<Engine::Script> ServoPage::scripts() const
{
    return m_scripts.values();
}

void ServoPage::download(const QUrl &)
{
    // No download delegate in the shim (capabilities().downloads).
}

QWidget *ServoPage::view() const
{
    return m_view;
}

// ---- ServoProfile ---------------------------------------------------

ServoProfile::ServoProfile(const QString &storageName, QObject *parent)
    : Engine::Profile(parent)
    , m_storageName(storageName)
{
}

bool ServoProfile::isOffTheRecord() const
{
    // Servo has no profile/off-the-record concept — the swap path
    // refuses private tabs rather than pretending one exists.
    return false;
}

QString ServoProfile::storageName() const
{
    return m_storageName;
}

void ServoProfile::setUserAgent(const QString &userAgent)
{
    // Stored only — no UA override hook in the shim.
    m_userAgent = userAgent;
}

void ServoProfile::setRequestPolicy(Engine::RequestPolicy *policy)
{
    // Stored, never enforced — servo resource loads are observable
    // but cannot be blocked through the current ABI.
    m_policy = policy;
}

Engine::RequestPolicy *ServoProfile::requestPolicy() const
{
    return m_policy;
}

void ServoProfile::setCookieFilter(
        const std::function<bool(const Engine::CookieAttempt &)> &filter)
{
    m_cookieFilter = filter;
}

void ServoProfile::insertScript(const Engine::Script &script)
{
    m_scripts.insert(script.name, script);
}

void ServoProfile::removeScript(const QString &name)
{
    m_scripts.remove(name);
}

QList<Engine::Script> ServoProfile::scripts() const
{
    return m_scripts.values();
}

void ServoProfile::clear(Engine::StorageAreas)
{
}

void ServoProfile::setProfileAttribute(const QString &name, const QVariant &value)
{
    m_attributes.insert(name, value);
}

// ---- ServoBackend ----------------------------------------------------

ServoBackend::ServoBackend(QObject *parent)
    : Engine::Backend(parent)
{
}

ServoBackend *ServoBackend::instance()
{
    if (!artifactAvailable())
        return nullptr;
    static ServoBackend *backend = new ServoBackend(qApp);
    return backend;
}

QString ServoBackend::artifactPath()
{
    const QByteArray env = qgetenv("ARORA_SERVO_EMBED_LIB");
    QStringList candidates;
    if (!env.isEmpty())
        candidates << QString::fromLocal8Bit(env);
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << appDir + QStringLiteral("/libservo_embed.so")
               << appDir + QStringLiteral("/../lib/libservo_embed.so")
               // Dev-tree layouts: the binary lands in the repo root
               // (or src/ in shadow-less builds); the artifact sits in
               // spikes/servo/servo-embed/target/*.
               << appDir + QStringLiteral(
                    "/spikes/servo/servo-embed/target/release/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/spikes/servo/servo-embed/target/debug/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../spikes/servo/servo-embed/target/release/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../spikes/servo/servo-embed/target/debug/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../../spikes/servo/servo-embed/target/release/libservo_embed.so")
               << appDir + QStringLiteral(
                    "/../../spikes/servo/servo-embed/target/debug/libservo_embed.so");
    for (const QString &candidate : candidates) {
        const QString clean = QDir(candidate).absolutePath();
        if (QFileInfo::exists(clean))
            return clean;
    }
    return QString();
}

bool ServoBackend::artifactAvailable()
{
    return resolve();
}

QString ServoBackend::id() const
{
    return QStringLiteral("servo");
}

QString ServoBackend::displayName() const
{
    return QStringLiteral("Servo (experimental)");
}

Engine::Capabilities ServoBackend::capabilities() const
{
    // Deliberately near-empty — the degraded-feature table the spike
    // doc grades; nothing here is faked.  downloads is true in the
    // struct's default so it is pinned off explicitly.
    Engine::Capabilities caps;
    caps.downloads = false;
    return caps;
}

bool ServoBackend::initialize()
{
    return artifactAvailable();
}

QString ServoBackend::configDir()
{
    const QString dir = BrowserPaths::dataFilePath(QLatin1String("servo"));
    QDir().mkpath(dir);
    return dir;
}

Engine::Profile *ServoBackend::createProfile(
        const Engine::ProfileOptions &, QObject *)
{
    // One shared browsing context — servo's profile objects don't
    // exist yet; a per-tab profile would fake isolation it can't give.
    if (!m_defaultProfile)
        m_defaultProfile = new ServoProfile(QStringLiteral("servo"), this);
    return m_defaultProfile;
}

Engine::Page *ServoBackend::createPage(Engine::Profile *profile,
                                       QObject *parent)
{
    return new ServoPage(profile, parent);
}

QWidget *ServoBackend::createView(Engine::Page *page, QWidget *parent)
{
    ServoView *view = new ServoView(parent);
    if (auto *servoPage = qobject_cast<ServoPage *>(page))
        servoPage->attachView(view);
    return view;
}

namespace {

// ENG05: self-registration without a registry->backend symbol
// dependency — this translation unit exists only in servo=1 objects,
// yet engineregistry.o is linked into subproject binaries (tools,
// autotests) that never see it.  The probe runs on the first
// EngineRegistry::backends() call: the servo-embed artifact either
// resolves once and stays registered, or the option hides for the
// process lifetime.
void probeServoBackend()
{
    static bool probed = false;
    if (probed)
        return;
    probed = true;
    ServoBackend *candidate = new ServoBackend();
    if (candidate->initialize())
        EngineRegistry::registerBackend(candidate);
    else
        delete candidate;
}

struct ServoProbeRegistration {
    ServoProbeRegistration()
    {
        EngineRegistry::addProbe(&probeServoBackend);
    }
} servoProbeRegistration;

} // namespace

#endif // ARORA_SERVO
