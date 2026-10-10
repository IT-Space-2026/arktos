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

#ifndef ENGINEINTERFACE_H
#define ENGINEINTERFACE_H

// ENG01 — engine-neutral browser-engine interface sketch (UNUSED).
//
// Groundwork for the selectable-engine effort (user directive: the Qt
// UI shell stays, the web engine underneath becomes selectable —
// QtWebEngine/Chromium today, Servo once the ENG03 spike says it is
// viable).  This header sketches the boundary every backend must
// implement.  The complete per-touchpoint coupling map and migration
// notes live in .devin/ENGINE.md.
//
// Design rules:
//  * Only QtCore/QtGui types cross the boundary.  An engine-specific
//    type (QWebEngine*, Servo handle) reaching this file is a smell —
//    wrap it in the backend instead.
//  * QObject + signals: the app's existing wiring style
//    (connect(view, &X::loadFinished, ...)) carries over 1:1 and the
//    QtWebEngine adapter is a thin pass-through.
//  * Async-first: nothing synchronous that a real engine is
//    asynchronous about (DOM, find results, icons, history).
//  * Everything here is a sketch — names and granularity will move
//    when ENG04 does the refactor.  Nothing in the app includes this
//    header yet; engineinterface.cpp exists only to keep it compiled.

#include <qbytearray.h>
#include <qicon.h>
#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qsslcertificate.h>
#include <qstring.h>
#include <qurl.h>
#include <qvariant.h>

#include <functional>

class QAction;
class QWidget;

