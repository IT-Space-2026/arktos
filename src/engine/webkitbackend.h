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
// WebkitView is the QWidget that consumes "buffer-rendered" — WK20
// tries the dmabuf -> EGLImage -> GL texture import first (private
// surfaceless GLES2 context on the WPE display's own EGLDisplay,
// rendering/release fences honored, FBO readback feeds this widget)
// and falls back to WK08's wpe_buffer_import_to_pixels raster copy
// whenever the EGL leg is unavailable.  WebkitProfile carries a
// lazily-created WebKitNetworkSession
// — ephemeral for OTR, the default session otherwise (per-profile
// storage trees are WK11).  EngineTab hosts the pair with zero
// changes: createPage + createView is all it calls.
//
// WK09 completes the load-signal contract: notify::page-icons feeds
// iconChanged off the favicon database (opt-in per data manager —
// the page ctor enables it; ephemeral sessions keep it in-memory),
// load-failed claims the failure so WebKit's own error page can't
// emit a fake-success load pair behind loadFinished(false),
// progress notifies are gated on the live load so the cancel-time
// reset to 0 never lands after loadFinished, and an observe-only
// decide-policy hook maps WebKitNavigationType (WK17's RequestPolicy
// veto reuses the same hook).
//
// WK10 maps the Page history contract onto the real
// WebKitBackForwardList: historyItems()/backItems()/forwardItems()
// enumerate the list in oldest-first stack order (WebKit's back
// slice arrives closest-first and is walked tail-to-head),
// currentHistoryIndex() is the back-item count, back()/forward() and
// canGoBack()/canGoForward() proxy the view calls, and
// goToHistoryEntry() re-addresses the stack index through
// get_nth_item + go_to_back_forward_list_item.
//
// WK11 gives each profile real storage: webkit_network_session_new
// takes a per-session base data/cache directory pair that becomes
// the session's private WebKitWebsiteDataManager.  The roots are
// resolved from ProfileOptions — an explicit dataPath/cachePath pair
// (the container contract, "persistent root") gains a "WebKit" leaf
// so the session can never share a directory with a QtWebEngine
// store rooted at the same path; a persona storageName ("arktos",
// "arktos-<id>") resolves to BrowserPaths::profileDataDir(<id>) +
// "/WebKit" and "/cache/WebKit" — the VSEC02-approved layout the
// profile vault already seals (the "WebKit" leaf is what cannot
// collide with the "QtWebEngine" sibling); any other non-empty
// storageName gets a sanitized leaf under the ambient persona's
// WebKit tree so foreign names still isolate per profile.  An
// options bag with no identity at all (no storageName, no paths)
// keeps the engine-default shared session; offTheRecord always gets
// webkit_network_session_new_ephemeral — a session whose data
// manager owns no directories and writes nothing.
//
// WK12 fills the storage-wipe + cookie-policy half of the profile
// contract.  clear(StorageAreas) maps onto
// webkit_website_data_manager_clear on the profile's session:
// HttpCacheArea -> MEMORY+DISK_CACHE, CookiesArea -> COOKIES,
// DomStorageArea -> SESSION+LOCAL_STORAGE+INDEXEDDB+DOM_CACHE,
// ServiceWorkerArea -> SERVICE_WORKER_REGISTRATIONS, AllAreas -> the
// full WEBKIT_WEBSITE_DATA_ALL set.  VisitedLinksArea has no WebKit
// store (visited-link state is in-process only) — an honest no-op,
// never silently widened.  Cookie policy is applied once at session
// materialization, resolved from the persona's persisted cookies/*
// settings (the same keys CookieJar::loadSettings reads):
// AcceptNever -> ACCEPT_NEVER, AcceptAlways -> ACCEPT_ALWAYS unless
// the blockThirdPartyCookies toggle is set, otherwise (incl. the
// default AcceptOnlyFromSitesNavigatedTo) -> ACCEPT_NO_THIRD_PARTY.
// KeepUntilExit keeps WebKit's memory-only default (no
// set_persistent_storage); the other keep policies point a named
// session's cookie manager at <dataDir>/cookies.sqlite — without
// that call a WebKit session never persists cookies at all.  The
// honest gaps: KeepUntilTimeLimit's 90-day cap is a CookieJar-side
// rewrite WebKitCookieManager cannot express (it persists like
// KeepUntilExpire), and the per-host exception lists stay behind
// the per-attempt cookieFilter contract capabilities() already
// reports false.
//
// WK13 lands the input path: WebkitView's QWidget event overrides
// translate into the wpe-platform event API and WebkitPage dispatches
// through wpe_view_event (WebKit drives its own frame schedule — no
// Servo-style coalescing is needed).  Event coordinates are
// view-space doubles; the view<->widget conversion is the live
// wpe_view-dims/widget-dims ratio, which is 1:1 today because
// resizeEngine mirrors the widget's DIP size onto a scale-1
// toplevel, and stays correct if a HiDPI toplevel scale ever lands.
// Buttons use the WPE_BUTTON_* small ordinals (WK04 measured: NOT
// the evdev BTN_* space), press_count is generated locally from
// Qt's own double-click window (the second down of a dblclick
// arrives as MouseButtonDblClick), and the delivered modifier mask
// follows the GDK convention — a releasing button still travels in
// its own event's mask, cleared only after dispatch.  Wheel maps
// Qt's two delta surfaces onto the two wpe meanings
// (WebEventFactoryWPE.cpp): angleDelta/120 -> precise=false wheel
// TICKS — WPE shares Qt's positive=scroll-up sign (WPEWaylandSeat
// negates scroll-down-positive wl_pointer.axis), so deltas pass
// through un-negated; WebKit scales ticks by pixelsPerLineStep;
// pixelDelta ->
// precise=true pixels, divided by the 2.5 multiplier WebKit bakes
// in for mouse/touchpad sources (the GTK scroll-unit factor) so
// real touchpad pixels stay pixel-faithful.  Keys resolve Qt::Key +
// KeypadModifier through a static table onto XKB keysyms
// (WPE_KEY_* == XK_*), with event->text() supplying the shifted
// level for printables (shift+a -> XK_A) and the standard
// utf32->keysym rule for non-Latin-1; keycode carries the X/XKB
// keycode (evdev+8 — measured: raw evdev maps 8 slots early and DOM
// sees e.g. KeyU for 'a') that KeyboardEvent.code derives from — a
// static table, because offscreen QPA reports no nativeScanCode and
// Qt's value is platform-shaped elsewhere anyway.
//
// WK14 lands zoom/find/lifecycle.  Zoom was already a pass-through
// to webkit_web_view_set_zoom_level (WK08); WK14 adds the
// non-positive guard.  findText maps onto the view's
// WebKitFindController: a fresh needle drives search() with the
// FindFlags -> WebKitFindOptions mapping (case-insensitive is the
// opt-in flag WebKit side — the inverse of FindCaseSensitively;
// WRAP_AROUND always on because Qt find wraps by default and has no
// flag), an identical findText() call steps via
// search_next/search_previous rather than restarting the count, and
// an empty needle is the clear — search_finish.  The async verdict
// arrives on the controller's found-text / failed-to-find-text /
// counted-matches signals; WebKit reports no active-match ordinal,
// so FindResult::activeMatch is tracked locally from the request
// sequence (fresh forward find = 1, backward = the count) — the
// count is authoritative, the ordinal best-effort.
// setLifecycleState maps over what the resolved WPE actually
// exports: webkit_web_view_suspend/resume are OPTIONAL exports —
// absent on 2.54 (header- and nm-verified), resolved through dlsym
// so a newer artifact gets the real suspend without a rebuild.
// With them Frozen suspends the web process outright; without them
// Frozen degrades to wpe_view_unmap + focus_out — "loaded but not
// compositing", the interface's own definition — with JS timers
// honestly still running.  Discarded is
// webkit_web_view_terminate_web_process + reload() on wake: the
// WebProcess dies outright (the spike's session-state round-trip
// degenerates to this — the WebKitBackForwardList is UI-process
// state and survives), and reload respawns it on the current entry.
// Navigation on a discarded page (load/reload/bf jump) revives it —
// the load itself performs the respawn.
//
// WK16 lands script injection, named worlds and the host channel.
// insertScript/removeScript now reach the page's
// WebKitUserContentManager: InjectionPoint maps DocumentCreation ->
// INJECT_AT_DOCUMENT_START and DocumentReady/Deferred ->
// INJECT_AT_DOCUMENT_END (WebKit's only two points — a DocumentReady
// script lands at the same boundary WebKit gives), runsOnSubFrames
// -> INJECT_ALL_FRAMES/TOP_FRAME, and worldId -> the
// WebKitScriptWorld name from scriptWorldName() — world 0 is the
// main world (NULL world_name), Engine::ArktosChannelWorld the
// stable "arktos-channel" name so the SEC24 private-world contract
// holds: page script in the main world can neither see the world's
// objects nor enumerate window.webkit.messageHandlers, which WebKit
// only creates in worlds that have a handler registered.  WebKit
// scopes UserContentManagers per view, never per session, so a
// profile's script collection lands on each page's own UCM —
// applied at page construction and propagated live by WebkitProfile
// to pages that already exist (the QWebEngineScriptCollection
// reach-existing-pages semantics).  runJavaScriptLifted passes the
// mapped world name to evaluate_javascript — lifted eval now truly
// runs in the named world, with one durability caveat WebKit
// imposes: a world name only becomes a persistent execution
// context once a user script anchors it — eval into an unanchored
// name gets a fresh transient context each call, so global state
// set there does not survive between evals.
//
// The host channel: every page registers the "arktosChannel" script
// message handler in the channel world.  The _with_reply variant
// (2.5x API, present on 2.54) is dlsym-gated like the suspend pair
// — with it window.webkit.messageHandlers.arktosChannel
// .postMessage(v) returns a Promise the installed listener's return
// QVariant resolves; without it the one-way handler still delivers
// messages but the promise never exists.  The native side of the
// arktos-channel transport is exactly this: page->native rides the
// handler, native->page rides evaluate_javascript.  The JS-asset
// verdict, honestly: autofill.js's DOM-only fill half runs (and
// captureEnabledForPage already gates the capture half on the
// WebEngine-only bridge); arktos-channel.js, autofill.js's capture
// path and reader.js all key on qt.webChannelTransport +
// QWebChannel, which have no WebKit equivalent — they inject
// inertly and never resolve, so the channel-object surface stays
// gated until a WK transport shim bridges this handler to the
// QWebChannel wire protocol (deliberately not faked here).
//
// WK17 turns decide-policy from observation into the veto contract
// and lands the custom-scheme serving set.  Navigation and
// new-window policy decisions marshal the action into
// Engine::NavigationRequest and consult the owning profile's
// RequestPolicy — main-frame and sub-frame allow/deny, a Block
// verdict ignore()s the decision, a Redirect verdict re-issues the
// load at the verdict's URL (the navigation decision object cannot
// rewrite its request, and a NEW_WINDOW redirect degrades to the
// refusal — honest, like the spike's named referer-mutation gap).
// Response decisions stay observe-only: the subresource lane is
// WK18's content-blocker answer.  External schemes get WebPage's
// SEC02 consent prompt verbatim — the navigation is refused, the
// queued dialog asks, and consent hands the URL to the desktop
// (iframe navs to external schemes are dropped silently).  A
// main-frame file: navigation at a directory reroutes onto the
// arktos-file: listing, matching WebPage's chrome behavior.
//
// webkit_web_context_register_uri_scheme installs the serving set
// once on the shared web context: arktos-file: (the FileAccessHandler
// listing + SEC02 initiator rules — a Referer-scheme check plus the
// recorded main-frame nav target standing in for WebKit's missing
// initiator origin), abp: (the subscribe prompt routed to the
// requesting page's persona AdBlockManager — never served),
// arktos-resource: (the compiled $redirect stub table) and the three
// nonce-backed interstitial carriers (arktos-cert-error:,
// arktos-http-warning:, arktos-site-block:) — every one reusing the
// same engine-agnostic serving halves backend-0's handlers expose.
// Security-manager flags mirror the QWebEngineUrlScheme
// registrations (arktos-file local; stubs secure+cors-enabled;
// interstitials secure).
//
// WK18 lands the subresource interception answer: WebKit has no
// per-request veto, so the policy's rules take the declarative lane
// instead — Engine::RequestPolicy::contentBlockerJson() (the
// AdBlockEnginePolicy override renders the persona's lists through
// the WKB01 Rust converter) is compiled by a process-shared
// WebKitUserContentFilterStore into a WebKitUserContentFilter that
// lands on every page's UserContentManager.  The store is
// content-addressed — the filter identifier is the rules JSON's
// SHA-256, so a store_load hit skips recompiles across runs and a
// rulesChanged re-push of setRequestPolicy() only recompiles when
// the document actually changed.  Scope is honestly narrower than
// the live consult: url-filter/resource-type block + block-cookies +
// css-display-none convert, $redirect degrades to a plain block
// (meta.degraded counts it), and everything without a Safari
// expression lands in the document's dropped[] channel — counted,
// logged, never silently applied.  capabilities() names the lane
// declarativeBlocking; requestInterception stays on because the
// WK17 navigation-level consult is a real live gate.
//
// WK19 lands the user-facing surfaces the caps table had claimed
// ahead of wiring.  load-failed-with-tls-errors carries WebPage's
// SEC06 contract verbatim — the neutral certificateError emission,
// then the nonce-bound arktos-cert-error: interstitial (the same
// publish/serve/resolve machinery; the HTML builder is a WebPage
// static the info struct now feeds), and "proceed" installs the
// certificate on the page's network session through
// allow_tls_certificate_for_host — session-scoped, never persisted,
// with the padlock's red verdict recorded via
// WebPage::noteCertErrorOverride.  "context-menu" marshals the
// WebKitHitTestResult into ContextMenuInfo, emits the new neutral
// contextMenuRequested signal, and the attached view presents
// WebKit's own item model as a Qt popup that activates the items'
// GActions.  "permission-request" maps the concrete request types
// onto SEC05's WebPermissionManager through its new engine-neutral
// resolver overload (geolocation/notifications/pointer-lock prompt;
// media/device-info take the deny list; EME/data-access/XR deny
// outright).  "download-started" wraps WebKitDownload as
// WebkitDownloadRequest — the profile's downloadRequested emits
// inside "decide-destination" (the only point the suggested name and
// response MIME are knowable, and where refusing must cancel rather
// than return FALSE, which would write to a default directory);
// RESPONSE policy decisions answer download for attachments and
// unrenderable MIMEs, and Page::download() drives
// webkit_web_view_download_uri.  Pause/resume stay honest no-ops —
// the API exposes none.  renderProcessId stays -1: WPE 2.54 exposes
// no OS WebProcess pid.

