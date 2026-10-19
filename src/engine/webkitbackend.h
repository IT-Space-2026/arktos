/*
 * Copyright 2026 The Arktos Authors
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

#ifndef WEBKITBACKEND_H
#define WEBKITBACKEND_H

// WK06/WK07 — the WPE WebKit backend.
//
// The class is in the build whenever engine.pri resolves the webkit
// CONFIG — mirroring servobackend's compile-but-inert contract rather
// than conditional source exclusion: a `qmake webkit=1` build on a
// host whose pkg-config cannot resolve wpe-webkit-2.0 +
// wpe-platform-2.0 + wpe-platform-headless-2.0 still compiles this TU,
// and the backend simply reports the engine unavailable
// (librariesLinked() == false, initialize() returns false so the
// registration probe drops it, pageUnavailableReason() explains) —
// the no-libs honesty half of the gate.
//
// ARKTOS_WEBKIT_LINKED marks the builds where the packages did
// resolve: the TU then carries real libWPEWebKit references (the
// version probe keeps the DT_NEEDED honest) and every binary linking
// it carries the ~/wpe sideloaded-glibc recipe from
// .devin/webkit-env.sh — sid ld.so interp + transitive DT_RPATH +
// the g_sort_array compat shim.
//
// WK07 landed the registry probe (static-init EngineRegistry::addProbe,
// the servo shape — no engineregistry.cpp edit), the "WebKit x.y.z"
// display name off webkit_get_*_version(), and the conservative
// capabilities table (.devin/WEBKIT-SPIKE.md — only what WK01-WK04
// proved or the 2.54 headers pin down).
//
// WK08 lands the minimal real stack: WebkitPage wraps a
// WebKitWebView + its internal WPEView on a per-page WPEToplevel over
// the process-shared headless WPEDisplay (the WK02-decided model —
// Qt's QPAEventDispatcherGlib pumps the global-default GMainContext on
// the GUI thread, which is where every WPE/WebKit object lives);
// WebkitView is the QWidget that consumes "buffer-rendered" via
// wpe_buffer_import_to_pixels and blits the imported frame
// (the WK03 raster path, productized — the dmabuf/EGLImage upgrade is
// WK20).  WebkitProfile carries a lazily-created WebKitNetworkSession
// — ephemeral for OTR, the default session otherwise (per-profile
// storage trees are WK11).  EngineTab hosts the pair with zero
// changes: createPage + createView is all it calls.

#include "engineinterface.h"

#include <qhash.h>
#include <qimage.h>
#include <qpointer.h>
#include <qurl.h>
#include <qvariant.h>
#include <qwidget.h>

// Opaque WPE/glib forward declarations — the real headers are only
// included by webkitbackend.cpp under ARKTOS_WEBKIT_LINKED (glib's
// `signals` member name forces Qt keyword macros off there), so the
// header stays clean for every other TU.  Tag names match the WPE
// typedefs exactly; identical re-typedefs are legal when both
// declarations reach one TU.
typedef struct _WPEDisplay WPEDisplay;
typedef struct _WPEView WPEView;
typedef struct _WPEToplevel WPEToplevel;
typedef struct _WPEBuffer WPEBuffer;
typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitNetworkSession WebKitNetworkSession;

class WebkitPage;

// WK08 — the widget that presents a WebkitPage.  It owns nothing on
// the engine side (the WPEView belongs to the page's WebKitWebView);
// it observes "buffer-rendered" through the page, imports the buffer
// to pixels, keeps the newest frame and blits it in paintEvent.  The
// resizeEvent half of the WK03 contract lives here too: a widget
// resize calls back into the page's WPEToplevel so the NEXT produced
// buffer carries the new dims.
class WebkitView : public QWidget
{
    Q_OBJECT

public:
    explicit WebkitView(QWidget *parent = nullptr);

    // Frame accounting for the WK08 autotest — imported frames since
    // construction and the dimensions of the newest one.
    int frameCount() const { return m_frameCount; }
    QSize frameSize() const { return m_frameSize; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    // WK03 adoptFrame: stride-honest deep copy into a
    // Format_ARGB32_Premultiplied QImage, then update().
    void adoptFrame(WPEBuffer *buffer, const void *data, int stride);
    void noteImportFailure();

    QPointer<WebkitPage> m_page;
    QImage m_frame;
    QSize m_frameSize;
    int m_frameCount = 0;
    int m_importFails = 0;

    friend class WebkitPage;
};

// WK08 — a real Engine::Page over WebKitWebView.  The view stack is
// built eagerly in the ctor on the GUI thread (headless WPEDisplay is
// process-shared via WebkitBackend::sharedDisplay; the WPEToplevel is
// per page).  WPE WebKit renders into the headless display whether or
// not a widget is attached, so load() never queues — attachView only
// wires the paint consumer.
class WebkitPage : public Engine::Page
{
    Q_OBJECT

public:
    explicit WebkitPage(Engine::Profile *profile,
                        QObject *parent = nullptr);
    ~WebkitPage() override;

    // False when the WPE/WebKit object graph could not be built —
    // createPage() refuses on this rather than handing out a tab that
    // can never render.
    bool hasWebView() const { return m_webView != nullptr; }

    void load(const QUrl &url) override;
    void stop() override;
    void reload() override;
    QUrl url() const override;

    bool canGoBack() const override;
    bool canGoForward() const override;
    void back() override;
    void forward() override;

    int historyCount() const override;
    int currentHistoryIndex() const override;
    QList<Engine::HistoryEntry> historyItems() const override;
    QList<Engine::HistoryEntry> backItems(int maxItems) const override;
    QList<Engine::HistoryEntry> forwardItems(int maxItems) const override;
    void goToHistoryEntry(const Engine::HistoryEntry &entry) override;

    void setZoomFactor(qreal factor) override;
    qreal zoomFactor() const override;

    void findText(const QString &subString,
                  Engine::FindFlags options) override;

    void runJavaScript(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>()) override;
    void runJavaScriptLifted(const QString &source,
                       const std::function<void(const QVariant &)> &resultCallback
                           = std::function<void(const QVariant &)>(),
                       quint32 worldId = 0) override;
    void toHtml(const std::function<void(const QString &)> &resultCallback) override;

    QAction *action(Engine::StandardAction action) override;

    bool isLoading() const override;
    bool recentlyAudible() const override;
    bool isOffTheRecord() const override;

    void setPageAttribute(const QString &name, bool on) override;
    void setLifecycleState(LifecycleState state) override;
    LifecycleState lifecycleState() const override;
    qint64 renderProcessId() const override;

    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;

    void download(const QUrl &url) override;
    QWidget *view() const override;

    // The backend binds the paint consumer — called once from
    // WebkitBackend::createView.  attachView is also where a
    // widget-size sync happens: the page's WPEToplevel starts at a
    // nominal size and is resized to the widget's real dims.
    void attachView(WebkitView *view);

private:
    // GLib signal entry points — reached through the static
    // trampolines in the .cpp (all on the GUI thread: on the glib
    // dispatcher model the "buffer-rendered"/load signals fire while
    // the Qt loop pumps the global-default GMainContext).
    void onBufferRendered(WPEBuffer *buffer);
    void onLoadChanged(int event);
    void onLoadFailed();
    void onWebProcessTerminated();
    void onUriNotify();
    void onTitleNotify();
    void onProgressNotify();
    void resizeEngine(int width, int height);
    void focusEngine(bool in);

    // GObject trampolines — static members so the GCallback ABI
    // reaches private state without friend pollution (the servo
    // shape).  Enums and non-forward-declared glib types travel as
    // plain int/void* — identical ABI, and the header stays
    // glib-free.
    static void cbBufferRendered(WPEView *view, WPEBuffer *buffer,
                                 WebkitPage *page);
    static void cbLoadChanged(WebKitWebView *view, int event,
                              WebkitPage *page);
    static int cbLoadFailed(WebKitWebView *view, int event, void *uri,
                            void *error, WebkitPage *page);
    static void cbWebProcessTerminated(WebKitWebView *view, int reason,
                                       WebkitPage *page);
    static void cbNotifyUri(WebKitWebView *view, void *pspec,
                            WebkitPage *page);
    static void cbNotifyTitle(WebKitWebView *view, void *pspec,
                              WebkitPage *page);
    static void cbNotifyProgress(WebKitWebView *view, void *pspec,
                                 WebkitPage *page);

    Engine::Profile *m_profile;
    QPointer<WebkitView> m_view;
    WebKitWebView *m_webView = nullptr;
    WPEView *m_wpeView = nullptr;   // owned by m_webView — never freed here
    WPEToplevel *m_toplevel = nullptr;
    QUrl m_url;
    QString m_title;
    qreal m_zoom = 1.0;
    bool m_loading = false;
    bool m_progressSeen = false;
    LifecycleState m_lifecycle = LifecycleState::Active;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, bool> m_attributes;

    friend class WebkitView;
};

// WK08 — the minimal session object.  WebKitNetworkSession IS the
// profile object in WPE 2.54 (the WEBKIT-SPIKE profile row): an OTR
// profile gets webkit_network_session_new_ephemeral(), a persistent
// profile the default session until WK11 roots per-id data/cache
// dirs under the profile's storage tree.  The session is created
// lazily on the first page so a profile that never renders never
// spawns one.  Everything else stays the options-bag contract
// ServoProfile serves.
class WebkitProfile : public Engine::Profile
{
    Q_OBJECT

public:
    explicit WebkitProfile(const Engine::ProfileOptions &options,
                           QObject *parent = nullptr);
    ~WebkitProfile() override;

    bool isOffTheRecord() const override;
    QString storageName() const override;
    void setUserAgent(const QString &userAgent) override;
    void setRequestPolicy(Engine::RequestPolicy *policy) override;
    Engine::RequestPolicy *requestPolicy() const override;
    void setCookieFilter(
            const std::function<bool(const Engine::CookieAttempt &)> &filter) override;
    void insertScript(const Engine::Script &script) override;
    void removeScript(const QString &name) override;
    QList<Engine::Script> scripts() const override;
    void clear(Engine::StorageAreas areas) override;
    void setProfileAttribute(const QString &name, const QVariant &value) override;

    const Engine::ProfileOptions &options() const { return m_options; }
    // The session this profile's pages construct their WebKitWebView
    // with — lazily created, nullptr when the libraries are not
    // linked.  Not part of Engine::Profile; WebkitPage reaches it via
    // qobject_cast.
    WebKitNetworkSession *networkSession();

private:
    Engine::ProfileOptions m_options;
    QString m_userAgent;
    Engine::RequestPolicy *m_policy = nullptr;
    std::function<bool(const Engine::CookieAttempt &)> m_cookieFilter;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, QVariant> m_attributes;
    WebKitNetworkSession *m_networkSession = nullptr;
};

class WebkitBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit WebkitBackend(QObject *parent = nullptr);

    // Configure-time pkg-config resolution — false on a webkit=1
    // build where the WPE packages were absent (the inert half of
    // compile-but-inert).
    static bool librariesLinked();
    // "x.y.z" from webkit_get_*_version — empty when not linked.
    static QString wpeVersion();
    // The process-shared headless WPEDisplay — created lazily on the
    // GUI thread and kept for the process lifetime (WebKit's own
    // singletons are process-scoped too; the WK02 verdict makes the
    // GUI thread the only place these objects may live).
    // nullptr when creation/connect fails — pages then refuse.
    static WPEDisplay *sharedDisplay();
    // Runtime environment fixups the web process needs under the
    // ~/wpe sideloaded prefix (CPU raster so buffer import works
    // headless — the dmabuf/EGL path is WK20 — and the relocated
    // injected-bundle path).  Called once from initialize(); safe to
    // call again.
    static void applyRuntimeEnvironment();
    // The ~/wpe prefix this build was configured against — the
    // runtime env var wins, else the value baked at configure time.
    // Empty for a system-WPE build (nothing to whitelist then).
    static QString wpePrefix();

    QString id() const override;
    QString displayName() const override;
    QString engineNote() const override;
    Engine::Capabilities capabilities() const override;
    bool initialize() override;

    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override;
    Engine::Page *createPage(Engine::Profile *profile,
                             QObject *parent = nullptr) override;
    QString pageUnavailableReason() const override;
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override;
};

#endif // WEBKITBACKEND_H