namespace Engine {

// ---- load / navigation -------------------------------------------------

// Why a load state transition happened.  Mirrors
// QWebEngineLoadingInfo's status — kept tiny because every consumer
// only distinguishes success from failure.
enum class LoadStatus {
    Started,
    Succeeded,
    Failed
};

// Mirrors QWebEnginePage::NavigationType; Servo reports a coarser set
// so LinkClicked/Typed/Redirect are the only guaranteed values.
enum class NavigationType {
    LinkClicked,
    Typed,
    FormSubmitted,
    BackOrForward,
    Reload,
    Redirect,
    Other
};

// What kind of window a page asked to open (POPUP01 relies on the
// popup-shaped values to discriminate window.open from target=_blank).
enum class WebWindowType {
    BrowserWindow,
    BrowserTab,
    BrowserBackgroundTab,
    Dialog
};

// ---- request policy ------------------------------------------------------

// Mirrors QWebEngineUrlRequestInfo::ResourceType.  The adblock rule
// engine and the privacy interceptor are keyed on these values, so the
// enum must survive an engine swap even though the underlying engine
// reports its own classification — each backend maps its type set in.
enum class ResourceType {
    MainFrame = 0,
    SubFrame,
    Stylesheet,
    Script,
    Image,
    FontResource,
    SubResource,
    Object,
    Media,
    Worker,
    SharedWorker,
    Prefetch,
    Favicon,
    Xhr,
    Ping,
    ServiceWorker,
    CspReport,
    PluginResource,
    NavigationPreloadMainFrame,
    NavigationPreloadSubFrame,
    WebSocket,
    Unknown = 255
};

enum class RequestAction {
    Allow,
    Block,
    Redirect
};

// Everything the policy layer may legitimately observe about an
// outgoing request.  Deliberately request-side only — QtWebEngine
// gives embedders no response-header access and a portable interface
// cannot promise it (SEC16C's named gaps).
struct NavigationRequest {
    QUrl url;
    ResourceType resourceType = ResourceType::Unknown;
    NavigationType navigationType = NavigationType::Other;
    QUrl firstPartyUrl;
    QUrl initiatorOrigin;
    bool isMainFrame = false;
    // Fetch metadata the engine surfaces (Sec-Purpose etc.) — the
    // prefetch block (SAFE04) keys on it.
    QByteArray purpose;
};

struct RequestDecision {
    RequestAction action = RequestAction::Allow;
    QUrl redirectUrl;      // action == Redirect
    QByteArray referer;    // optional referer override (REF01 levels)
};

// The app's policy engine (adblock + privacy today; the rustcore
// NavigationPolicy once ENG02 lands).  A backend calls decide() on its
// request path wherever the engine exposes one — engines without a
// request hook report PolicyInterception in Backend::capabilities()
// as false and the app degrades honestly.
class RequestPolicy {
public:
    virtual ~RequestPolicy() = default;
    virtual RequestDecision decide(const NavigationRequest &request) = 0;
};

// ---- cookies / storage ---------------------------------------------------

// The cookie gate (MIG03/SECLVL policy).  Kept as a plain functor
// because WebEngine invokes it on the IO thread — implementations must
// be thread-safe either way.
struct CookieAttempt {
    QUrl firstPartyUrl;
    QUrl origin;
    bool thirdParty = false;
};

// Storage classes ClearPrivateData/SEC12 wipe.  Engine-neutral names;
// each backend maps onto whatever it actually persists.
enum StorageArea {
    HttpCacheArea      = 0x01,
    CookiesArea        = 0x02,
    VisitedLinksArea   = 0x04,
    DomStorageArea     = 0x08,   // localStorage/IndexedDB/etc — engine-bound on WebEngine
    ServiceWorkerArea  = 0x10,
    AllAreas           = 0xff
};
Q_DECLARE_FLAGS(StorageAreas, StorageArea)

// ---- script injection ------------------------------------------------------

enum class InjectionPoint {
    DocumentCreation,
    DocumentReady,
    Deferred
};

// Engine-neutral QWebEngineScript.  `worldId` 0 = the page's main
// world, anything else is an isolated world — a backend without world
// isolation (none is guaranteed) reports it via capabilities().
struct Script {
    QString name;
    QString sourceCode;
    InjectionPoint injectionPoint = InjectionPoint::Deferred;
    quint32 worldId = 0;
    bool runsOnSubFrames = false;
};

// ---- find-in-page ------------------------------------------------------------

enum FindFlag {
    FindBackward    = 0x01,
    FindCaseSensitively = 0x02
};
Q_DECLARE_FLAGS(FindFlags, FindFlag)

// Mirrors QWebEngineFindTextResult.
struct FindResult {
    int numberOfMatches = 0;
    int activeMatch = 0;   // 1-based on QtWebEngine 6.12 (empirically)
};

// ---- permissions ---------------------------------------------------------------

// Union of the permission types the SEC05 broker handles plus the
// obvious engine-neutral rest.  Backend maps its own permission set;
// anything unmapped arrives as Unsupported and the broker denies it.
enum class PermissionType {
    Notifications,
    Geolocation,
    MediaAudioCapture,
    MediaVideoCapture,
    MediaAudioVideoCapture,
    MouseLock,
    DesktopVideoCapture,
    DesktopAudioVideoCapture,
    ClipboardReadWrite,
    LocalFontsAccess,
    Unsupported
};

// ---- certificate errors ----------------------------------------------------------

// Engine-neutral summary of a certificate failure (SEC06).  `error` is
// the backend's numeric code — the adapter maps it to a symbolic name
// for display; `overridable` gates the proceed affordance exactly like
// QWebEngineCertificateError::isOverridable.
struct CertificateErrorInfo {
    QUrl url;
    int error = 0;
    QString description;
    bool overridable = false;
    bool isMainFrame = true;
    QList<QSslCertificate> certificateChain;
};

enum class CertificateErrorAction {
    Reject,
    AcceptOnce     // session-scoped, never persisted (SEC06 rule)
};

// ---- downloads ---------------------------------------------------------------------

class Page;

// Handle a backend hands the app when a download starts (MIG05's
// QWebEngineDownloadRequest shape — every engine offers roughly this).
class DownloadRequest : public QObject {
    Q_OBJECT

public:
    explicit DownloadRequest(QObject *parent = nullptr) : QObject(parent) {}

    // Mirrors QWebEngineDownloadRequest::DownloadState.
    enum class State {
        Requested,
        InProgress,
        Completed,
        Cancelled,
        Interrupted
    };
    Q_ENUM(State)