#include "engineinterface.h"

#include <qelapsedtimer.h>
#include <qhash.h>
#include <qimage.h>
#include <qpointer.h>
#include <qset.h>
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
typedef struct _WebKitPolicyDecision WebKitPolicyDecision;
typedef struct _WebKitFindController WebKitFindController;
typedef struct _WebKitUserContentManager WebKitUserContentManager;
typedef struct _WebKitUserContentFilter WebKitUserContentFilter;
typedef struct _WebKitUserContentFilterStore WebKitUserContentFilterStore;
typedef struct _WebKitUserScript WebKitUserScript;
typedef struct _WebKitScriptMessageReply WebKitScriptMessageReply;
typedef struct _WebKitContextMenu WebKitContextMenu;
typedef struct _WebKitHitTestResult WebKitHitTestResult;
typedef struct _WebKitPermissionRequest WebKitPermissionRequest;
typedef struct _WebKitDownload WebKitDownload;
typedef struct _GTlsCertificate GTlsCertificate;
typedef struct _JSCValue JSCValue;

class WebkitPage;
class WebkitProfile;

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
    // WK20 GL-path accounting — frames that came through the
    // dmabuf/EGLImage pipeline and import attempts it declined (the
    // autotest legs pin the fallback contract on these).
    int glFrameCount() const { return m_glFrames; }
    int glImportFails() const { return m_glImportFails; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    // WK13 — the input path.
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private:
    // WK03/WK20 frame adoption: take an already-produced QImage,
    // store it as the latest frame, and schedule a repaint.
    // noteImportFailure counts raster import declines.
    void adoptFrameImage(const QImage &img);
    void noteImportFailure();
    // WK20 adoptGlFrame: like adoptFrameImage + bumps the GL-frame
    // counter the autotest legs inspect.  noteGlImportFailure counts
    // declined GL attempts for the same legs.
    void adoptGlFrame(const QImage &img);
    void noteGlImportFailure();

    // WK13 input helpers — widget-space position -> WPEView
    // coordinate space (the live view/widget dims ratio via the
    // page), press_count generation off Qt's double-click window,
    // and the keyboard dispatch shared by press/release.
    QPointF viewPoint(const QPointF &widgetPos) const;
    int nextPressCount(Qt::MouseButton button, const QPointF &pos);
    void dispatchPointerMove(int type, const QPointF &widgetPos,
                             unsigned mods);
    void dispatchPointerButton(int type, QMouseEvent *event);
    bool dispatchKeyEvent(int type, QKeyEvent *event);

    // WK19 — the context-menu half the signal contract can't carry:
    // WPE's "context-menu" callback arrives without coordinates (the
    // GTK build reads them off the GdkEvent WPE lacks), so the last
    // right-button press position doubles as the menu's anchor.
    void showContextMenu(const Engine::ContextMenuInfo &info);

    QPointer<WebkitPage> m_page;
    QImage m_frame;
    QSize m_frameSize;
    int m_frameCount = 0;
    int m_importFails = 0;
    int m_glFrames = 0;
    int m_glImportFails = 0;
    QPointF m_contextMenuPos;
    // WK13 pointer bookkeeping — the held-buttons half of the
    // delivered WPEModifiers mask (WPE_MODIFIER_POINTER_BUTTON*
    // bits), the last widget-space pointer position (move deltas
    // feed DOM movementX/Y), and the click-generation state for
    // press_count.
    unsigned m_pointerButtonMask = 0;
    QPointF m_lastPointerPos;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    QPointF m_pressPos;
    QElapsedTimer m_pressTimer;
    int m_pressCount = 0;

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

    // WK09: the navigation type WebKit reported for the most recent
    // navigation decision (decide-policy is the only place the enum
    // surfaces).  Other for embedder-initiated load() — WebKit has
    // no Typed.  WK10: back()/forward()/goToHistoryEntry() stamp
    // BackOrForward themselves — decide-policy never fires for
    // back-forward navigations on 2.54.  Autotest-visible state
    // like WebkitView::frameCount.
    Engine::NavigationType lastNavigationType() const
    {
        return m_lastNavigationType;
    }

    // WK14 diagnostics/test surface (the WK11 sessionDataDir role).
    // suspendResumeAvailable() reports the dlsym gate verdict for
    // webkit_web_view_suspend/resume — false on WPE 2.54, where
    // Frozen degrades to the unmap primitive the second accessor
    // exposes.  engineProcessDiscarded() is true while a Discarded
    // page holds a terminated web process pending wake.
    static bool suspendResumeAvailable();
    bool engineViewMapped() const;
    bool engineProcessDiscarded() const { return m_engineDiscarded; }

    // WK16 diagnostics/test surface (the WK11 sessionDataDir role).
    // scriptWorldName() is the worldId -> WebKitScriptWorld-name map
    // insertScript and runJavaScriptLifted share (world 0 -> empty,
    // the default world).  channelHandlerName() is the
    // window.webkit.messageHandlers member registered in the channel
    // world.  scriptMessageReplyAvailable() reports the dlsym gate
    // for register_script_message_handler_with_reply (present on
    // 2.54): false means messages still arrive but postMessage
    // returns no Promise.  setChannelMessageListener installs the
    // native endpoint — its return QVariant is marshaled back as the
    // Promise resolution (invalid -> undefined); with no listener a
    // message is answered with an error rather than a fake ack.
    static QByteArray scriptWorldName(quint32 worldId);
    static QByteArray channelHandlerName();
    static bool scriptMessageReplyAvailable();
    void setChannelMessageListener(
            const std::function<QVariant(const QVariant &)> &listener);

    // WK17 test/diagnostic surface: the profile this page was created
    // with.  The uri-scheme handlers resolve the persona's
    // AdBlockManager through it (the abp: subscribe prompt must reach
    // the requesting page's persona, not the ambient one).
    Engine::Profile *profile() const { return m_profile; }
    // WK17: the URL of the most recently approved main-frame
    // navigation — the uri-scheme handlers match it to authenticate a
    // no-Referer request as the document nav decide-policy approved
    // (the arktos-file: SEC02 initiator check).
    QUrl lastNavTarget() const { return m_lastNavTarget; }

    // WK19 diagnostics/test surface (the WK11 sessionDataDir role).
    // permissionRequestsSeen() counts every "permission-request"
    // signal the page brokered (mapped or denied outright);
    // certErrorPending() is true between the interstitial's
    // publication and its action-link resolution.
    int permissionRequestsSeen() const { return m_permissionSeen; }
    int permissionRequestsGranted() const { return m_permissionGranted; }
    bool certErrorPending() const { return m_certErrorPending; }
    QUrl certErrorUrl() const { return m_certErrorUrl; }

private:
    // GLib signal entry points — reached through the static
    // trampolines in the .cpp (all on the GUI thread: on the glib
    // dispatcher model the "buffer-rendered"/load signals fire while
    // the Qt loop pumps the global-default GMainContext).
    void onBufferRendered(WPEBuffer *buffer);
    void onLoadChanged(int event);
    void onLoadFailed(const QString &failingUri, bool cancelled);
    void onWebProcessTerminated();
    void onUriNotify();
    void onTitleNotify();
    void onProgressNotify();
    void onPageIconsNotify();
    // decide-policy IS WK17's RequestPolicy consult: navigation and
    // new-window actions marshal into NavigationRequest, the owning
    // profile's policy answers Allow/Block/Redirect, and the return
    // value tells the "decide-policy" signal handler whether WebKit's
    // default use-decision was overridden (a policy refusal IGNOREs,
    // a redirect re-issues the load at the verdict's URL — the
    // navigation decision object cannot rewrite its request).
    // Response decisions stay observe-only — the subresource lane is
    // WK18's content-blocker answer.
    bool onDecidePolicy(WebKitPolicyDecision *decision, int type);
    // WK14 — WebKitFindController signal sinks: the terminal verdict
    // (found/failed) plus the final count correction, and the
    // lifecycle primitives behind setLifecycleState (suspend when the
    // export exists, else the unmap freeze) + the discarded-page
    // revive any navigation performs.
    void onFindFound(unsigned matchCount);
    void onFindCounted(unsigned matchCount);
    void onFindFailed();
    void freezeEngine();
    void thawEngine();
    void wakeForNavigation();
    void resizeEngine(int width, int height);
    void focusEngine(bool in);

    // WK16 — the UCM halves of the script contract.  install/
    // removeUserScript translate the Engine::Script triple into a
    // retained WebKitUserScript on the page's content manager (one
    // hash per scope so a page script and a profile script may share
    // a name, matching the two-collection WebEngine shape);
    // install/removeProfileScript are WebkitProfile's propagation
    // entry points.  onChannelMessage answers a script-message
    // through the installed listener (or an error reply when none
    // is), reply == nullptr on the one-way fallback registration.
    void installUserScript(const Engine::Script &script,
                           QHash<QString, WebKitUserScript *> &installed);
    void removeUserScript(const QString &name,
                          QHash<QString, WebKitUserScript *> &installed);
    void installProfileScript(const Engine::Script &script);
    void removeProfileScript(const QString &name);
    void registerChannelHandler();
    void onChannelMessage(JSCValue *value,
                          WebKitScriptMessageReply *reply);

    // WK19 — the user-facing surfaces.  onTlsError claims
    // "load-failed-with-tls-errors": it emits the neutral
    // certificateError (an accepted answer installs the certificate
    // exception on the page's network session and reloads), then
    // publishes WebPage's nonce-bound arktos-cert-error: interstitial
    // whose action links resolveCertificateErrorLink answers
    // (proceed -> session cert exception + reload; back -> history
    // or the start page).  onContextMenu marshals the
    // WebKitContextMenu + hit-test into ContextMenuInfo, emits
    // contextMenuRequested and hands the retained menu to the
    // attached view for presentation.  onPermissionRequest maps the
    // WebKitPermissionRequest concrete type onto the SEC05 broker —
    // unmappable/dangerous types deny outright.
    void onTlsError(const QString &failingUri,
                    GTlsCertificate *certificate, unsigned flags);
    void onContextMenu(WebKitContextMenu *menu,
                       WebKitHitTestResult *hitTestResult);
    void onPermissionRequest(WebKitPermissionRequest *request);
    void resolveCertificateErrorLink(const QUrl &command);

    // WK13 — the wpe_view_event dispatch points.  The callers
    // (WebkitView's event overrides, friend) hand over view-space
    // coordinates and an assembled WPEModifiers mask; these build the
    // WPEEvent and deliver.  viewPoint() performs the widget->view
    // coordinate conversion the view needs.
    QPointF viewPoint(const QPointF &widgetPos) const;
    void sendPointerEvent(int type, double x, double y,
                          double deltaX, double deltaY, unsigned mods);
    void sendButtonEvent(int type, double x, double y,
                         unsigned button, unsigned pressCount,
                         unsigned mods);
    void sendScrollEvent(double x, double y, double deltaX,
                         double deltaY, bool precise, int source,
                         unsigned mods);
    void sendKeyEvent(int type, unsigned keycode, unsigned keyval,
                      unsigned mods);

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
    static void cbNotifyPageIcons(WebKitWebView *view, void *pspec,
                                  WebkitPage *page);
    static int cbDecidePolicy(WebKitWebView *view,
                              WebKitPolicyDecision *decision,
                              int type, WebkitPage *page);
    static void cbFindFound(WebKitFindController *controller,
                            unsigned matchCount, WebkitPage *page);
    static void cbFindCounted(WebKitFindController *controller,
                              unsigned matchCount, WebkitPage *page);
    static void cbFindFailed(WebKitFindController *controller,
                             WebkitPage *page);
    // WK16 — the script-message sinks.  The with-reply signal orders
    // its params (value, reply) and returns gboolean; the one-way
    // signal carries the value alone.  int keeps the trampoline
    // glib-free like cbLoadFailed.
    static void cbScriptMessage(WebKitUserContentManager *manager,
                                JSCValue *value, WebkitPage *page);
    static int cbScriptMessageWithReply(
            WebKitUserContentManager *manager, JSCValue *value,
            WebKitScriptMessageReply *reply, WebkitPage *page);
    // WK19 — the surface sinks.  TLS errors, context-menu and
    // permission-request are boolean claims like load-failed;
    // glib types travel as void* (the cb* contract).
    static int cbLoadFailedTls(WebKitWebView *view, void *uri,
                               void *certificate, unsigned flags,
                               WebkitPage *page);
    static int cbContextMenu(WebKitWebView *view, void *menu,
                             void *hitTestResult, WebkitPage *page);
    static int cbPermissionRequest(WebKitWebView *view, void *request,
                                   WebkitPage *page);

    Engine::Profile *m_profile;
    // WK16: the WebkitProfile this page attached to (when it is one)
    // — QPointer so the dtor's detachPage survives a profile that
    // died first, which raw m_profile could not.
    QPointer<WebkitProfile> m_webProfile;
    QPointer<WebkitView> m_view;
    WebKitWebView *m_webView = nullptr;
    WPEView *m_wpeView = nullptr;   // owned by m_webView — never freed here
    WPEToplevel *m_toplevel = nullptr;
    // WK19: the session the webview was constructed on (borrowed —
    // the webview owns it for the page's life).  The TLS-error
    // proceed path installs certificate exceptions on it.
    WebKitNetworkSession *m_session = nullptr;
    QUrl m_url;
    // WK17: URL of the last main-frame navigation decision this page
    // approved — the uri-scheme handlers' "is this the document nav"
    // authentication (see lastNavTarget()).
    QUrl m_lastNavTarget;
    QString m_title;
    // WK10: uri of an in-flight back/forward navigation — the history
    // machinery emits a spurious CANCELLED load-failed mid-hop, which
    // onLoadFailed suppresses for exactly this uri (2.54-verified).
    QString m_pendingBfNavUri;
    QImage m_iconImage;   // WK09 iconChanged dedup — content-compared
    Engine::NavigationType m_lastNavigationType =
        Engine::NavigationType::Other;
    qreal m_zoom = 1.0;
    bool m_loading = false;
    bool m_progressSeen = false;
    LifecycleState m_lifecycle = LifecycleState::Active;
    // WK14 — find-in-page tracking: the last query so a repeat
    // findText steps instead of re-searching, the locally-tracked
    // active ordinal (WebKit exposes none) and the authoritative
    // match count; plus which lifecycle primitive each state applied
    // (suspend export / unmap freeze / terminated process) so the
    // Active transition knows what to undo.
    QString m_findText;
    Engine::FindFlags m_findFlags;
    int m_findOrdinal = 0;
    int m_findCount = 0;
    bool m_suspended = false;
    bool m_frozenUnmapped = false;
    bool m_engineDiscarded = false;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, bool> m_attributes;
    // WK19 — the pending certificate-error decision (WebPage's
    // m_certError* contract): the nonce the published interstitial's
    // links carry, the refused URL, the retained GTlsCertificate the
    // proceed path installs on the session, and the flags ->
    // overridable answer the interstitial's proceed link is gated on.
    QString m_certErrorNonce;
    QUrl m_certErrorUrl;
    QString m_certErrorHost;
    GTlsCertificate *m_certErrorCert = nullptr;
    bool m_certErrorPending = false;
    bool m_certErrorOverridable = false;
    // WK19 — the WebKitContextMenu retained between "context-menu"
    // and the view's popup (WebKit keeps it alive for the gesture;
    // the ref survives the view reading it on the next tick).
    WebKitContextMenu *m_contextMenu = nullptr;
    // WK19 — permission-request diagnostics the autotest reads.
    int m_permissionSeen = 0;
    int m_permissionGranted = 0;
    // WK16 — the retained WebKitUserScript objects behind the two
    // script scopes (remove_script needs the instance pointer) and
    // the channel endpoint.
    QHash<QString, WebKitUserScript *> m_userScripts;
    QHash<QString, WebKitUserScript *> m_profileUserScripts;
    std::function<QVariant(const QVariant &)> m_channelListener;

    friend class WebkitView;
    friend class WebkitProfile;
    friend class WebkitDownloadRequest;
};