    virtual QUrl url() const = 0;
    virtual QString suggestedFileName() const = 0;
    virtual QString mimeType() const = 0;
    virtual void accept(const QString &filePath) = 0;
    virtual void cancel() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual qint64 receivedBytes() const = 0;
    virtual qint64 totalBytes() const = 0;
    virtual State state() const = 0;
    virtual bool isFinished() const = 0;
    virtual QString interruptReasonString() const = 0;
    // Split target-path setters — DownloadItem picks the name through
    // its own policy dialog before accept().
    virtual void setDownloadDirectory(const QString &directory) = 0;
    virtual void setDownloadFileName(const QString &fileName) = 0;
    // The page the request originated from — nullptr when the engine
    // no longer attributes one (e.g. a page that already closed).
    virtual Page *page() const = 0;

signals:
    void stateChanged(Engine::DownloadRequest::State state);
    void receivedBytesChanged();
    void totalBytesChanged();
};

// ---- standard actions --------------------------------------------------------------

// Chrome actions the engine owns — menu/toolbar entries whose enabled
// and checked state mirrors the engine's own QAction (WebActionMapper
// keeps the two in sync).  Mirrors QWebEnginePage::WebAction; a
// backend may leave an entry unmapped and return nullptr from
// Page::action(), which chrome must read as "disabled".
enum class StandardAction {
    Back, Forward, Stop, Reload, ReloadAndBypassCache,
    Cut, Copy, Paste, Undo, Redo, SelectAll, PasteAndMatchStyle,
    OpenLinkInThisWindow, OpenLinkInNewWindow, OpenLinkInNewTab,
    CopyLinkToClipboard, DownloadLinkToDisk,
    CopyImageToClipboard, CopyImageUrlToClipboard, DownloadImageToDisk,
    CopyMediaUrlToClipboard, DownloadMediaToDisk,
    InspectElement, ExitFullScreen, RequestClose, Unselect,
    SavePage, ViewSource
};

// ---- context menus ------------------------------------------------------------------

// Minimal context-menu payload (webview.cpp feeds on
// QWebEngineContextMenuRequest — only the fields the menu actually
// consumes are surfaced).
struct ContextMenuInfo {
    QPoint position;
    QUrl linkUrl;
    QString linkText;
    QUrl mediaUrl;
    QString selectedText;
    QUrl pageUrl;
    bool isContentEditable = false;
    // media/link classification — the "media type" enum collapses to
    // flags since only Image/Video/Audio/Canvas drive menu entries.
    bool hasImage = false;
    bool hasMedia = false;
    bool isCanvas = false;
};

// ---- page ----------------------------------------------------------------------------

// One entry in a page's session history (QWebEngineHistoryItem's
// url/title — icons resolve through the app's icon store, not the
// engine).  `index` is the entry's position in the linear stack;
// Page::goToHistoryEntry() consumes it.
struct HistoryEntry {
    QUrl url;
    QString title;
    int index = -1;
};

// One document's worth of engine.  The WebEngine adapter is a thin
// subclass around QWebEnginePage; a Servo backend drives its WebView
// delegate callbacks into the same signals.
class Page : public QObject {
    Q_OBJECT

public:
    explicit Page(QObject *parent = nullptr) : QObject(parent) {}

    // lifecycle / navigation
    virtual void load(const QUrl &url) = 0;
    virtual void stop() = 0;
    virtual void reload() = 0;
    virtual QUrl url() const = 0;

    // history navigation
    virtual bool canGoBack() const = 0;
    virtual bool canGoForward() const = 0;
    virtual void back() = 0;
    virtual void forward() = 0;

    // The page's session history, QWebEngineHistory-shaped: a linear
    // stack with a current index.  backItems()/forwardItems() return
    // up to maxItems entries in stack order (oldest first), and
    // goToHistoryEntry() jumps to an entry the lists produced.
    virtual int historyCount() const = 0;
    virtual int currentHistoryIndex() const = 0;
    virtual QList<HistoryEntry> historyItems() const = 0;
    virtual QList<HistoryEntry> backItems(int maxItems) const = 0;
    virtual QList<HistoryEntry> forwardItems(int maxItems) const = 0;
    virtual void goToHistoryEntry(const HistoryEntry &entry) = 0;

    // zoom
    virtual void setZoomFactor(qreal factor) = 0;
    virtual qreal zoomFactor() const = 0;

    // find-in-page — async on every real engine
    virtual void findText(const QString &subString, FindFlags options) = 0;

    // script evaluation — the autofill/reader/PiP/fingerprint paths
    // all reduce to this
    virtual void runJavaScript(const QString &source,
                               const std::function<void(const QVariant &)> &resultCallback
                                   = std::function<void(const QVariant &)>()) = 0;

    // Serialized page source for "View Source" — async on every real
    // engine (Chromium pulls the DOM out of the render process).
    virtual void toHtml(
            const std::function<void(const QString &)> &resultCallback) = 0;

    // The engine-owned QAction behind a standard chrome entry —
    // nullptr when the backend has no equivalent (chrome disables the
    // entry rather than synthesizing one).
    virtual QAction *action(StandardAction action) = 0;

    // SLEEP01's suspend gate consults these; an engine without the
    // concept answers false so the gate treats the page as quiet.
    virtual bool isLoading() const = 0;
    virtual bool recentlyAudible() const = 0;

    // Whether the page's profile is off-the-record — the per-tab
    // privacy gates (history, icon store, autofill) key on it.
    virtual bool isOffTheRecord() const = 0;

    // per-page attributes (JSCTL's JavascriptEnabled flip, JS can-open-
    // windows for the popup gate).  Keyed loosely so each backend maps
    // what it supports; unknown keys are ignored.
    virtual void setPageAttribute(const QString &name, bool on) = 0;

    // SLEEP01 suspend/wake.  ActiveFrozen is "loaded but not
    // compositing"; Discarded unloads content entirely.
    enum class LifecycleState { Active, Frozen, Discarded };
    virtual void setLifecycleState(LifecycleState state) = 0;
    virtual LifecycleState lifecycleState() const = 0;

    // SBAR01's memory widget needs a per-tab process identity.  On
    // QtWebEngine this is QWebEnginePage::renderProcessPid; a backend
    // without per-page processes returns -1 and the widget shows '—'.
    virtual qint64 renderProcessId() const = 0;

    // POPUP01/tab creation — the engine asks the app for a new page.
    // Returning nullptr refuses the window.
    virtual Page *createWindow(WebWindowType type) = 0;

    // Trusted chrome-script injection for callers whose work must
    // still reach a page while page scripting is off (JSCTL/SECLVL
    // block): reader-mode's driver, PiP's pop-out, the context-image
    // resolver and the storage wipes all need it.  The backend lifts
    // its script gate for the injection window only — page scripts
    // refused at parse time do not retro-run while the gate is up.
    // A backend with no script gate just runs runJavaScript.
    virtual void runJavaScriptLifted(const QString &source,
            const std::function<void(const QVariant &)> &resultCallback
                = std::function<void(const QVariant &)>()) = 0;

    // Per-page user-script collection (adblock cosmetic, autofill) —
    // the same Script triple Profile carries, scoped to one document.
    virtual void insertScript(const Script &script) = 0;
    virtual void removeScript(const QString &name) = 0;
    virtual QList<Script> scripts() const = 0;

    // Ask the engine to fetch url as a download — QWebEnginePage::
    // download's neutral spelling.  Downloads surface back through
    // Profile::downloadRequested.
    virtual void download(const QUrl &url) = 0;

    // The widget currently presenting the page — the engine's
    // forPage reverse lookup.  nullptr while the page is headless or
    // detached (an engine may attach more than one view; the backend
    // answers with the primary one).
    virtual QWidget *view() const = 0;

signals:
    void loadStarted();
    void loadProgress(int progress);
    void loadFinished(bool ok);
    void urlChanged(const QUrl &url);
    void titleChanged(const QString &title);
    void iconChanged(const QIcon &icon);
    // Per-page chrome signals the sketch missed — every engine can
    // emit them (ENG04: TabWidget's tab-label plumbing, SBAR01's
    // memory widget, and the print/close paths consume these).
    void linkHovered(const QString &url);
    void windowCloseRequested();
    void printRequested();
    void renderProcessIdChanged(qint64 pid);
    void findTextFinished(const Engine::FindResult &result);
    void contextMenuRequested(const Engine::ContextMenuInfo &info);
    void certificateError(const Engine::CertificateErrorInfo &error,
                          bool *accepted /* out-param on the signal is
                              sketch shorthand — the real interface may
                              prefer a decision callback */);
    void permissionRequested(const QUrl &origin,
                             Engine::PermissionType type,
                             bool *granted);
    void windowRequested(Engine::WebWindowType type);
};

// ---- profile --------------------------------------------------------------------------

// An engine's browsing context: cookie jar, cache, storage, settings,
// the request-policy hook and the download handoff.  Backed today by
// QWebEngineProfile ("arora" named + unnamed OTR + tor OTR + per-
// container); containers (CONT01) are just Profiles to the app.
class Profile : public QObject {
    Q_OBJECT

public:
    explicit Profile(QObject *parent = nullptr) : QObject(parent) {}

    virtual bool isOffTheRecord() const = 0;
    virtual QString storageName() const = 0;

    // UA01/UA03 user agent.
    virtual void setUserAgent(const QString &userAgent) = 0;