// WK19 — the WebKitDownload -> Engine::DownloadRequest adapter.
// WebKit's decision point is "decide-destination": the suggested
// filename and the response MIME are only knowable once the response
// headers land, and the destination MUST be answered inside the
// signal (returning FALSE falls back to a silent default-directory
// write — forbidden).  The adapter therefore emits the profile's
// downloadRequested from inside onDecideDestination: every
// DownloadItem path (accept, cancel, rustdl handoff) resolves
// synchronously inside the emission, so the handler's return carries
// either an answered destination or a self-cancelled transfer.
// DownloadItem's split-path setters feed set_destination.
// pause()/resume() are honest no-ops — WebKitDownload has no pause
// API; the sidebar shows the state, not a fake toggle.
class WebkitDownloadRequest : public Engine::DownloadRequest
{
    Q_OBJECT

public:
    // Takes a ref on `download` — the profile that created the
    // owning page parents the wrapper so a profile teardown cannot
    // leave a dangling transfer callback.
    WebkitDownloadRequest(WebKitDownload *download, WebkitPage *page,
                          QObject *parent);
    ~WebkitDownloadRequest() override;

    QUrl url() const override;
    QString suggestedFileName() const override;
    QString mimeType() const override;
    void accept(const QString &filePath) override;
    void cancel() override;
    void pause() override;
    void resume() override;
    bool isPaused() const override;
    qint64 receivedBytes() const override;
    qint64 totalBytes() const override;
    State state() const override;
    bool isFinished() const override;
    QString interruptReasonString() const override;
    void setDownloadDirectory(const QString &directory) override;
    void setDownloadFileName(const QString &fileName) override;
    QString downloadDirectory() const override;
    QString downloadFileName() const override;
    Engine::Page *page() const override;

private:
    // The glib signal sinks — decide-destination is the handoff
    // described above; finished/failed retire the wrapper via
    // deleteLater so a mid-signal cancellation can't pull the
    // object out from under its own handler.
    int onDecideDestination(const char *suggestedFileName);
    void onReceivedData();
    void onFinished();
    void onFailed(const QString &message, bool cancelledByUser);
    void setState(State state);
    static int cbDecideDestination(WebKitDownload *download,
                                   void *suggested,
                                   WebkitDownloadRequest *self);
    static void cbReceivedData(WebKitDownload *download,
                               quint64 dataLength,
                               WebkitDownloadRequest *self);
    static void cbFinished(WebKitDownload *download,
                           WebkitDownloadRequest *self);
    static void cbFailed(WebKitDownload *download, void *error,
                         WebkitDownloadRequest *self);

    WebKitDownload *m_download = nullptr;   // retained ref
    QPointer<WebkitPage> m_page;
    QString m_suggested;
    QString m_mime;
    QString m_directory;
    QString m_fileName;
    QString m_interrupt;
    State m_state = State::Requested;
    // The decision resolved inside (or after) decide-destination:
    // accept() sets m_decided, cancel() sets m_cancelled — the
    // post-emission branch picks one.
    bool m_decided = false;
    bool m_cancelled = false;
};

// WK08 — the minimal session object, WK11's storage roots applied.
// WebKitNetworkSession IS the profile object in WPE 2.54 (the
// WEBKIT-SPIKE profile row): an OTR profile gets
// webkit_network_session_new_ephemeral(), a profile carrying a
// storage identity (storageName and/or dataPath) gets
// webkit_network_session_new rooted inside the profile's storage
// tree, and an identity-less profile keeps the shared default
// session.  The session is created lazily on the first page so a
// profile that never renders never spawns one — and a persistent
// profile's directories are not even minted until then.
// Everything else stays the options-bag contract ServoProfile
// serves.
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

    // WK11 diagnostics/test surface: the base directories the
    // session's WebsiteDataManager was rooted at (resolved lazily —
    // empty until the session exists, always empty for the shared
    // default session and for OTR, whose manager owns no dirs).
    QString sessionDataDir() const { return m_sessionDataDir; }
    QString sessionCacheDir() const { return m_sessionCacheDir; }
    // True only once an OTR profile has materialized its ephemeral
    // session — the "writes nothing" guarantee WK11 asserts.
    bool sessionIsEphemeral() const;

    // WK12 diagnostics/test surface (the WK11 sessionDataDir()
    // role).  cookieAcceptPolicy() reports the
    // WebKitCookieAcceptPolicy ordinal the session was configured
    // with (-1 until the session exists or when unlinked);
    // cookieStoragePath() is the file set_persistent_storage was
    // pointed at — empty for OTR, the shared default session and
    // KeepUntilExit personas, whose cookies then live in memory
    // only.  fetchCookies() is an async read-back of the session's
    // WebKitCookieManager store as "name=value" strings — the
    // clear() leg's cookie-manager read-back; on an ephemeral
    // session it answers the memory store.
    int cookieAcceptPolicy() const { return m_cookieAcceptPolicy; }
    QString cookieStoragePath() const { return m_cookieStoragePath; }
    void fetchCookies(
        const std::function<void(const QStringList &)> &callback);

    // WK18 diagnostics/test surface (the WK11 sessionDataDir()
    // role).  contentBlockerReady() is true once the first
    // compile/install pass for the current rules document settled
    // (or an empty document was applied — ready covers both); the
    // counters report the document's meta channel — emitted rules,
    // dropped rules (unconvertible, listed per-rule in the doc) and
    // degraded rules ($redirect -> block).  -1 while no document
    // was ever pulled.
    bool contentBlockerReady() const { return m_cbReady; }
    int contentBlockerEmitted() const { return m_cbEmitted; }
    int contentBlockerDropped() const { return m_cbDropped; }
    int contentBlockerDegraded() const { return m_cbDegraded; }
    // Completed compile/install passes — the refresh leg polls this:
    // a rulesChanged re-push that produced a NEW document bumps it.
    int contentBlockerSyncs() const { return m_cbSyncs; }