    // The request-policy hook (ENG02's NavigationPolicy lives behind
    // it).  Engines without interception leave this unimplemented and
    // advertise so via Backend::capabilities().
    virtual void setRequestPolicy(RequestPolicy *policy) = 0;
    virtual RequestPolicy *requestPolicy() const = 0;

    // The cookie gate — the app's CookieJar equivalent.  Same
    // thread-safety contract as RequestPolicy.
    virtual void setCookieFilter(
            const std::function<bool(const CookieAttempt &)> &filter) = 0;

    // Script collection (autofill, adblock cosmetic, fingerprint,
    // referrer meta, user stylesheet).
    virtual void insertScript(const Script &script) = 0;
    virtual void removeScript(const QString &name) = 0;
    virtual QList<Script> scripts() const = 0;

    // Storage wipes (SEC12).
    virtual void clear(StorageAreas areas) = 0;

    // Free-form per-profile attribute bag for the settings apply path
    // (BrowserProfile::applySettings) — engine-specific attributes are
    // wrapped in the backend's own key namespace so a second backend
    // ignores them harmlessly instead of misreading them.
    virtual void setProfileAttribute(const QString &name, const QVariant &value) = 0;

signals:
    // The download handoff — the backend emits, DownloadManager
    // routes.
    void downloadRequested(Engine::DownloadRequest *request);
};

// ---- backend -----------------------------------------------------------------------------

struct ProfileOptions {
    bool offTheRecord = false;
    QString storageName;        // empty = engine default
    QString dataPath;           // persistent root (containers)
    QString cachePath;
};

// Capability bits the app consults to light up or hide features —
// honest-degradation plumbing so a Servo tab never fakes a surface it
// lacks (Tor stays Chromium per ENG05).
struct Capabilities {
    bool requestInterception = false;  // setRequestPolicy actually gates traffic
    bool scriptInjection = false;      // insertScript reaches pages
    bool scriptEvaluation = false;     // runJavaScript returns results
    bool cookieFilter = false;         // setCookieFilter is honored
    bool perPageSettings = false;      // setPageAttribute is per-page, not global
    bool lifecycleDiscard = false;     // Discarded frees resources
    bool contextMenuInfo = false;      // contextMenuRequested carries payload
    bool certificateOverride = false;  // cert-error decisions are accepted
    bool devTools = false;             // an inspectable debug channel exists
    bool extensions = false;           // WebExtension-style add-ons
    bool downloads = true;             // download handoff exists
};

// The engine factory + process-wide hooks (scheme registration,
// process flags, dns mode — the pre-QApplication work main() does
// today becomes backend->initialize()).
class Backend : public QObject {
    Q_OBJECT

public:
    explicit Backend(QObject *parent = nullptr) : QObject(parent) {}

    virtual QString id() const = 0;            // "webengine" | "servo" | ...
    virtual QString displayName() const = 0;   // "Chromium (QtWebEngine)"
    virtual Capabilities capabilities() const = 0;

    // Engine-level init/shutdown that must run outside page lifetime
    // (QtWebEngine: custom-scheme registration pre-QApplication,
    // QTWEBENGINE_CHROMIUM_FLAGS, QWebEngineGlobalSettings::setDnsMode).
    virtual bool initialize() = 0;

    virtual Profile *createProfile(const ProfileOptions &options,
                                   QObject *parent = nullptr) = 0;
    virtual Page *createPage(Profile *profile, QObject *parent = nullptr) = 0;

    // The painted surface — a QWidget so existing tab/chrome code
    // hosts any backend; QtWebEngine returns a QWebEngineView, Servo
    // would return the spike's QOpenGLWidget host.
    virtual QWidget *createView(Page *page, QWidget *parent = nullptr) = 0;
};

} // namespace Engine

// Signal payloads need metatypes for queued delivery + QSignalSpy.
Q_DECLARE_METATYPE(Engine::StandardAction)
Q_DECLARE_METATYPE(Engine::FindResult)
Q_DECLARE_METATYPE(Engine::ContextMenuInfo)
Q_DECLARE_METATYPE(Engine::CertificateErrorInfo)

Q_DECLARE_OPERATORS_FOR_FLAGS(Engine::StorageAreas)
Q_DECLARE_OPERATORS_FOR_FLAGS(Engine::FindFlags)

#endif // ENGINEINTERFACE_H