private:
    // WK12: one-shot cookie-policy apply at session materialization —
    // reads the persona's persisted cookies/* keys and drives the
    // session's WebKitCookieManager.
    void applyCookiePolicy();

    // WK16: WebKit scopes UserContentManagers per view — the
    // profile's script collection reaches a page through the page's
    // own UCM, so the profile tracks its live pages (attach/detach
    // are the page ctor/dtor's calls) to keep an insertScript or
    // removeScript landing after pages exist in sync.
    void attachPage(WebkitPage *page);
    void detachPage(WebkitPage *page);

    // WK18 — the declarative-blocking lane.  setRequestPolicy()
    // pulls the policy's contentBlockerJson() document (a re-install
    // after rulesChanged is the refresh hook — the app's adblock
    // wiring re-pushes the same policy pointer on every rules
    // change); syncContentBlocker() compiles the document's "rules"
    // array through the shared WebKitUserContentFilterStore and
    // swaps the filter on every attached page's UCM.  A document
    // that arrives while a compile is in flight is folded into a
    // dirty re-pull at finish.  contentFilterCompiled is the async
    // save/load callback's entry point.
    void syncContentBlocker();
    void contentFilterCompiled(const QByteArray &identifier,
                               WebKitUserContentFilter *filter);
    void applyContentFilter(WebKitUserContentFilter *filter,
                            const QByteArray &identifier);
    WebKitUserContentFilter *contentFilter() const { return m_filter; }

    // WK18 — the filter-store async callbacks (static members so the
    // GCallback ABI reaches private state without friend pollution —
    // the page cb* shape; GObject/GAsyncResult travel as void* so
    // the header stays glib-free).  `call` is a heap
    // ContentFilterCall the callbacks own.
    static void cbContentFilterLoad(void *object, void *result,
                                    void *call);
    static void cbContentFilterSave(void *object, void *result,
                                    void *call);

    Engine::ProfileOptions m_options;
    QString m_userAgent;
    Engine::RequestPolicy *m_policy = nullptr;
    std::function<bool(const Engine::CookieAttempt &)> m_cookieFilter;
    QHash<QString, Engine::Script> m_scripts;
    QHash<QString, QVariant> m_attributes;
    WebKitNetworkSession *m_networkSession = nullptr;
    QString m_sessionDataDir;
    QString m_sessionCacheDir;
    int m_cookieAcceptPolicy = -1;
    QString m_cookieStoragePath;
    bool m_cookiePolicyApplied = false;
    QSet<WebkitPage *> m_pages;
    // WK18 — the installed content filter and its bookkeeping: the
    // content-hash identifier of the document that is live (or
    // in-flight), a dirty flag for documents pushed mid-compile, the
    // ready flag the test surface polls, and the doc's meta counts.
    WebKitUserContentFilter *m_filter = nullptr;
    QByteArray m_cbIdentifier;
    bool m_cbCompiling = false;
    bool m_cbDirty = false;
    bool m_cbReady = false;
    int m_cbSyncs = 0;
    int m_cbEmitted = -1;
    int m_cbDropped = -1;
    int m_cbDegraded = -1;

    friend class WebkitPage;
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
