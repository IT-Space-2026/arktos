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

#include "webkitbackend.h"

// WK17: adblockmanager.h, browserapplication.h and webpage.h use
// Qt's keyword macros (signals:/slots:), which the undef block below
// puts out of scope for the WPE headers — include them here, above
// the undefs.
#include "adblockmanager.h"
#include "browserapplication.h"
#include "webpage.h"
#include "webpermissionmanager.h"

// glib/WPE headers declare a struct member named `signals` — Qt's
// keyword macros must be out of the way for the include (the harness
// used no_keywords; here the undef is scoped to this TU so the rest
// of the app keeps the signal/slot spellings — use the Q_EMIT forms
// below the include if this file ever needs them).
#ifdef ARKTOS_WEBKIT_LINKED
#ifdef signals
#undef signals
#endif
#ifdef slots
#undef slots
#endif
#ifdef emit
#undef emit
#endif
#include <jsc/jsc.h>
#include <wpe/webkit.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/wpe-platform.h>
#include <dlfcn.h>
#endif
// WK20 — the dmabuf/EGLImage import path needs system EGL/GLESv2.
// Only compiled when pkg-config resolved egl+glesv2 (engine.pri sets
// ARKTOS_WEBKIT_GL) — without dev headers the backend still builds
// and simply never leaves the raster path.
#if defined(ARKTOS_WEBKIT_LINKED) && defined(ARKTOS_WEBKIT_GL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <unistd.h>
#endif

#include "adblockresourcehandler.h"
#include "adblockschemeaccesshandler.h"
#include "browserpaths.h"
#include "engineregistry.h"
#include "fileaccesshandler.h"
#include "profilesettings.h"
#include "schemeaccesshandler.h"

#include <qapplication.h>
#include <qcryptographichash.h>
#include <qdir.h>
#include <qevent.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qpixmap.h>
#include <qpointer.h>
#include <qsslcertificate.h>
#include <qstandardpaths.h>
#include <qtimer.h>
#include <qurlquery.h>
#include <quuid.h>

#include <cstring>

// ---- shared WebKit-side helpers (linked builds only) ---------------

#ifdef ARKTOS_WEBKIT_LINKED

// WK17 — the custom-scheme serving set (register_uri_scheme) and the
// web_view -> page map the scheme handlers resolve personas and
// initiators through; defined at file scope further down where the
// shared serving adapters and the persona helpers live.
static void installUriSchemeHandlers(WebKitWebContext *context);
static QHash<WebKitWebView *, WebkitPage *> &schemePageMap();
// SEC02 external-scheme consent — the decide-policy consult refuses
// external-scheme navigations and hands the URL to WebPage's shared
// queued prompt (promptOpenExternalUrl).
// WK19 — the download channel lives on the WebKitNetworkSession:
// connect "download-started" once per session (the default session
// is shared across identity-less profiles — the hook is idempotent).
static void ensureDownloadHook(WebKitNetworkSession *session);

namespace {

// The default WebKitWebContext carries the process-global config —
// including the bwrap sandbox whitelist this prefix needs (WK02:
// without add_path_to_sandbox the inner execvp cannot see the sid
// glibc/prefix and the web process dies on spawn).  Applied once;
// WebKitWebContext itself is a singleton the whole process shares.
WebKitWebContext *sharedWebContext()
{
    static WebKitWebContext *context = [] {
        WebKitWebContext *ctx = webkit_web_context_get_default();
        const QString prefix = WebkitBackend::wpePrefix();
        if (!prefix.isEmpty()) {
            const char *dirs[] = { "/wpe-webkit-2.0",
                                   "/usr/lib/x86_64-linux-gnu",
                                   "/glibc/usr/lib/x86_64-linux-gnu",
                                   "/compat" };
            for (const char *d : dirs) {
                const QString path = prefix + QString::fromUtf8(d);
                webkit_web_context_add_path_to_sandbox(
                    ctx, path.toUtf8().constData(), TRUE);
            }
        }
        // WK17 — the custom-scheme serving set registers here, once,
        // at web-context creation — the WPE analog of the
        // pre-application QWebEngineUrlScheme registrations.
        installUriSchemeHandlers(ctx);
        return ctx;
    }();
    return context;
}

// WK18 — the process-shared WebKitUserContentFilterStore.  Filters
// are content-addressed: WebkitProfile names each rules document by
// its SHA-256, so one store under the app's cache root serves every
// persona and every profile, and a store_load hit reuses WebKit's
// own compiled-blob cache instead of reparsing the JSON.  The store
// root is disposable cache — a wipe costs one recompile, nothing
// more.  (OTR profiles ride the same store: the blobs are RULE
// compilations, not browsing state — nothing about a session ever
// lands here.)
WebKitUserContentFilterStore *contentFilterStore()
{
    static WebKitUserContentFilterStore *store = [] {
        const QString dir = BrowserPaths::dataRoot()
            + QLatin1String("/cache/WebKitContentFilters");
        QDir().mkpath(dir);
        return webkit_user_content_filter_store_new(
            QFile::encodeName(dir).constData());
    }();
    return store;
}

// The async hop for store_load -> (miss) store_save -> install.
// WebKit's content-blocker compiler runs off the GUI thread; the
// callbacks land on the same global-default main context the Qt
// loop pumps.  The QPointer guards a profile that died mid-compile;
// `source` is carried so the load miss can re-issue as a save
// without another round trip through the profile.
struct ContentFilterCall {
    QPointer<WebkitProfile> profile;
    QByteArray identifier;
    QByteArray source;
};

// JSCValue -> QVariant (WK15 marshaling matrix).  Scalars map
// directly; JS null becomes a valid-but-null variant (the
// QMetaType::Nullptr spelling QJsonValue::toVariant uses — valid()
// distinguishes it) while JS undefined maps to the invalid QVariant;
// objects/arrays hand back their JSON text form — the documented
// marshaling limit vs WebEngine's full QVariantMap/List decode.
QVariant jscValueToVariant(JSCValue *value)
{
    if (!value || jsc_value_is_undefined(value))
        return QVariant();
    if (jsc_value_is_null(value))
        return QVariant::fromMetaType(QMetaType::fromType<std::nullptr_t>());
    if (jsc_value_is_boolean(value))
        return QVariant(bool(jsc_value_to_boolean(value)));
    if (jsc_value_is_number(value))
        return QVariant(jsc_value_to_double(value));
    if (jsc_value_is_string(value)) {
        gchar *text = jsc_value_to_string(value);
        const QString out = QString::fromUtf8(text ? text : "");
        g_free(text);
        return out;
    }
    gchar *json = jsc_value_to_json(value, 0);
    const QString out = QString::fromUtf8(json ? json : "");
    g_free(json);
    return out;
}

// evaluate_javascript is async on the same main context the Qt loop
// already pumps; the heap block keeps the callback alive across the
// hop.  The page pointer only guards a debug log — the result
// callback must fire even if the page died first (its caller may be
// parked waiting).
struct EvalCall {
    QPointer<WebkitPage> page;
    std::function<void(const QVariant &)> callback;
};

void evalReady(GObject *object, GAsyncResult *result, gpointer data)
{
    EvalCall *call = static_cast<EvalCall *>(data);
    GError *error = nullptr;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    QVariant out;
    if (value) {
        out = jscValueToVariant(value);
        g_object_unref(value);
    } else if (error) {
        qWarning("webkit: runJavaScript failed: %s", error->message);
    }
    g_clear_error(&error);
    if (call->callback)
        call->callback(out);
    delete call;
}

// WK09 — WebKitNavigationType -> Engine::NavigationType.  WebKit's
// set is coarser than Chromium's: embedder-initiated load() arrives
// as OTHER (there is no Typed), FORM_RESUBMITTED folds into
// FormSubmitted, and server-side redirects produce no navigation
// action at all — is_redirect is what distinguishes those hops, so
// it wins over the enum.  decide-policy is the only place the enum
// surfaces; WK17 reuses this mapping for NavigationRequest.
Engine::NavigationType engineNavigationType(WebKitNavigationType type,
                                            bool isRedirect)
{
    if (isRedirect)
        return Engine::NavigationType::Redirect;
    switch (type) {
    case WEBKIT_NAVIGATION_TYPE_LINK_CLICKED:
        return Engine::NavigationType::LinkClicked;
    case WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED:
    case WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED:
        return Engine::NavigationType::FormSubmitted;
    case WEBKIT_NAVIGATION_TYPE_BACK_FORWARD:
        return Engine::NavigationType::BackOrForward;
    case WEBKIT_NAVIGATION_TYPE_RELOAD:
        return Engine::NavigationType::Reload;
    default:
        return Engine::NavigationType::Other;
    }
}

// WK09 — WebKitImage (the 2.54 GLib image class) -> QImage.  The
// bytes are 32-bit premultiplied-alpha in the platform's preferred
// byte order — BGRA8888 on little-endian, byte-identical to
// Format_ARGB32_Premultiplied (the same contract the WK03 buffer
// import already relies on).  The returned GBytes is borrowed from
// the image, so rows are deep-copied like wkRasterImage.
QImage imageFromWebKitImage(WebKitImage *image)
{
    if (!image)
        return QImage();
    const int w = webkit_image_get_width(image);
    const int h = webkit_image_get_height(image);
    const int stride = int(webkit_image_get_stride(image));
    GBytes *bytes = webkit_image_as_bytes(image);
    gsize len = 0;
    const void *data = bytes ? g_bytes_get_data(bytes, &len) : nullptr;
    if (!data || w <= 0 || h <= 0 || stride < w * 4
            || qint64(len) < qint64(stride) * (h - 1) + w * 4)
        return QImage();
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y)
        memcpy(out.scanLine(y),
               static_cast<const quint8 *>(data) + size_t(stride) * y,
               size_t(w) * 4);
    return out;
}

// WK03 raster import, factored so WK20's channel-order calibration
// can hold the image for comparison.  The returned GBytes is OWNED BY
// THE BUFFER (borrowed, never unrefed); rows are deep-copied honoring
// the shm stride.
QImage wkRasterImage(WPEBuffer *buffer)
{
    GError *error = nullptr;
    GBytes *pixels = wpe_buffer_import_to_pixels(buffer, &error);
    if (!pixels) {
        qWarning("webkit: wpe_buffer_import_to_pixels failed (%s): %s",
                 G_OBJECT_TYPE_NAME(buffer),
                 error ? error->message : "?");
        g_clear_error(&error);
        return QImage();
    }
    const int bw = wpe_buffer_get_width(buffer);
    const int bh = wpe_buffer_get_height(buffer);
    int stride = bw * 4;
    if (WPE_IS_BUFFER_SHM(buffer))
        stride = int(wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer)));
    gsize len = 0;
    const void *data = g_bytes_get_data(pixels, &len);
    if (stride <= 0 || bw <= 0 || bh <= 0
            || qint64(len) < qint64(stride) * (bh - 1) + bw * 4) {
        qWarning("webkit: imported pixels undersized: %zu for %dx%d "
                 "stride=%d", size_t(len), bw, bh, stride);
        return QImage();
    }
    QImage img(bw, bh, QImage::Format_ARGB32_Premultiplied);
    const quint8 *src = static_cast<const quint8 *>(data);
    for (int y = 0; y < bh; ++y)
        memcpy(img.scanLine(y), src + size_t(stride) * y,
               size_t(bw) * 4);
    return img;
}

// Largest icon out of the list — favicon candidates arrive ordered
// but area-picking is cheap and correct for the tab strip.
QImage bestPageIcon(WebKitImageList *list)
{
    if (!list)
        return QImage();
    const gsize count = webkit_image_list_get_length(list);
    QImage best;
    for (gsize i = 0; i < count; ++i) {
        const QImage img =
            imageFromWebKitImage(webkit_image_list_get(list, i));
        if (qint64(img.width()) * img.height()
                > qint64(best.width()) * best.height())
            best = img;
    }
    return best;
}

// ---- WK10 — the real WebKitBackForwardList --------------------------
//
// WebKit keeps a per-view back-forward list with a current item and
// two slices.  Runtime-verified ordering on 2.54 (the GLib wrapper
// prepends while building, so both slices arrive reversed relative
// to stack order): get_back_list is closest-to-current FIRST and
// get_forward_list is furthest-from-current FIRST.  The Page
// contract wants a linear stack ordered oldest→newest, so BOTH
// slices walk tail-to-head around the current item.
// The returned GLists are borrowed-item copies — g_list_free the
// nodes, never unref the items; the list itself is owned by the
// WebKitWebView.
struct BackForwardShape {
    int backCount = 0;
    int forwardCount = 0;
    bool hasCurrent = false;
};

BackForwardShape backForwardShape(WebKitWebView *view)
{
    BackForwardShape shape;
    WebKitBackForwardList *list =
        view ? webkit_web_view_get_back_forward_list(view) : nullptr;
    if (!list)
        return shape;
    shape.hasCurrent =
        webkit_back_forward_list_get_current_item(list) != nullptr;
    GList *back = webkit_back_forward_list_get_back_list(list);
    GList *forward = webkit_back_forward_list_get_forward_list(list);
    shape.backCount = int(g_list_length(back));
    shape.forwardCount = int(g_list_length(forward));
    g_list_free(back);
    g_list_free(forward);
    return shape;
}

// The item's committed (post-redirect) uri — original_uri keeps the
// pre-redirect target.
QString bfItemUri(WebKitBackForwardListItem *item)
{
    if (!item)
        return QString();
    const char *uri = webkit_back_forward_list_item_get_uri(item);
    return QString::fromUtf8(uri ? uri : "");
}

// uri + the stored title + the stack index the caller's loop
// assigned.
Engine::HistoryEntry entryFromBfItem(WebKitBackForwardListItem *item,
                                   int index)
{
    Engine::HistoryEntry entry;
    entry.url = QUrl(bfItemUri(item));
    const char *title = webkit_back_forward_list_item_get_title(item);
    entry.title = QString::fromUtf8(title ? title : "");
    entry.index = index;
    return entry;
}

// WK12 — clear() is fire-and-forget in the interface (WebEngine's
// clear* calls are async under the hood too), but a failed wipe must
// be loud in the log rather than silently keeping state.
void clearFinished(GObject *object, GAsyncResult *result, gpointer)
{
    GError *error = nullptr;
    if (!webkit_website_data_manager_clear_finish(
            WEBKIT_WEBSITE_DATA_MANAGER(object), result, &error))
        qWarning("webkit: website-data clear failed: %s",
                 error ? error->message : "?");
    g_clear_error(&error);
}

// WK12 — the fetchCookies heap block keeping the callback alive
// across the async get_all_cookies hop (the EvalCall shape).  The
// result fires even on error — a parked caller must never hang.
struct FetchCookiesCall {
    std::function<void(const QStringList &)> callback;
};

void cookiesReady(GObject *object, GAsyncResult *result, gpointer data)
{
    FetchCookiesCall *call = static_cast<FetchCookiesCall *>(data);
    GError *error = nullptr;
    GList *cookies = webkit_cookie_manager_get_all_cookies_finish(
        WEBKIT_COOKIE_MANAGER(object), result, &error);
    if (error) {
        qWarning("webkit: cookie read-back failed: %s", error->message);
        g_clear_error(&error);
    }
    QStringList out;
    for (GList *l = cookies; l; l = l->next) {
        SoupCookie *cookie = static_cast<SoupCookie *>(l->data);
        const char *name = soup_cookie_get_name(cookie);
        const char *value = soup_cookie_get_value(cookie);
        out.append(QString::fromUtf8(name ? name : "")
                   + QLatin1Char('=')
                   + QString::fromUtf8(value ? value : ""));
    }
    g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
    if (call->callback)
        call->callback(out);
    delete call;
}

// ---- WK13 — Qt -> WPE event translation -----------------------------

// Event timestamps are monotonic ms — the WK04-proven clock.
guint32 wpeEventTime()
{
    return guint32(g_get_monotonic_time() / 1000);
}

// Qt keyboard modifiers -> the WPEModifiers flag set.  The
// pointer-button half is tracked by the view and OR-ed in at
// dispatch time.
unsigned wpeKeyboardMods(Qt::KeyboardModifiers qt)
{
    unsigned mods = 0;
    if (qt & Qt::ControlModifier)
        mods |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if (qt & Qt::ShiftModifier)
        mods |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if (qt & Qt::AltModifier)
        mods |= WPE_MODIFIER_KEYBOARD_ALT;
    if (qt & Qt::MetaModifier)
        mods |= WPE_MODIFIER_KEYBOARD_META;
    return mods;
}

// Qt::MouseButton -> the WPE_BUTTON_* small ordinal (WK04 measured:
// NOT the evdev BTN_* space).  Side buttons take the ordinals GTK
// passes through for extra buttons; 0 = unmappable.
unsigned wpeButtonOrdinal(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:    return WPE_BUTTON_PRIMARY;
    case Qt::MiddleButton:  return WPE_BUTTON_MIDDLE;
    case Qt::RightButton:   return WPE_BUTTON_SECONDARY;
    case Qt::BackButton:    return 8;
    case Qt::ForwardButton: return 9;
    case Qt::ExtraButton4:  return 10;
    case Qt::ExtraButton5:  return 11;
    default:                return 0;
    }
}

// The WPE_MODIFIER_POINTER_BUTTONn bit an ordinal carries into the
// tracked mask — only ordinals 1..5 have flags.
unsigned wpeButtonModifier(unsigned ordinal)
{
    return ordinal >= 1 && ordinal <= 5
        ? unsigned(WPE_MODIFIER_POINTER_BUTTON1) << (ordinal - 1)
        : 0;
}

// Qt::Key -> XKB keysym (WPE_KEY_* == XK_*) for every key that is
// not a printable character; Qt::KeypadModifier selects the KP_*
// family.  Returns 0 when the printable fallback must handle it.
unsigned wpeKeysymForQtKey(int key, bool keypad)
{
    if (keypad) {
        switch (key) {
        case Qt::Key_0: case Qt::Key_1: case Qt::Key_2:
        case Qt::Key_3: case Qt::Key_4: case Qt::Key_5:
        case Qt::Key_6: case Qt::Key_7: case Qt::Key_8:
        case Qt::Key_9:
            return WPE_KEY_KP_0 + (key - Qt::Key_0);
        case Qt::Key_Enter:    return WPE_KEY_KP_Enter;
        case Qt::Key_Asterisk: return WPE_KEY_KP_Multiply;
        case Qt::Key_Plus:     return WPE_KEY_KP_Add;
        case Qt::Key_Minus:    return WPE_KEY_KP_Subtract;
        case Qt::Key_Period:
        case Qt::Key_Comma:    return WPE_KEY_KP_Decimal;
        case Qt::Key_Slash:    return WPE_KEY_KP_Divide;
        case Qt::Key_Equal:    return WPE_KEY_KP_Equal;
        case Qt::Key_Space:    return WPE_KEY_KP_Space;
        case Qt::Key_Tab:      return WPE_KEY_KP_Tab;
        case Qt::Key_Home:     return WPE_KEY_KP_Home;
        case Qt::Key_Left:     return WPE_KEY_KP_Left;
        case Qt::Key_Up:       return WPE_KEY_KP_Up;
        case Qt::Key_Right:    return WPE_KEY_KP_Right;
        case Qt::Key_Down:     return WPE_KEY_KP_Down;
        case Qt::Key_PageUp:   return WPE_KEY_KP_Page_Up;
        case Qt::Key_PageDown: return WPE_KEY_KP_Page_Down;
        case Qt::Key_End:      return WPE_KEY_KP_End;
        case Qt::Key_Insert:   return WPE_KEY_KP_Insert;
        case Qt::Key_Delete:   return WPE_KEY_KP_Delete;
        default:               break;
        }
    }
    switch (key) {
    case Qt::Key_Escape:    return WPE_KEY_Escape;
    case Qt::Key_Tab:       return WPE_KEY_Tab;
    case Qt::Key_Backtab:   return WPE_KEY_ISO_Left_Tab;
    case Qt::Key_Backspace: return WPE_KEY_BackSpace;
    case Qt::Key_Return:    return WPE_KEY_Return;
    case Qt::Key_Enter:     return WPE_KEY_KP_Enter;
    case Qt::Key_Insert:    return WPE_KEY_Insert;
    case Qt::Key_Delete:    return WPE_KEY_Delete;
    case Qt::Key_Pause:     return WPE_KEY_Pause;
    case Qt::Key_Print:     return WPE_KEY_Print;
    case Qt::Key_SysReq:    return WPE_KEY_Sys_Req;
    case Qt::Key_Clear:     return WPE_KEY_Clear;
    case Qt::Key_Home:      return WPE_KEY_Home;
    case Qt::Key_End:       return WPE_KEY_End;
    case Qt::Key_Left:      return WPE_KEY_Left;
    case Qt::Key_Up:        return WPE_KEY_Up;
    case Qt::Key_Right:     return WPE_KEY_Right;
    case Qt::Key_Down:      return WPE_KEY_Down;
    case Qt::Key_PageUp:    return WPE_KEY_Page_Up;
    case Qt::Key_PageDown:  return WPE_KEY_Page_Down;
    case Qt::Key_Shift:     return WPE_KEY_Shift_L;
    case Qt::Key_Control:   return WPE_KEY_Control_L;
    case Qt::Key_Meta:      return WPE_KEY_Meta_L;
    case Qt::Key_Alt:       return WPE_KEY_Alt_L;
    case Qt::Key_AltGr:     return WPE_KEY_ISO_Level3_Shift;
    case Qt::Key_CapsLock:  return WPE_KEY_Caps_Lock;
    case Qt::Key_NumLock:   return WPE_KEY_Num_Lock;
    case Qt::Key_ScrollLock:return WPE_KEY_Scroll_Lock;
    case Qt::Key_Super_L:   return WPE_KEY_Super_L;
    case Qt::Key_Super_R:   return WPE_KEY_Super_R;
    case Qt::Key_Hyper_L:   return WPE_KEY_Hyper_L;
    case Qt::Key_Hyper_R:   return WPE_KEY_Hyper_R;
    case Qt::Key_Menu:      return WPE_KEY_Menu;
    case Qt::Key_Help:      return WPE_KEY_Help;
    case Qt::Key_Multi_key: return WPE_KEY_Multi_key;
    // F1..F35 are contiguous on both sides.
    case Qt::Key_F1: case Qt::Key_F2: case Qt::Key_F3:
    case Qt::Key_F4: case Qt::Key_F5: case Qt::Key_F6:
    case Qt::Key_F7: case Qt::Key_F8: case Qt::Key_F9:
    case Qt::Key_F10: case Qt::Key_F11: case Qt::Key_F12:
    case Qt::Key_F13: case Qt::Key_F14: case Qt::Key_F15:
    case Qt::Key_F16: case Qt::Key_F17: case Qt::Key_F18:
    case Qt::Key_F19: case Qt::Key_F20: case Qt::Key_F21:
    case Qt::Key_F22: case Qt::Key_F23: case Qt::Key_F24:
    case Qt::Key_F25: case Qt::Key_F26: case Qt::Key_F27:
    case Qt::Key_F28: case Qt::Key_F29: case Qt::Key_F30:
    case Qt::Key_F31: case Qt::Key_F32: case Qt::Key_F33:
    case Qt::Key_F34: case Qt::Key_F35:
        return WPE_KEY_F1 + (key - Qt::Key_F1);
    // Compose/dead keys — the common European set.
    case Qt::Key_Dead_Grave:        return WPE_KEY_dead_grave;
    case Qt::Key_Dead_Acute:        return WPE_KEY_dead_acute;
    case Qt::Key_Dead_Circumflex:   return WPE_KEY_dead_circumflex;
    case Qt::Key_Dead_Tilde:        return WPE_KEY_dead_tilde;
    case Qt::Key_Dead_Macron:       return WPE_KEY_dead_macron;
    case Qt::Key_Dead_Breve:        return WPE_KEY_dead_breve;
    case Qt::Key_Dead_Abovedot:     return WPE_KEY_dead_abovedot;
    case Qt::Key_Dead_Diaeresis:    return WPE_KEY_dead_diaeresis;
    case Qt::Key_Dead_Abovering:    return WPE_KEY_dead_abovering;
    case Qt::Key_Dead_Doubleacute:  return WPE_KEY_dead_doubleacute;
    case Qt::Key_Dead_Caron:        return WPE_KEY_dead_caron;
    case Qt::Key_Dead_Cedilla:      return WPE_KEY_dead_cedilla;
    case Qt::Key_Dead_Ogonek:       return WPE_KEY_dead_ogonek;
    case Qt::Key_Dead_Iota:         return WPE_KEY_dead_iota;
    // XF86 browser/media keys (the vendor set rides in the same
    // WPE_KEY_* namespace).
    case Qt::Key_Back:           return WPE_KEY_Back;
    case Qt::Key_Forward:        return WPE_KEY_Forward;
    case Qt::Key_Stop:           return WPE_KEY_Stop;
    case Qt::Key_Refresh:        return WPE_KEY_Refresh;
    case Qt::Key_HomePage:       return WPE_KEY_HomePage;
    case Qt::Key_Favorites:      return WPE_KEY_Favorites;
    case Qt::Key_Search:         return WPE_KEY_Search;
    case Qt::Key_VolumeDown:     return WPE_KEY_AudioLowerVolume;
    case Qt::Key_VolumeUp:       return WPE_KEY_AudioRaiseVolume;
    case Qt::Key_VolumeMute:     return WPE_KEY_AudioMute;
    case Qt::Key_MediaPlay:      return WPE_KEY_AudioPlay;
    case Qt::Key_MediaStop:      return WPE_KEY_AudioStop;
    case Qt::Key_MediaPrevious:  return WPE_KEY_AudioPrev;
    case Qt::Key_MediaNext:      return WPE_KEY_AudioNext;
    default:                     return 0;
    }
}

// Unicode code point -> XKB keysym — the xkb_utf32_to_keysym rule:
// printable Latin-1 is identity, everything beyond carries the
// 0x01000000 Unicode marker; C0/C1 controls are not keysyms.
unsigned wpeKeysymForUcs4(char32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7e)
        return cp;
    if (cp >= 0xa0 && cp <= 0xff)
        return cp;
    if (cp > 0xff)
        return 0x01000000u | cp;
    return 0;
}

// Qt::Key -> evdev scancode for DOM KeyboardEvent.code — a static
// table because offscreen QPA reports nativeScanCode()==0 and the
// native value is platform-shaped (XKB keycode = evdev+8 on X11)
// everywhere else anyway.  Left-side codes where a pair exists —
// right-vs-left is a documented limit (Qt cannot tell them apart).
unsigned wpeKeycodeForQtKey(int key, bool keypad)
{
    if (keypad) {
        switch (key) {
        case Qt::Key_7:       return 71;   // KEY_KP7
        case Qt::Key_8:       return 72;
        case Qt::Key_9:       return 73;
        case Qt::Key_Minus:   return 74;   // KEY_KPMINUS
        case Qt::Key_4:       return 75;
        case Qt::Key_5:       return 76;
        case Qt::Key_6:       return 77;
        case Qt::Key_Plus:    return 78;   // KEY_KPPLUS
        case Qt::Key_1:       return 79;
        case Qt::Key_2:       return 80;
        case Qt::Key_3:       return 81;
        case Qt::Key_0:       return 82;
        case Qt::Key_Period:
        case Qt::Key_Comma:   return 83;   // KEY_KPDOT
        case Qt::Key_Asterisk:return 55;   // KEY_KPASTERISK
        case Qt::Key_Slash:   return 98;   // KEY_KPSLASH
        case Qt::Key_Enter:   return 96;   // KEY_KPENTER
        case Qt::Key_Equal:   return 117;  // KEY_KPEQUAL
        default:              break;
        }
    }
    switch (key) {
    case Qt::Key_Escape:    return 1;
    case Qt::Key_1:         return 2;
    case Qt::Key_2:         return 3;
    case Qt::Key_3:         return 4;
    case Qt::Key_4:         return 5;
    case Qt::Key_5:         return 6;
    case Qt::Key_6:         return 7;
    case Qt::Key_7:         return 8;
    case Qt::Key_8:         return 9;
    case Qt::Key_9:         return 10;
    case Qt::Key_0:         return 11;
    case Qt::Key_Minus:     return 12;
    case Qt::Key_Equal:     return 13;
    case Qt::Key_Backspace: return 14;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:   return 15;
    case Qt::Key_Q:         return 16;
    case Qt::Key_W:         return 17;
    case Qt::Key_E:         return 18;
    case Qt::Key_R:         return 19;
    case Qt::Key_T:         return 20;
    case Qt::Key_Y:         return 21;
    case Qt::Key_U:         return 22;
    case Qt::Key_I:         return 23;
    case Qt::Key_O:         return 24;
    case Qt::Key_P:         return 25;
    case Qt::Key_BracketLeft:  return 26;
    case Qt::Key_BracketRight: return 27;
    case Qt::Key_Return:    return 28;    // KEY_ENTER
    case Qt::Key_Control:   return 29;    // KEY_LEFTCTRL
    case Qt::Key_A:         return 30;
    case Qt::Key_S:         return 31;
    case Qt::Key_D:         return 32;
    case Qt::Key_F:         return 33;
    case Qt::Key_G:         return 34;
    case Qt::Key_H:         return 35;
    case Qt::Key_J:         return 36;
    case Qt::Key_K:         return 37;
    case Qt::Key_L:         return 38;
    case Qt::Key_Semicolon: return 39;
    case Qt::Key_Apostrophe:return 40;
    case Qt::Key_QuoteLeft: return 41;    // KEY_GRAVE
    case Qt::Key_Shift:     return 42;    // KEY_LEFTSHIFT
    case Qt::Key_Backslash: return 43;
    case Qt::Key_Z:         return 44;
    case Qt::Key_X:         return 45;
    case Qt::Key_C:         return 46;
    case Qt::Key_V:         return 47;
    case Qt::Key_B:         return 48;
    case Qt::Key_N:         return 49;
    case Qt::Key_M:         return 50;
    case Qt::Key_Comma:     return 51;
    case Qt::Key_Period:    return 52;
    case Qt::Key_Slash:     return 53;
    case Qt::Key_Alt:       return 56;    // KEY_LEFTALT
    case Qt::Key_Space:     return 57;
    case Qt::Key_CapsLock:  return 58;
    case Qt::Key_F1:        return 59;
    case Qt::Key_F2:        return 60;
    case Qt::Key_F3:        return 61;
    case Qt::Key_F4:        return 62;
    case Qt::Key_F5:        return 63;
    case Qt::Key_F6:        return 64;
    case Qt::Key_F7:        return 65;
    case Qt::Key_F8:        return 66;
    case Qt::Key_F9:        return 67;
    case Qt::Key_F10:       return 68;
    case Qt::Key_NumLock:   return 69;
    case Qt::Key_ScrollLock:return 70;
    case Qt::Key_F11:       return 87;
    case Qt::Key_F12:       return 88;
    case Qt::Key_F13:       return 183;
    case Qt::Key_F14:       return 184;
    case Qt::Key_F15:       return 185;
    case Qt::Key_F16:       return 186;
    case Qt::Key_F17:       return 187;
    case Qt::Key_F18:       return 188;
    case Qt::Key_F19:       return 189;
    case Qt::Key_F20:       return 190;
    case Qt::Key_F21:       return 191;
    case Qt::Key_F22:       return 192;
    case Qt::Key_F23:       return 193;
    case Qt::Key_F24:       return 194;
    case Qt::Key_Super_L:
    case Qt::Key_Meta:      return 125;   // KEY_LEFTMETA
    case Qt::Key_Super_R:   return 126;   // KEY_RIGHTMETA
    case Qt::Key_Menu:      return 139;
    case Qt::Key_Enter:     return 96;    // KP enter sans the flag
    case Qt::Key_Home:      return 102;
    case Qt::Key_Up:        return 103;
    case Qt::Key_PageUp:    return 104;
    case Qt::Key_Left:      return 105;
    case Qt::Key_Right:     return 106;
    case Qt::Key_End:       return 107;
    case Qt::Key_Down:      return 108;
    case Qt::Key_PageDown:  return 109;
    case Qt::Key_Insert:    return 110;
    case Qt::Key_Delete:    return 111;
    case Qt::Key_VolumeMute:return 113;
    case Qt::Key_VolumeDown:return 114;
    case Qt::Key_VolumeUp:  return 115;
    case Qt::Key_HomePage:  return 172;
    case Qt::Key_Back:      return 158;
    case Qt::Key_Forward:   return 159;
    case Qt::Key_Stop:      return 166;   // KEY_STOPCD
    case Qt::Key_Refresh:   return 173;
    case Qt::Key_Favorites: return 156;   // KEY_BOOKMARKS
    case Qt::Key_Search:    return 217;
    case Qt::Key_MediaPlay: return 164;   // KEY_PLAYPAUSE
    case Qt::Key_MediaStop: return 166;   // KEY_STOPCD
    case Qt::Key_MediaPrevious: return 165; // KEY_PREVIOUSSONG
    case Qt::Key_MediaNext: return 163;   // KEY_NEXTSONG
    default:                return 0;
    }
}

// ---- WK14 — find options + the optional suspend/resume exports -------

// FindFlags -> WebKitFindOptions.  WebKit's case handling inverts
// Qt's: NONE is case-SENSITIVE there, while the Qt flag set defaults
// insensitive and opts into FindCaseSensitively.  Qt find wraps
// unconditionally (WebViewSearch relies on it — WebEngine dropped its
// wrap flag), so WRAP_AROUND is always on.
guint32 wkFindOptions(Engine::FindFlags options)
{
    guint32 out = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
    if (!(options & Engine::FindCaseSensitively))
        out |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
    if (options & Engine::FindBackward)
        out |= WEBKIT_FIND_OPTIONS_BACKWARDS;
    return out;
}

// webkit_web_view_suspend/resume are not in the WPE 2.54 headers —
// the build's libWPEWebKit exports neither (nm-verified).  Resolve
// them through dlsym anyway, the same way the servo shim gates its
// optional se_* exports: a newer artifact gets the real process
// suspend with no rebuild, and this build reports the gate honestly.
// RTLD_DEFAULT reaches the DT_NEEDED-linked library's own symbols.
typedef void (*WebkitViewVoidFn)(WebKitWebView *);

WebkitViewVoidFn webkitViewExport(const char *name)
{
    return reinterpret_cast<WebkitViewVoidFn>(dlsym(RTLD_DEFAULT, name));
}

bool wkSuspendResumeAvailable()
{
    static const bool available =
        webkitViewExport("webkit_web_view_suspend")
        && webkitViewExport("webkit_web_view_resume");
    return available;
}

// ---- WK16 — user scripts, named worlds + the host channel ---------

// The window.webkit.messageHandlers member every page's channel
// world gets — JS posts via
// window.webkit.messageHandlers.arktosChannel.postMessage(v).  The
// two detailed-signal spellings below carry the same name inline
// (g_signal_connect wants string literals).
constexpr char kChannelHandlerName[] = "arktosChannel";

// Engine::Script -> a retained WebKitUserScript the caller holds by
// name for a later remove_script.  World 0 is the main world (NULL
// world_name); the interface's three injection points fold onto
// WebKit's two — DocumentCreation is document-start, DocumentReady
// and Deferred share the document-end boundary.
WebKitUserScript *userScriptFor(const Engine::Script &script)
{
    const QByteArray source = script.sourceCode.toUtf8();
    const QByteArray world =
        WebkitPage::scriptWorldName(script.worldId);
    const WebKitUserContentInjectedFrames frames =
        script.runsOnSubFrames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES
                               : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
    const WebKitUserScriptInjectionTime time =
        script.injectionPoint == Engine::InjectionPoint::DocumentCreation
            ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
            : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;
    // new_for_world asserts worldName non-NULL — the main world has
    // its own constructor spelling.
    if (world.isEmpty()) {
        return webkit_user_script_new(
            source.constData(), frames, time, nullptr, nullptr);
    }
    return webkit_user_script_new_for_world(
        source.constData(), frames, time, world.constData(),
        nullptr, nullptr);
}

// QVariant -> JSCValue, the inverse of jscValueToVariant's matrix:
// scalars map directly, the invalid variant answers undefined,
// valid-null answers null, and every structured value crosses as
// parsed JSON so objects come back as real JS objects rather than
// JSON text.
JSCValue *variantToJsc(JSCContext *context, const QVariant &variant)
{
    if (!context)
        return nullptr;
    if (!variant.isValid())
        return jsc_value_new_undefined(context);
    if (variant.isNull())
        return jsc_value_new_null(context);
    const int type = variant.typeId();
    if (type == QMetaType::Bool)
        return jsc_value_new_boolean(context, variant.toBool());
    if (type == QMetaType::QString) {
        const QByteArray text = variant.toString().toUtf8();
        return jsc_value_new_string(context, text.constData());
    }
    if (type == QMetaType::Int || type == QMetaType::UInt
            || type == QMetaType::LongLong
            || type == QMetaType::ULongLong
            || type == QMetaType::Double || type == QMetaType::Float)
        return jsc_value_new_number(context, variant.toDouble());
    const QJsonDocument document = QJsonDocument::fromVariant(variant);
    if (!document.isNull()) {
        const QByteArray json =
            document.toJson(QJsonDocument::Compact);
        return jsc_value_new_from_json(context, json.constData());
    }
    return jsc_value_new_undefined(context);
}

// webkit_user_content_manager_register_script_message_handler_
// with_reply is the 2.5x-era API — exported on this build's 2.54;
// dlsym-gated like the suspend pair so an older artifact degrades to
// the one-way registration instead of failing symbol resolution at
// load time.
typedef gboolean (*RegisterScriptMessageWithReplyFn)(
    WebKitUserContentManager *, const char *, const char *);

RegisterScriptMessageWithReplyFn wkRegisterWithReply()
{
    return reinterpret_cast<RegisterScriptMessageWithReplyFn>(
        dlsym(RTLD_DEFAULT,
              "webkit_user_content_manager_register_script_message_"
              "handler_with_reply"));
}

bool wkScriptMessageReplyAvailable()
{
    static const bool available = wkRegisterWithReply() != nullptr;
    return available;
}

} // namespace
#endif // ARKTOS_WEBKIT_LINKED

// ---- WK20: dmabuf -> EGLImage -> GL texture import -------------------
//
// WK08's raster path (wpe_buffer_import_to_pixels -> QImage -> blit)
// pays a full CPU copy per frame.  The spec'd upgrade imports the
// WPEBufferDMABuf as an EGLImage and samples it as a GL texture.  The
// EGLImage lives on the WPE display's own EGLDisplay
// (wpe_display_get_egl_display — the headless display's is a Mesa
// surfaceless display, which initializes even under offscreen test
// runs), so it cannot be bound by a QOpenGLWidget/QRhiWidget context:
// those live on the Qt QPA display, a different EGLDisplay, and the
// offscreen platform provides no GL at all.  The import therefore runs
// in a private surfaceless GLES2 context created on that same display;
// the texture is drawn into an FBO at buffer size and read back into
// the QImage the widget presents (the compositor leg stays the proven
// raster blit).  Cross-display onscreen compositing — re-importing the
// raw dmabuf fds into Qt's EGLDisplay — is the remaining step past
// WK20, which exists to prove the dmabuf -> EGLImage -> texture ->
// draw -> fence machinery, not to re-plumb the widget surface.
//
// Fences: WPE docs scope the fence fds to explicit-sync displays (the
// headless display reports use_explicit_sync false, so they are
// normally -1); the code still honors them — the buffer's rendering
// fence is waited on server-side before sampling, and a release fence
// is exported back onto the buffer after our GL use completes.
//
// wpe_buffer_import_to_egl_image is (transfer none): the buffer owns
// the EGLImage — never eglDestroyImage it.
#if defined(ARKTOS_WEBKIT_LINKED) && defined(ARKTOS_WEBKIT_GL)
namespace {

struct WkGlPipe {
    enum State { Untried, Ready, Broken };
    State state = Untried;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint prog = 0;
    GLuint fbo = 0;
    GLuint fboTex = 0;
    int fboW = 0;
    int fboH = 0;
    GLint aPos = -1;
    GLint aTex = -1;
    GLint uSwap = -1;
    PFNEGLCREATESYNCKHRPROC eglCreateSync = nullptr;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySync = nullptr;
    PFNEGLWAITSYNCKHRPROC eglWaitSync = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupFenceFd = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glImageTargetTex = nullptr;
    // Channel-order calibration: the dmabuf fourcc declares a memory
    // layout, but what the WebProcess actually wrote can disagree
    // (measured: the same XR24 X-tiled buffer sampled B,G,R,A in one
    // process and R,G,B,A in another).  So the first GL-imported frame
    // is compared against the raster ground truth instead of trusting
    // a fourcc table — the pixels vote identity vs R/B-swap.
    bool calibrated = false;
    bool swapRB = false;
    int calibMisses = 0;
    // Frame-time accounting — cumulative per path so the rough
    // raster-vs-GL log line can show both sides of the difference
    // (ARKTOS_WEBKIT_FRAME_TIMING=1 prints per frame).
    qint64 glFrames = 0;
    qint64 glFails = 0;
    qint64 rasterFrames = 0;
    qint64 glImportNs = 0;
    qint64 glReadbackNs = 0;
    qint64 glTotalNs = 0;
    qint64 rasterNs = 0;
};

WkGlPipe &wkGlPipe()
{
    static WkGlPipe pipe;
    return pipe;
}

void wkGlCleanup();

GLuint wkGlShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE)
        return shader;
    char log[512];
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    qWarning("webkit: gl shader compile failed: %s", log);
    glDeleteShader(shader);
    return 0;
}

bool wkGlInit()
{
    WkGlPipe &p = wkGlPipe();
    if (p.state != WkGlPipe::Untried)
        return p.state == WkGlPipe::Ready;
    p.state = WkGlPipe::Broken;
    GError *error = nullptr;
    p.dpy = static_cast<EGLDisplay>(wpe_display_get_egl_display(
        WebkitBackend::sharedDisplay(), &error));
    if (p.dpy == EGL_NO_DISPLAY) {
        qWarning("webkit: no EGL display for the GL frame path (%s) — "
                 "raster only", error ? error->message : "unavailable");
        g_clear_error(&error);
        return false;
    }
    // The display arrives already eglInitialize'd by wpe-platform —
    // re-initializing an initialized display is a cheap no-op.
    eglInitialize(p.dpy, nullptr, nullptr);
    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    p.ctx = eglCreateContext(p.dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
                             ctxAttribs);
    // Surfaceless first (EGL_KHR_surfaceless_context); a 1x1 pbuffer
    // covers drivers that still want a drawable for offscreen work.
    if (p.ctx == EGL_NO_CONTEXT
            || !eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                               p.ctx)) {
        EGLConfig config = nullptr;
        EGLint n = 0;
        const EGLint cfgAttribs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE };
        if (p.ctx != EGL_NO_CONTEXT
                && eglChooseConfig(p.dpy, cfgAttribs, &config, 1, &n)
                && n >= 1) {
            const EGLint pbAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1,
                                         EGL_NONE };
            p.surface = eglCreatePbufferSurface(p.dpy, config,
                                                pbAttribs);
            if (p.surface != EGL_NO_SURFACE)
                eglMakeCurrent(p.dpy, p.surface, p.surface, p.ctx);
        }
        if (p.ctx == EGL_NO_CONTEXT || !eglGetCurrentContext()) {
            qWarning("webkit: GLES2 context for the GL frame path "
                     "failed (0x%x) — raster only", eglGetError());
            if (p.surface != EGL_NO_SURFACE)
                eglDestroySurface(p.dpy, p.surface);
            if (p.ctx != EGL_NO_CONTEXT)
                eglDestroyContext(p.dpy, p.ctx);
            p.ctx = EGL_NO_CONTEXT;
            p.surface = EGL_NO_SURFACE;
            return false;
        }
    }
    p.eglCreateSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(
        eglGetProcAddress("eglCreateSyncKHR"));
    p.eglDestroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(
        eglGetProcAddress("eglDestroySyncKHR"));
    p.eglWaitSync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(
        eglGetProcAddress("eglWaitSyncKHR"));
    p.eglDupFenceFd =
        reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
            eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    p.glImageTargetTex =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!p.glImageTargetTex) {
        qWarning("webkit: glEGLImageTargetTexture2DOES missing — "
                 "raster only");
        return false;
    }
    static const char vsSrc[] =
        "attribute vec2 aPos;\n"
        "attribute vec2 aTex;\n"
        "varying vec2 vTex;\n"
        "void main() { vTex = aTex; gl_Position = vec4(aPos, 0, 1); }\n";
    // uSwap picks the swizzle; the value is decided empirically by
    // wkGlCalibrate, never by the fourcc (see WkGlPipe::calibrated).
    static const char fsSrc[] =
        "precision mediump float;\n"
        "varying vec2 vTex;\n"
        "uniform sampler2D uTex;\n"
        "uniform float uSwap;\n"
        "void main() { vec4 t = texture2D(uTex, vTex);"
        " gl_FragColor = uSwap > 0.5 ? t.bgra : t.rgba; }\n";
    GLuint vs = wkGlShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = wkGlShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs)
        return false;
    p.prog = glCreateProgram();
    glAttachShader(p.prog, vs);
    glAttachShader(p.prog, fs);
    glLinkProgram(p.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        qWarning("webkit: gl program link failed — raster only");
        return false;
    }
    p.aPos = glGetAttribLocation(p.prog, "aPos");
    p.aTex = glGetAttribLocation(p.prog, "aTex");
    p.uSwap = glGetUniformLocation(p.prog, "uSwap");
    // glUniform needs the program bound — and any error left flagged
    // here would poison the post-bind glGetError gate on the first
    // frame, so keep init's own error state clean.
    glUseProgram(p.prog);
    glUniform1i(glGetUniformLocation(p.prog, "uTex"), 0);
    glGetError();
    glGenFramebuffers(1, &p.fbo);
    eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    static bool cleanupRegistered = false;
    if (!cleanupRegistered) {
        cleanupRegistered = true;
        qAddPostRoutine(wkGlCleanup);
    }
    p.state = WkGlPipe::Ready;
    qInfo("webkit: GL frame path ready (%s) attribs pos=%d tex=%d "
          "uSwap=%d", glGetString(GL_RENDERER), int(p.aPos), int(p.aTex),
          int(p.uSwap));
    return true;
}

void wkGlEnsureFbo(WkGlPipe *p, int w, int h)
{
    if (p->fboW == w && p->fboH == h)
        return;
    if (p->fboTex)
        glDeleteTextures(1, &p->fboTex);
    glGenTextures(1, &p->fboTex);
    glBindTexture(GL_TEXTURE_2D, p->fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, p->fboTex, 0);
    p->fboW = w;
    p->fboH = h;
}

void wkGlCleanup()
{
    WkGlPipe &p = wkGlPipe();
    if (p.dpy == EGL_NO_DISPLAY)
        return;
    if (p.ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(p.dpy, p.surface, p.surface, p.ctx);
        if (p.fboTex)
            glDeleteTextures(1, &p.fboTex);
        if (p.fbo)
            glDeleteFramebuffers(1, &p.fbo);
        if (p.prog)
            glDeleteProgram(p.prog);
        eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(p.dpy, p.ctx);
        p.ctx = EGL_NO_CONTEXT;
    }
    if (p.surface != EGL_NO_SURFACE) {
        eglDestroySurface(p.dpy, p.surface);
        p.surface = EGL_NO_SURFACE;
    }
    p.dpy = EGL_NO_DISPLAY;   // owned by the WPEDisplay — not ours
}

// One buffer-rendered frame through the GL path.  Returns true and
// fills *frame with the readback image on success; false sends the
// caller down the raster fallback.  All GL work is synchronous inside
// the signal handler — the buffer (and its borrowed EGLImage) dies
// with the emission.
bool wkGlImport(WPEBuffer *buffer, QImage *frame)
{
    WkGlPipe &p = wkGlPipe();
    if (!wkGlInit())
        return false;
    QElapsedTimer total;
    total.start();
    if (!eglMakeCurrent(p.dpy, p.surface, p.surface, p.ctx)) {
        p.state = WkGlPipe::Broken;
        ++p.glFails;
        return false;
    }

    GError *error = nullptr;
    QElapsedTimer importTimer;
    importTimer.start();
    EGLImage image = static_cast<EGLImage>(
        wpe_buffer_import_to_egl_image(buffer, &error));
    if (image == EGL_NO_IMAGE) {
        ++p.glFails;
        if (p.glFails <= 4)
            qWarning("webkit: wpe_buffer_import_to_egl_image failed "
                     "(%s): %s", G_OBJECT_TYPE_NAME(buffer),
                     error ? error->message : "?");
        g_clear_error(&error);
        eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return false;
    }

    // Rendering fence: the producer's "writes done" signal, present
    // only on explicit-sync displays.  take() transfers fd ownership
    // to us; the native-fence sync then adopts it (get() would leave
    // the buffer's own UnixFileDescriptor aliasing an fd the sync
    // closes at destroy — a double-close hazard).
    const int fenceFd = wpe_buffer_take_rendering_fence(buffer);
    if (fenceFd >= 0) {
        EGLSync sync = EGL_NO_SYNC_KHR;
        if (p.eglCreateSync && p.eglWaitSync) {
            const EGLint attribs[] = {
                EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fenceFd, EGL_NONE };
            sync = p.eglCreateSync(p.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID,
                                   attribs);
        }
        if (sync != EGL_NO_SYNC_KHR) {
            p.eglWaitSync(p.dpy, sync, 0);
            p.eglDestroySync(p.dpy, sync);
        } else {
            close(fenceFd);   // no sync object adopted it — still ours
        }
    }

    const int w = wpe_buffer_get_width(buffer);
    const int h = wpe_buffer_get_height(buffer);
    if (w <= 0 || h <= 0) {
        ++p.glFails;
        eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return false;
    }
    wkGlEnsureFbo(&p, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, p.fbo);

    if (qEnvironmentVariableIsSet("ARKTOS_WEBKIT_FRAME_TIMING")
            && WPE_IS_BUFFER_DMA_BUF(buffer)) {
        WPEBufferDMABuf *db = WPE_BUFFER_DMA_BUF(buffer);
        qInfo("webkit: gl buffer %s fourcc=0x%08x modifier=0x%llx "
              "swapRB=%d", G_OBJECT_TYPE_NAME(buffer),
              wpe_buffer_dma_buf_get_format(db),
              (unsigned long long)wpe_buffer_dma_buf_get_modifier(db),
              int(p.swapRB));
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    p.glImageTargetTex(GL_TEXTURE_2D, image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) {
        ++p.glFails;
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &tex);
        eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return false;
    }
    const qint64 importNs = importTimer.nsecsElapsed();

    glViewport(0, 0, w, h);
    glUseProgram(p.prog);
    // uSwap stays 0 until calibration settles the channel order —
    // uncalibrated frames exist to feed the raster-truth comparison.
    glUniform1f(p.uSwap, p.swapRB ? 1.f : 0.f);
    // v = 0 at FBO y = 0: the buffer is top-down, so readback row 0
    // lands page-top without a mirror pass.
    static const float quad[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f };
    glVertexAttribPointer(p.aPos, 2, GL_FLOAT, GL_FALSE, 16, quad);
    glVertexAttribPointer(p.aTex, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
    glEnableVertexAttribArray(p.aPos);
    glEnableVertexAttribArray(p.aTex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Release fence: report when our GL use is done so an
    // explicit-sync display can safely recycle the buffer.
    if (p.eglCreateSync && p.eglDupFenceFd && p.eglDestroySync) {
        EGLSync done = p.eglCreateSync(p.dpy, EGL_SYNC_FENCE_KHR,
                                       nullptr);
        if (done != EGL_NO_SYNC_KHR) {
            glFlush();
            const int releaseFd = p.eglDupFenceFd(p.dpy, done);
            p.eglDestroySync(p.dpy, done);
            if (releaseFd >= 0)
                wpe_buffer_set_release_fence(buffer, releaseFd);
        }
    }

    QElapsedTimer readTimer;
    readTimer.start();
    QImage img(w, h, QImage::Format_RGBA8888);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    const qint64 readNs = readTimer.nsecsElapsed();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &tex);
    eglMakeCurrent(p.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    ++p.glFrames;
    const qint64 totalNs = total.nsecsElapsed();
    p.glImportNs += importNs;
    p.glReadbackNs += readNs;
    p.glTotalNs += totalNs;
    if (qEnvironmentVariableIsSet("ARKTOS_WEBKIT_FRAME_TIMING"))
        qInfo("webkit: gl frame %lld total=%.2fms import=%.2f "
              "draw+fence=%.2f readback=%.2f | raster avg=%.2fms n=%lld",
              (long long)p.glFrames, totalNs / 1e6, importNs / 1e6,
              (totalNs - importNs - readNs) / 1e6, readNs / 1e6,
              p.rasterFrames
                  ? p.rasterNs / 1e6 / p.rasterFrames : 0.0,
              (long long)p.rasterFrames);
    *frame = img;
    return true;
}

// Compare an identity-swizzled GL readback against the raster copy of
// the same buffer: every sampled pixel votes identity or R/B-swapped,
// and a >=4x consistent majority settles WkGlPipe::swapRB for the
// rest of the process.  Opaque pixels only — the raster copy is
// premultiplied, so translucent pixels would vote garbage.
// Frames that cannot decide (gray page, mismatched content) stay
// uncalibrated and retry; persistent total disagreement marks the
// pipe broken so raster takes over for good.
void wkGlCalibrate(const QImage &gl, const QImage &raster)
{
    WkGlPipe &p = wkGlPipe();
    const int w = qMin(gl.width(), raster.width());
    const int h = qMin(gl.height(), raster.height());
    int identity = 0, swapped = 0;
    if (w > 0 && h > 0) {
        const int stride = qMax(1, w * h / 512);
        int n = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x, ++n) {
                if (n % stride)
                    continue;
                const QRgb g = gl.pixel(x, y) | 0xff000000u;
                const QRgb r = raster.pixel(x, y) | 0xff000000u;
                if ((r & 0xff000000u) != 0xff000000u)
                    continue;
                if (g == r)
                    ++identity;
                else if (g == ((r & 0xff00ff00u)
                               | ((r & 0xffu) << 16)
                               | ((r >> 16) & 0xffu)))
                    ++swapped;
            }
        }
    }
    const int votes = identity + swapped;
    if (votes == 0) {
        if (++p.calibMisses > 8) {
            p.state = WkGlPipe::Broken;
            qWarning("webkit: GL readback never matches raster "
                     "ground truth — raster path stays");
        }
        return;
    }
    if (identity >= qMax(4, swapped * 4)
            || swapped >= qMax(4, identity * 4)) {
        p.swapRB = swapped > identity;
        p.calibrated = true;
        qInfo("webkit: GL frame channel order calibrated: %s "
              "(identity=%d swapped=%d)",
              p.swapRB ? "BGRA->RGBA" : "RGBA", identity, swapped);
        return;
    }
    if (++p.calibMisses > 8) {
        // Never converges — identity is Mesa's declared semantic.
        p.swapRB = false;
        p.calibrated = true;
        qWarning("webkit: GL channel-order calibration inconclusive; "
                 "assuming RGBA (identity=%d swapped=%d)",
                 identity, swapped);
    }
}

} // namespace
#endif // ARKTOS_WEBKIT_LINKED && ARKTOS_WEBKIT_GL

// ---- WebkitView -----------------------------------------------------

WebkitView::WebkitView(QWidget *parent)
    : QWidget(parent)
{
    // The page's buffers carry real content — the widget is a paint
    // surface only.
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    // Focusable like a real web view (QWebEngineView uses WheelFocus)
    // so focusInEvent can push wpe_view_focus_in for WK13's input path.
    setFocusPolicy(Qt::WheelFocus);
}

void WebkitView::adoptFrameImage(const QImage &img)
{
    m_frame = img;
    m_frameSize = img.size();
    ++m_frameCount;
    update();
}

void WebkitView::adoptGlFrame(const QImage &img)
{
    adoptFrameImage(img);
    ++m_glFrames;
}

void WebkitView::noteGlImportFailure()
{
    ++m_glImportFails;
}

void WebkitView::paintEvent(QPaintEvent *)
{
    // Blank-page color under whatever the newest buffer covers.
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    if (!m_frame.isNull())
        painter.drawImage(0, 0, m_frame);
}

void WebkitView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // WK03 resize contract: re-size the toplevel; the headless
    // toplevel propagates to its view and the NEXT produced buffer
    // carries the new dims.
    if (m_page)
        m_page->resizeEngine(event->size().width(),
                             event->size().height());
}

void WebkitView::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    if (m_page)
        m_page->focusEngine(true);
}

void WebkitView::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    if (m_page)
        m_page->focusEngine(false);
}

// ---- WK13 — QWidget events -> wpe-platform events -------------------

QPointF WebkitView::viewPoint(const QPointF &widgetPos) const
{
    return m_page ? m_page->viewPoint(widgetPos) : widgetPos;
}

int WebkitView::nextPressCount(Qt::MouseButton button,
                               const QPointF &pos)
{
    // The second down of a Qt double-click arrives as
    // MouseButtonDblClick through the same entry point, so one
    // generation counter covers plain presses and dblclicks.
    if (button == m_pressButton && m_pressTimer.isValid()
            && m_pressTimer.elapsed()
                   <= QApplication::doubleClickInterval()
            && (pos - m_pressPos).manhattanLength() <= 8)
        ++m_pressCount;
    else
        m_pressCount = 1;
    m_pressButton = button;
    m_pressPos = pos;
    m_pressTimer.start();
    return m_pressCount;
}

void WebkitView::dispatchPointerMove(int type,
                                     const QPointF &widgetPos,
                                     unsigned mods)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_page)
        return;
    const QPointF p = viewPoint(widgetPos);
    // Enter/leave carry no movement delta; move reports the hop
    // from the last delivered position (DOM movementX/Y).
    double dx = 0.0, dy = 0.0;
    if (type == int(WPE_EVENT_POINTER_MOVE)
            && !m_lastPointerPos.isNull()) {
        const QPointF from = viewPoint(m_lastPointerPos);
        dx = p.x() - from.x();
        dy = p.y() - from.y();
    }
    m_lastPointerPos = widgetPos;
    m_page->sendPointerEvent(type, p.x(), p.y(), dx, dy, mods);
#else
    Q_UNUSED(type);
    Q_UNUSED(widgetPos);
    Q_UNUSED(mods);
#endif
}

void WebkitView::dispatchPointerButton(int type, QMouseEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_page)
        return;
    const unsigned ordinal = wpeButtonOrdinal(event->button());
    if (!ordinal)
        return;
    const QPointF p = viewPoint(event->position());
    m_lastPointerPos = event->position();
    if (type == int(WPE_EVENT_POINTER_DOWN)) {
        // The freshly-pressed button travels in its own event's
        // modifier mask (the GDK convention the WK04 harness
        // measured); press_count is DOWN-only.
        m_pointerButtonMask |= wpeButtonModifier(ordinal);
        m_page->sendButtonEvent(
            type, p.x(), p.y(), ordinal,
            unsigned(nextPressCount(event->button(),
                                    event->position())),
            wpeKeyboardMods(event->modifiers())
                | m_pointerButtonMask);
    } else {
        // The releasing button still travels in its own event's
        // mask — clear it only after the dispatch.
        m_page->sendButtonEvent(
            type, p.x(), p.y(), ordinal, 0,
            wpeKeyboardMods(event->modifiers())
                | m_pointerButtonMask);
        m_pointerButtonMask &= ~wpeButtonModifier(ordinal);
    }
#else
    Q_UNUSED(type);
    Q_UNUSED(event);
#endif
}

bool WebkitView::dispatchKeyEvent(int type, QKeyEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_page)
        return false;
    const bool keypad = event->modifiers() & Qt::KeypadModifier;
    unsigned keyval = keypad
        ? wpeKeysymForQtKey(event->key(), true)
        : 0;   // keypad keysyms first — KP_7 text is plain '7'
    if (!keyval) {
        // Printable text carries the shift level — shift+a is XK_A,
        // shift+1 XK_exclam — and non-ASCII text lands through the
        // Unicode keysym rule.  Control text (ctrl+a = 0x01) has no
        // keysym and falls through to the key table.
        const QString text = event->text();
        if (text.size() == 1 && text.at(0).unicode() >= 0x20
                && text.at(0).unicode() != 0x7f)
            keyval = wpeKeysymForUcs4(text.at(0).unicode());
    }
    if (!keyval)
        keyval = wpeKeysymForQtKey(event->key(), keypad);
    if (!keyval && event->key() >= 'A' && event->key() <= 'Z')
        keyval = unsigned(event->key()) + 0x20;   // ctrl+letter: XK_a
    else if (!keyval && event->key() >= 0x21 && event->key() <= 0x7e)
        keyval = unsigned(event->key());   // ASCII punct/digit shape
    if (!keyval)
        return false;   // unmappable — leave it to Qt
    // XKB level semantics: Shift held reports the uppercase letter
    // keysym (WebKit derives DOM key from keyval; QTest and some
    // platforms do not shift the event text).
    if ((event->modifiers() & Qt::ShiftModifier) != 0
        && keyval >= 0x61 && keyval <= 0x7a)
        keyval -= 0x20;
    unsigned keycode = wpeKeycodeForQtKey(event->key(), keypad);
    if (keycode)
        keycode += 8;   // WPE consumes the X/XKB keycode space
    m_page->sendKeyEvent(type, keycode, keyval,
                         wpeKeyboardMods(event->modifiers())
                             | m_pointerButtonMask);
    return true;
#else
    Q_UNUSED(type);
    Q_UNUSED(event);
    return false;
#endif
}

void WebkitView::enterEvent(QEnterEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    dispatchPointerMove(int(WPE_EVENT_POINTER_ENTER),
                        event->position(),
                        wpeKeyboardMods(event->modifiers())
                            | m_pointerButtonMask);
#endif
    QWidget::enterEvent(event);
}

void WebkitView::leaveEvent(QEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    // Pointer leave at the last known position — the engine only
    // needs the transition; the exit point is stale by definition.
    dispatchPointerMove(int(WPE_EVENT_POINTER_LEAVE), m_lastPointerPos,
                        wpeKeyboardMods(
                            QGuiApplication::keyboardModifiers())
                            | m_pointerButtonMask);
#endif
    QWidget::leaveEvent(event);
}

void WebkitView::mouseMoveEvent(QMouseEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    dispatchPointerMove(int(WPE_EVENT_POINTER_MOVE),
                        event->position(),
                        wpeKeyboardMods(event->modifiers())
                            | m_pointerButtonMask);
#endif
    event->accept();
}

void WebkitView::mousePressEvent(QMouseEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    // WK19: the "context-menu" signal carries no coordinates on WPE —
    // the last right-button press position anchors the popup.
    if (event->button() == Qt::RightButton)
        m_contextMenuPos = event->position();
    dispatchPointerButton(int(WPE_EVENT_POINTER_DOWN), event);
#endif
    event->accept();
}

void WebkitView::mouseReleaseEvent(QMouseEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    dispatchPointerButton(int(WPE_EVENT_POINTER_UP), event);
#endif
    event->accept();
}

void WebkitView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Qt routes the second press here instead of mousePressEvent —
    // dispatch it as another DOWN so press_count reaches 2.
#ifdef ARKTOS_WEBKIT_LINKED
    dispatchPointerButton(int(WPE_EVENT_POINTER_DOWN), event);
#endif
    event->accept();
}

void WebkitView::wheelEvent(QWheelEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_page) {
        const QPointF p = viewPoint(event->position());
        const unsigned mods =
            wpeKeyboardMods(event->modifiers()) | m_pointerButtonMask;
        const QPoint pixel = event->pixelDelta();
        const QPoint angle = event->angleDelta();
        if (!pixel.isNull()) {
            // Precise path: real device pixels -> CSS px, with the
            // 2.5 multiplier WebKit applies to precise mouse/touchpad
            // deltas (WebEventFactoryWPE.cpp — the GTK scroll-unit
            // factor) divided back out so touchpad pixels stay
            // pixel-faithful.  Same sign convention as Qt: positive
            // delta_y scrolls UP (WPEWaylandSeat.cpp negates the
            // scroll-down-positive wl_pointer.axis value).
            const qreal dpr = devicePixelRatioF();
            m_page->sendScrollEvent(
                p.x(), p.y(),
                double(pixel.x()) / dpr / 2.5,
                double(pixel.y()) / dpr / 2.5,
                true, int(WPE_INPUT_SOURCE_TOUCHPAD), mods);
        } else if (!angle.isNull()) {
            // Tick path: non-precise deltas are wheel ticks,
            // 120 angle units per notch — same positive=up sign
            // convention as Qt, so no negation.
            m_page->sendScrollEvent(
                p.x(), p.y(),
                double(angle.x()) / 120.0,
                double(angle.y()) / 120.0,
                false, int(WPE_INPUT_SOURCE_MOUSE), mods);
        }
    }
#endif
    event->accept();
}

void WebkitView::keyPressEvent(QKeyEvent *event)
{
    // QEvent::ShortcutOverride is routed through keyPressEvent too —
    // leave those unclaimed so app-level QShortcuts (Ctrl+T &co)
    // still fire on a webkit tab; a real KeyPress follows when no
    // shortcut claims the sequence.
    if (event->type() == QEvent::ShortcutOverride) {
        event->ignore();
        return;
    }
#ifdef ARKTOS_WEBKIT_LINKED
    if (dispatchKeyEvent(int(WPE_EVENT_KEYBOARD_KEY_DOWN), event)) {
        event->accept();
        return;
    }
#endif
    QWidget::keyPressEvent(event);
}

void WebkitView::keyReleaseEvent(QKeyEvent *event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (dispatchKeyEvent(int(WPE_EVENT_KEYBOARD_KEY_UP), event)) {
        event->accept();
        return;
    }
#endif
    QWidget::keyReleaseEvent(event);
}

// ---- WebkitPage -----------------------------------------------------

WebkitPage::WebkitPage(Engine::Profile *profile, QObject *parent)
    : Engine::Page(parent)
    , m_profile(profile)
{
#ifdef ARKTOS_WEBKIT_LINKED
    WPEDisplay *display = WebkitBackend::sharedDisplay();
    if (!display)
        return;

    WebKitNetworkSession *session = nullptr;
    if (auto *webProfile = qobject_cast<WebkitProfile *>(profile)) {
        session = webProfile->networkSession();
        // WK11: a WebkitProfile whose session failed to build must NOT
        // fall back to the default session — a named profile would
        // silently share the global store across personas, and an OTR
        // profile would land on a PERSISTENT session.  Refusing the
        // page is the honest failure (createPage maps it to "no
        // webview").
        if (!session)
            return;
    }
    if (!session)
        session = webkit_network_session_get_default();
    m_session = session;
    // WK19: the session is the download channel — hook
    // "download-started" once per session (idempotent for the shared
    // default) so transfers route to the owning page's profile.
    ensureDownloadHook(session);

    // WK09: favicon tracking is opt-in per data manager (disabled by
    // default) — without it page-icons never updates and iconChanged
    // could never fire.  Ephemeral managers open the database
    // in-memory, so OTR still writes nothing.
    webkit_website_data_manager_set_favicons_enabled(
        webkit_network_session_get_website_data_manager(session), TRUE);

    // WPE 2.54: WebKitWebView owns its WPEView; the display/context/
    // session are construct-only properties.
    m_webView = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                             "display", display,
                                             "web-context",
                                                 sharedWebContext(),
                                             "network-session",
                                                 session,
                                             nullptr));
    if (!m_webView)
        return;
    m_wpeView = webkit_web_view_get_wpe_view(m_webView);
    m_toplevel = wpe_display_create_toplevel(display, 1);
    if (!m_wpeView || !m_toplevel) {
        g_clear_object(&m_webView);
        g_clear_object(&m_toplevel);
        m_wpeView = nullptr;
        return;
    }
    wpe_view_set_toplevel(m_wpeView, m_toplevel);
    wpe_toplevel_resize(m_toplevel, 800, 600);
    wpe_view_set_visible(m_wpeView, TRUE);
    wpe_view_map(m_wpeView);
    // Focus follows map so the web process considers the view active —
    // WK13's input path needs it, and unfocused views defer paint.
    wpe_view_focus_in(m_wpeView);

    g_signal_connect(m_webView, "load-changed",
                     G_CALLBACK(cbLoadChanged), this);
    g_signal_connect(m_webView, "load-failed",
                     G_CALLBACK(cbLoadFailed), this);
    g_signal_connect(m_webView, "web-process-terminated",
                     G_CALLBACK(cbWebProcessTerminated), this);
    g_signal_connect(m_webView, "notify::uri",
                     G_CALLBACK(cbNotifyUri), this);
    g_signal_connect(m_webView, "notify::title",
                     G_CALLBACK(cbNotifyTitle), this);
    g_signal_connect(m_webView, "notify::estimated-load-progress",
                     G_CALLBACK(cbNotifyProgress), this);
    // WK09: the 2.54 property is "page-icons" (boxed WebKitImageList);
    // "favicon" is the GTK-shaped name the spike notes used — both
    // details connect harmlessly and whichever the build has fires.
    g_signal_connect(m_webView, "notify::page-icons",
                     G_CALLBACK(cbNotifyPageIcons), this);
    g_signal_connect(m_webView, "notify::favicon",
                     G_CALLBACK(cbNotifyPageIcons), this);
    g_signal_connect(m_webView, "decide-policy",
                     G_CALLBACK(cbDecidePolicy), this);
    // WK19: the user-facing surfaces — TLS errors feed the
    // certificateError/interstitial contract, context-menu marshals
    // a ContextMenuInfo + the view's popup, permission-request goes
    // through the SEC05 broker.
    g_signal_connect(m_webView, "load-failed-with-tls-errors",
                     G_CALLBACK(cbLoadFailedTls), this);
    g_signal_connect(m_webView, "context-menu",
                     G_CALLBACK(cbContextMenu), this);
    g_signal_connect(m_webView, "permission-request",
                     G_CALLBACK(cbPermissionRequest), this);
    g_signal_connect(m_wpeView, "buffer-rendered",
                     G_CALLBACK(cbBufferRendered), this);
    // WK14: the find controller is a per-view object owned by the
    // WebKitWebView — stable for the view's life, so one connect in
    // the ctor covers every findText call.
    WebKitFindController *findController =
        webkit_web_view_get_find_controller(m_webView);
    g_signal_connect(findController, "found-text",
                     G_CALLBACK(cbFindFound), this);
    g_signal_connect(findController, "counted-matches",
                     G_CALLBACK(cbFindCounted), this);
    g_signal_connect(findController, "failed-to-find-text",
                     G_CALLBACK(cbFindFailed), this);
    // WK16: the channel handler goes on before any load can arm the
    // channel-world scripts — window.webkit.messageHandlers only
    // materializes in worlds a handler was registered into, which is
    // also what keeps the main world bare of it (the SEC24 shape).
    registerChannelHandler();
    // WK17: the uri-scheme handlers resolve their originating page
    // through this map (the abp: persona lookup, the arktos-file:
    // initiator fallback).
    schemePageMap().insert(m_webView, this);
    // WebKit scopes the UserContentManager per view — the profile's
    // script collection lands on this page's own UCM, applied here
    // and kept in sync by WebkitProfile while the page lives.
    if (auto *webProfile = qobject_cast<WebkitProfile *>(profile)) {
        m_webProfile = webProfile;
        const QList<Engine::Script> profileScripts =
            webProfile->scripts();
        for (const Engine::Script &script : profileScripts)
            installProfileScript(script);
        webProfile->attachPage(this);
        // WK18: a compiled content filter may already exist (the
        // store resolved it before this page was built) — land it on
        // the new UCM before any load can run unfiltered.
        if (WebKitUserContentFilter *filter =
                webProfile->contentFilter()) {
            webkit_user_content_manager_add_filter(
                webkit_web_view_get_user_content_manager(m_webView),
                filter);
        }
    }
#else
    Q_UNUSED(profile);
#endif
}

WebkitPage::~WebkitPage()
{
#ifdef ARKTOS_WEBKIT_LINKED
    schemePageMap().remove(m_webView);
    if (m_webProfile)
        m_webProfile->detachPage(this);
    if (m_view)
        m_view->m_page = nullptr;
    // The WebKitWebView owns its WPEView; unrefing it tears the view
    // down and drops every g_signal handler bound to `this`.  The
    // retained user-script objects are ours to release.
    for (WebKitUserScript *script : std::as_const(m_userScripts))
        webkit_user_script_unref(script);
    for (WebKitUserScript *script :
             std::as_const(m_profileUserScripts))
        webkit_user_script_unref(script);
    m_userScripts.clear();
    m_profileUserScripts.clear();
    // WK19: the retained cert-error certificate and context menu.
    g_clear_object(&m_certErrorCert);
    g_clear_object(&m_contextMenu);
    g_clear_object(&m_webView);
    g_clear_object(&m_toplevel);
#endif
}

void WebkitPage::attachView(WebkitView *view)
{
    m_view = view;
    if (!m_view)
        return;
    m_view->m_page = this;
    if (m_view->width() > 0 && m_view->height() > 0)
        resizeEngine(m_view->width(), m_view->height());
}

void WebkitPage::resizeEngine(int width, int height)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_toplevel && width > 0 && height > 0)
        wpe_toplevel_resize(m_toplevel, width, height);
#else
    Q_UNUSED(width);
    Q_UNUSED(height);
#endif
}

void WebkitPage::focusEngine(bool in)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_wpeView)
        return;
    if (in)
        wpe_view_focus_in(m_wpeView);
    else
        wpe_view_focus_out(m_wpeView);
#else
    Q_UNUSED(in);
#endif
}

// ---- WK13 — the wpe_view_event dispatch points -----------------------

QPointF WebkitPage::viewPoint(const QPointF &widgetPos) const
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_wpeView && m_view && m_view->width() > 0
            && m_view->height() > 0) {
        // The event space is the view's logical pixel size — the
        // toplevel mirrors the widget's DIP size at scale 1, so the
        // ratio is 1:1 today and folds any future scale drift in.
        const double sx = double(wpe_view_get_width(m_wpeView))
                          / double(m_view->width());
        const double sy = double(wpe_view_get_height(m_wpeView))
                          / double(m_view->height());
        return QPointF(widgetPos.x() * sx, widgetPos.y() * sy);
    }
#endif
    return widgetPos;
}

void WebkitPage::sendPointerEvent(int type, double x, double y,
                                  double deltaX, double deltaY,
                                  unsigned mods)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_wpeView)
        return;
    wpe_view_focus_in(m_wpeView);   // see sendKeyEvent — WK04
    WPEEvent *event = wpe_event_pointer_move_new(
        WPEEventType(type), m_wpeView, WPE_INPUT_SOURCE_MOUSE,
        wpeEventTime(), WPEModifiers(mods), x, y, deltaX, deltaY);
    wpe_view_event(m_wpeView, event);
    wpe_event_unref(event);
#else
    Q_UNUSED(type);
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(deltaX);
    Q_UNUSED(deltaY);
    Q_UNUSED(mods);
#endif
}

void WebkitPage::sendButtonEvent(int type, double x, double y,
                                 unsigned button, unsigned pressCount,
                                 unsigned mods)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_wpeView)
        return;
    wpe_view_focus_in(m_wpeView);   // see sendKeyEvent — WK04
    WPEEvent *event = wpe_event_pointer_button_new(
        WPEEventType(type), m_wpeView, WPE_INPUT_SOURCE_MOUSE,
        wpeEventTime(), WPEModifiers(mods), button, x, y, pressCount);
    if (!event)   // ctor refuses press_count on a non-DOWN type
        return;
    wpe_view_event(m_wpeView, event);
    wpe_event_unref(event);
#else
    Q_UNUSED(type);
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(button);
    Q_UNUSED(pressCount);
    Q_UNUSED(mods);
#endif
}

void WebkitPage::sendScrollEvent(double x, double y, double deltaX,
                                 double deltaY, bool precise,
                                 int source, unsigned mods)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_wpeView)
        return;
    wpe_view_focus_in(m_wpeView);   // see sendKeyEvent — WK04
    WPEEvent *event = wpe_event_scroll_new(
        m_wpeView, WPEInputSource(source), wpeEventTime(),
        WPEModifiers(mods), deltaX, deltaY, gboolean(precise), FALSE,
        x, y);
    wpe_view_event(m_wpeView, event);
    wpe_event_unref(event);
#else
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(deltaX);
    Q_UNUSED(deltaY);
    Q_UNUSED(precise);
    Q_UNUSED(source);
    Q_UNUSED(mods);
#endif
}

void WebkitPage::sendKeyEvent(int type, unsigned keycode,
                              unsigned keyval, unsigned mods)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_wpeView)
        return;
    // Input is dropped by an unfocused view (WK04) — re-asserting
    // here is a no-op while focused and covers the focus reset a
    // committed navigation performs.
    wpe_view_focus_in(m_wpeView);
    WPEEvent *event = wpe_event_keyboard_new(
        WPEEventType(type), m_wpeView, WPE_INPUT_SOURCE_KEYBOARD,
        wpeEventTime(), WPEModifiers(mods), keycode, keyval);
    if (!event)
        return;
    wpe_view_event(m_wpeView, event);
    wpe_event_unref(event);
#else
    Q_UNUSED(type);
    Q_UNUSED(keycode);
    Q_UNUSED(keyval);
    Q_UNUSED(mods);
#endif
}

void WebkitPage::load(const QUrl &url)
{
    if (!url.isValid())
        return;
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    wakeForNavigation();   // WK14 — an explicit load revives a discard
    m_pendingBfNavUri.clear();   // supersedes any in-flight bf hop
    const QByteArray uri = url.toString().toUtf8();
    webkit_web_view_load_uri(m_webView, uri.constData());
#else
    Q_UNUSED(url);
#endif
}

void WebkitPage::stop()
{
#ifdef ARKTOS_WEBKIT_LINKED
    m_pendingBfNavUri.clear();   // a user stop reports its cancel
    if (m_webView)
        webkit_web_view_stop_loading(m_webView);
#endif
}

void WebkitPage::reload()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView) {
        wakeForNavigation();
        webkit_web_view_reload(m_webView);
    }
#endif
}

QUrl WebkitPage::url() const
{
    return m_url;
}

bool WebkitPage::canGoBack() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView)
        return webkit_web_view_can_go_back(m_webView);
#endif
    return false;
}

bool WebkitPage::canGoForward() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView)
        return webkit_web_view_can_go_forward(m_webView);
#endif
    return false;
}

void WebkitPage::back()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView && webkit_web_view_can_go_back(m_webView)) {
        // decide-policy does not fire for back-forward navigations
        // (verified on 2.54 — the history-item path never consults
        // the embedder policy client), so the embedder-initiated bf
        // navigation stamps its own type.  WK17 note: this also
        // means bf hops cannot be vetoed through decide-policy.
        m_lastNavigationType = Engine::NavigationType::BackOrForward;
        // The history machinery cancels its own provisional request
        // between COMMITTED and FINISHED — onLoadFailed suppresses
        // that artifact for the uri we are about to land on.
        WebKitBackForwardList *list =
            webkit_web_view_get_back_forward_list(m_webView);
        m_pendingBfNavUri =
            bfItemUri(webkit_back_forward_list_get_back_item(list));
        wakeForNavigation();
        webkit_web_view_go_back(m_webView);
    }
#endif
}

void WebkitPage::forward()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView && webkit_web_view_can_go_forward(m_webView)) {
        m_lastNavigationType = Engine::NavigationType::BackOrForward;
        WebKitBackForwardList *list =
            webkit_web_view_get_back_forward_list(m_webView);
        m_pendingBfNavUri =
            bfItemUri(webkit_back_forward_list_get_forward_item(list));
        wakeForNavigation();
        webkit_web_view_go_forward(m_webView);
    }
#endif
}

int WebkitPage::historyCount() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    const BackForwardShape shape = backForwardShape(m_webView);
    if (shape.hasCurrent)
        return shape.backCount + 1 + shape.forwardCount;
#endif
    return 0;
}

int WebkitPage::currentHistoryIndex() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    const BackForwardShape shape = backForwardShape(m_webView);
    if (shape.hasCurrent)
        return shape.backCount;   // back items precede current
#endif
    return -1;
}

QList<Engine::HistoryEntry> WebkitPage::historyItems() const
{
    QList<Engine::HistoryEntry> out;
#ifdef ARKTOS_WEBKIT_LINKED
    WebKitBackForwardList *list =
        m_webView ? webkit_web_view_get_back_forward_list(m_webView)
                  : nullptr;
    WebKitBackForwardListItem *current =
        list ? webkit_back_forward_list_get_current_item(list)
             : nullptr;
    if (!current)
        return out;
    GList *back = webkit_back_forward_list_get_back_list(list);
    GList *forward = webkit_back_forward_list_get_forward_list(list);
    for (GList *l = g_list_last(back); l; l = l->prev)
        out.append(entryFromBfItem(
            WEBKIT_BACK_FORWARD_LIST_ITEM(l->data), out.count()));
    out.append(entryFromBfItem(current, out.count()));
    for (GList *l = g_list_last(forward); l; l = l->prev)
        out.append(entryFromBfItem(
            WEBKIT_BACK_FORWARD_LIST_ITEM(l->data), out.count()));
    g_list_free(back);
    g_list_free(forward);
#endif
    return out;
}

QList<Engine::HistoryEntry> WebkitPage::backItems(int maxItems) const
{
    const int current = currentHistoryIndex();
    if (current < 0)
        return {};
    return historyItems().mid(qMax(0, current - maxItems),
                              qMin(maxItems, current));
}

QList<Engine::HistoryEntry> WebkitPage::forwardItems(int maxItems) const
{
    const int current = currentHistoryIndex();
    if (current < 0)
        return {};
    return historyItems().mid(current + 1, maxItems);
}

void WebkitPage::goToHistoryEntry(const Engine::HistoryEntry &entry)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView || entry.index < 0)
        return;
    WebKitBackForwardList *list =
        webkit_web_view_get_back_forward_list(m_webView);
    if (!list)
        return;
    // entry.index is a stack position our own lists produced;
    // WebKit's nth item addressing is relative to the current item.
    const int relative =
        entry.index - backForwardShape(m_webView).backCount;
    WebKitBackForwardListItem *item =
        webkit_back_forward_list_get_nth_item(list, relative);
    if (item) {
        m_lastNavigationType = Engine::NavigationType::BackOrForward;
        m_pendingBfNavUri = bfItemUri(item);
        wakeForNavigation();
        webkit_web_view_go_to_back_forward_list_item(m_webView, item);
    }
#else
    Q_UNUSED(entry);
#endif
}

void WebkitPage::setZoomFactor(qreal factor)
{
    // WK14: the zoom level is a positive gdouble — a zero/negative or
    // non-finite factor would hand the engine a broken scale.
    if (!qIsFinite(factor) || factor <= 0.0)
        return;
    m_zoom = factor;
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView)
        webkit_web_view_set_zoom_level(m_webView, factor);
#endif
}

qreal WebkitPage::zoomFactor() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView)
        return webkit_web_view_get_zoom_level(m_webView);
#endif
    return m_zoom;
}

void WebkitPage::findText(const QString &subString,
                          Engine::FindFlags options)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView) {
        WebKitFindController *controller =
            webkit_web_view_get_find_controller(m_webView);
        if (subString.isEmpty()) {
            // The Qt contract for an empty needle is "clear the
            // search" — highlights off, counter blanked by the
            // WebViewSearch handler.
            webkit_find_controller_search_finish(controller);
            m_findText.clear();
            m_findOrdinal = 0;
            m_findCount = 0;
        } else if (subString == m_findText
                   && options == m_findFlags) {
            // Same query — step to the next/previous match rather
            // than re-searching (a fresh search() would restart the
            // match walk at the document position).  found-text /
            // failed-to-find-text deliver the verdict; the ordinal
            // steps optimistically here and onFindFound wraps/clamps
            // it once the count lands.
            m_findOrdinal += (options & Engine::FindBackward) ? -1 : 1;
            if (options & Engine::FindBackward)
                webkit_find_controller_search_previous(controller);
            else
                webkit_find_controller_search_next(controller);
            return;
        } else {
            m_findText = subString;
            m_findFlags = options;
            // 0 marks "fresh backward search — ordinal unknown until
            // the count arrives" (onFindFound resolves it to the
            // last match); forward searches land on match 1.
            m_findOrdinal = (options & Engine::FindBackward) ? 0 : 1;
            m_findCount = 0;
            const QByteArray needle = subString.toUtf8();
            webkit_find_controller_search(
                controller, needle.constData(),
                wkFindOptions(options), guint(-1));
            return;
        }
    }
#else
    Q_UNUSED(subString);
    Q_UNUSED(options);
#endif
    Engine::FindResult result;
    Q_EMIT findTextFinished(result);
}

void WebkitPage::runJavaScript(
        const QString &source,
        const std::function<void(const QVariant &)> &resultCallback)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView) {
        EvalCall *call = new EvalCall;
        call->page = this;
        call->callback = resultCallback;
        const QByteArray script = source.toUtf8();
        webkit_web_view_evaluate_javascript(
            m_webView, script.constData(), script.size(),
            nullptr, nullptr, nullptr, evalReady, call);
        return;
    }
#else
    Q_UNUSED(source);
#endif
    if (resultCallback)
        resultCallback(QVariant());
}

void WebkitPage::runJavaScriptLifted(
        const QString &source,
        const std::function<void(const QVariant &)> &resultCallback,
        quint32 worldId)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_webView) {
        // WK16: lifted eval runs in the mapped named world — the
        // reader driver's isolated world and the channel world both
        // land where page script cannot follow.  This backend wires
        // no JS gate to lift (setPageAttribute stores attributes but
        // does not apply WebKitSettings::enable-javascript), so the
        // world is the whole contract delta vs runJavaScript.
        EvalCall *call = new EvalCall;
        call->page = this;
        call->callback = resultCallback;
        const QByteArray script = source.toUtf8();
        const QByteArray world = scriptWorldName(worldId);
        webkit_web_view_evaluate_javascript(
            m_webView, script.constData(), script.size(),
            world.isEmpty() ? nullptr : world.constData(),
            nullptr, nullptr, evalReady, call);
        return;
    }
#else
    Q_UNUSED(source);
    Q_UNUSED(worldId);
#endif
    if (resultCallback)
        resultCallback(QVariant());
}

void WebkitPage::toHtml(
        const std::function<void(const QString &)> &resultCallback)
{
    // WK15: serialized markup is the document element's outerHTML —
    // the same eval channel WebEngine's toHtml rides under the hood.
    // A document-less page answers null -> invalid -> empty string.
    runJavaScript(QStringLiteral("document.documentElement.outerHTML"),
                  [resultCallback](const QVariant &v) {
        if (resultCallback)
            resultCallback(v.toString());
    });
}

QAction *WebkitPage::action(Engine::StandardAction action)
{
    Q_UNUSED(action);
    return nullptr;
}

bool WebkitPage::isLoading() const
{
    return m_loading;
}

bool WebkitPage::recentlyAudible() const
{
    return false;
}

bool WebkitPage::isOffTheRecord() const
{
    return m_profile && m_profile->isOffTheRecord();
}

void WebkitPage::setPageAttribute(const QString &name, bool on)
{
    // No per-page attribute application on this backend — the
    // override exists so the interface's free-form keys are ignored
    // harmlessly instead of misapplied.
    Q_UNUSED(name);
    Q_UNUSED(on);
}

void WebkitPage::setLifecycleState(LifecycleState state)
{
    if (state == m_lifecycle)
        return;
    const LifecycleState old = m_lifecycle;
    m_lifecycle = state;
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    if (state == LifecycleState::Discarded) {
        // A Frozen surface is always undone before the renderer dies
        // so the wake path has one clean state to recover.
        if (old == LifecycleState::Frozen)
            thawEngine();
        // terminate kills the WebProcess outright — the whole point
        // of the discard (the UI-side WebKitBackForwardList survives,
        // so waking is reload(), not a session-state re-create).
        // An empty page has nothing to drop.
        if (webkit_web_view_get_uri(m_webView)) {
            webkit_web_view_terminate_web_process(m_webView);
            m_engineDiscarded = true;
        }
    } else if (state == LifecycleState::Frozen) {
        if (m_engineDiscarded) {
            // A dead renderer cannot freeze-then-thaw back alive —
            // Active is the only way out of a discard; the request
            // stays honestly Discarded.
            m_lifecycle = LifecycleState::Discarded;
            return;
        }
        freezeEngine();
    } else {   // Active
        if (m_engineDiscarded) {
            m_engineDiscarded = false;
            // reload() on a terminated view respawns the WebProcess
            // at the current history entry — the load signals land
            // exactly like a user reload.
            webkit_web_view_reload(m_webView);
        } else if (old == LifecycleState::Frozen) {
            thawEngine();
        }
    }
#else
    Q_UNUSED(old);
#endif
}

WebkitPage::LifecycleState WebkitPage::lifecycleState() const
{
    return m_lifecycle;
}

// WK14 — the engine-side halves of the lifecycle transitions.
// freezeEngine prefers the real suspend export (absent on 2.54 — the
// dlsym gate); the fallback unmaps and unfocuses the WPEView, which
// stops the engine compositing the page — the interface's own
// "loaded but not compositing" definition of Frozen.  JS timers keep
// running, which is the honest delta vs a true process suspend.
void WebkitPage::freezeEngine()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (wkSuspendResumeAvailable()) {
        webkitViewExport("webkit_web_view_suspend")(m_webView);
        m_suspended = true;
        return;
    }
    if (m_wpeView && wpe_view_get_mapped(m_wpeView)) {
        wpe_view_focus_out(m_wpeView);
        wpe_view_unmap(m_wpeView);
        m_frozenUnmapped = true;
    }
#endif
}

void WebkitPage::thawEngine()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_suspended) {
        if (WebkitViewVoidFn resume =
                webkitViewExport("webkit_web_view_resume"))
            resume(m_webView);
        m_suspended = false;
    }
    if (m_frozenUnmapped) {
        wpe_view_map(m_wpeView);
        wpe_view_focus_in(m_wpeView);
        m_frozenUnmapped = false;
    }
#endif
}

// Every navigation is use-intent — a discarded page revives to
// Active first (the navigation itself respawns the terminated web
// process; no separate reload needed).
void WebkitPage::wakeForNavigation()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_lifecycle == LifecycleState::Discarded) {
        m_lifecycle = LifecycleState::Active;
        m_engineDiscarded = false;
        thawEngine();   // paranoia — the discard path already thawed
    }
#endif
}

// WK14 test/diagnostic surface — the suspend/resume symbol-gate
// verdict and the WPEView's mapped state (the Frozen fallback's
// observable half).
bool WebkitPage::suspendResumeAvailable()
{
#ifdef ARKTOS_WEBKIT_LINKED
    return wkSuspendResumeAvailable();
#else
    return false;
#endif
}

bool WebkitPage::engineViewMapped() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    return m_wpeView && wpe_view_get_mapped(m_wpeView);
#else
    return false;
#endif
}

qint64 WebkitPage::renderProcessId() const
{
    // No renderer-PID API exists in WPE WebKit 2.54 (named spike
    // gap — get_page_id is an internal PageID, not an OS pid).
    return -1;
}

void WebkitPage::insertScript(const Engine::Script &script)
{
    m_scripts.insert(script.name, script);
    installUserScript(script, m_userScripts);
}

void WebkitPage::removeScript(const QString &name)
{
    m_scripts.remove(name);
    removeUserScript(name, m_userScripts);
}

// WK16 — the UCM halves.  A same-name insert replaces the live
// instance first so the manager never runs the stale copy alongside
// the new one; the retained pointer is what remove_script needs
// later.
void WebkitPage::installUserScript(
        const Engine::Script &script,
        QHash<QString, WebKitUserScript *> &installed)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    removeUserScript(script.name, installed);
    WebKitUserScript *userScript = userScriptFor(script);
    if (!userScript) {
        qWarning("webkit: user script %s failed to build",
                 qPrintable(script.name));
        return;
    }
    webkit_user_content_manager_add_script(
        webkit_web_view_get_user_content_manager(m_webView),
        userScript);
    installed.insert(script.name, userScript);
#else
    Q_UNUSED(script);
    Q_UNUSED(installed);
#endif
}

void WebkitPage::removeUserScript(
        const QString &name,
        QHash<QString, WebKitUserScript *> &installed)
{
#ifdef ARKTOS_WEBKIT_LINKED
    WebKitUserScript *userScript = installed.take(name);
    if (!userScript)
        return;
    if (m_webView) {
        webkit_user_content_manager_remove_script(
            webkit_web_view_get_user_content_manager(m_webView),
            userScript);
    }
    webkit_user_script_unref(userScript);
#else
    Q_UNUSED(name);
    Q_UNUSED(installed);
#endif
}

// WebkitProfile's propagation entry points — same install path, the
// profile-scope hash so a profile script never aliases a page one.
void WebkitPage::installProfileScript(const Engine::Script &script)
{
    installUserScript(script, m_profileUserScripts);
}

void WebkitPage::removeProfileScript(const QString &name)
{
    removeUserScript(name, m_profileUserScripts);
}

void WebkitPage::registerChannelHandler()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    WebKitUserContentManager *manager =
        webkit_web_view_get_user_content_manager(m_webView);
    if (!manager)
        return;
    // The docs' ordering rule: connect the detailed signals BEFORE
    // registering the handler name so no early message is missed.
    // The handler lives only in the channel world — the main world
    // never grows a window.webkit.messageHandlers surface at all.
    g_signal_connect(manager,
                     "script-message-received::arktosChannel",
                     G_CALLBACK(cbScriptMessage), this);
    g_signal_connect(manager,
                     "script-message-with-reply-received::arktosChannel",
                     G_CALLBACK(cbScriptMessageWithReply), this);
    const QByteArray world =
        scriptWorldName(Engine::ArktosChannelWorld);
    if (RegisterScriptMessageWithReplyFn registerWithReply =
            wkRegisterWithReply()) {
        registerWithReply(manager, kChannelHandlerName,
                          world.constData());
    } else {
        // Honest degradation: the message still arrives on the
        // one-way signal — postMessage just returns no Promise.
        webkit_user_content_manager_register_script_message_handler(
            manager, kChannelHandlerName, world.constData());
    }
#endif
}

void WebkitPage::onChannelMessage(JSCValue *value,
                                  WebKitScriptMessageReply *reply)
{
#ifdef ARKTOS_WEBKIT_LINKED
    const QVariant payload = jscValueToVariant(value);
    if (!reply) {
        // One-way registration — deliver and drop; the JS side has
        // no Promise to answer.
        if (m_channelListener)
            m_channelListener(payload);
        return;
    }
    if (!m_channelListener) {
        // No fake ack — the JS-side Promise rejects with the truth.
        webkit_script_message_reply_return_error_message(
            reply, "arktos channel has no listener on this page");
        return;
    }
    const QVariant out = m_channelListener(payload);
    JSCContext *context = jsc_value_get_context(value);
    JSCValue *replyValue = variantToJsc(context, out);
    if (!replyValue)
        replyValue = jsc_value_new_undefined(context);
    webkit_script_message_reply_return_value(reply, replyValue);
    g_object_unref(replyValue);
#else
    Q_UNUSED(value);
    Q_UNUSED(reply);
#endif
}

// WK16 diagnostics/test surface — the pure world-name map works
// unlinked too, the reply gate reports the dlsym verdict.
QByteArray WebkitPage::scriptWorldName(quint32 worldId)
{
    // Engine worldIds are arbitrary 32-bit tags; WebKit isolates by
    // world NAME.  0 is the default/main world (NULL world_name);
    // the SEC24 channel world gets a stable readable name; every
    // other id maps deterministically so two callers sharing an id
    // share the world — QWebEngineScript's worldId join semantics.
    if (!worldId)
        return QByteArray();
    if (worldId == Engine::ArktosChannelWorld)
        return QByteArrayLiteral("arktos-channel");
    return QByteArrayLiteral("arktos-world-")
        + QByteArray::number(worldId, 16);
}

QByteArray WebkitPage::channelHandlerName()
{
    return QByteArrayLiteral("arktosChannel");
}

bool WebkitPage::scriptMessageReplyAvailable()
{
#ifdef ARKTOS_WEBKIT_LINKED
    return wkScriptMessageReplyAvailable();
#else
    return false;
#endif
}

void WebkitPage::setChannelMessageListener(
        const std::function<QVariant(const QVariant &)> &listener)
{
    m_channelListener = listener;
}

QList<Engine::Script> WebkitPage::scripts() const
{
    return m_scripts.values();
}

void WebkitPage::download(const QUrl &url)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    // The view-scoped download (DownloadManager::pageDownload's
    // entry): it surfaces on the session's "download-started" like a
    // content-disposition transfer and routes to this page's profile
    // through schemePageMap.
    webkit_web_view_download_uri(
        m_webView, url.toString().toUtf8().constData());
#else
    Q_UNUSED(url);
#endif
}

QWidget *WebkitPage::view() const
{
    return m_view;
}

// ---- WebkitPage GLib signal handling --------------------------------
//
// Every handler below runs on the Qt GUI thread — the WK02 verdict:
// QPAEventDispatcherGlib pumps the global-default GMainContext where
// WPE/WebKit attach their sources, so the signals land inline during
// QCoreApplication::processEvents.

#ifdef ARKTOS_WEBKIT_LINKED

void WebkitPage::cbBufferRendered(WPEView *, WPEBuffer *buffer,
                                  WebkitPage *page)
{
    page->onBufferRendered(buffer);
}

void WebkitPage::cbLoadChanged(WebKitWebView *, int event,
                               WebkitPage *page)
{
    page->onLoadChanged(event);
}

int WebkitPage::cbLoadFailed(WebKitWebView *, int, void *uri,
                             void *error, WebkitPage *page)
{
    const GError *gerror = static_cast<const GError *>(error);
    const bool cancelled = gerror
        && gerror->domain == WEBKIT_NETWORK_ERROR
        && gerror->code == WEBKIT_NETWORK_ERROR_CANCELLED;
    page->onLoadFailed(QString::fromUtf8(
        uri ? static_cast<const char *>(uri) : ""), cancelled);
    // TRUE claims the failure: WebKit's default handler would load a
    // built-in error page, and that nested load would emit a fresh
    // loadStarted/loadFinished(true) pair that masks the failed
    // navigation behind a fake success.
    return TRUE;
}

void WebkitPage::cbWebProcessTerminated(WebKitWebView *, int,
                                        WebkitPage *page)
{
    page->onWebProcessTerminated();
}

void WebkitPage::cbNotifyUri(WebKitWebView *, void *,
                             WebkitPage *page)
{
    page->onUriNotify();
}

void WebkitPage::cbNotifyTitle(WebKitWebView *, void *,
                               WebkitPage *page)
{
    page->onTitleNotify();
}

void WebkitPage::cbNotifyProgress(WebKitWebView *, void *,
                                  WebkitPage *page)
{
    page->onProgressNotify();
}

void WebkitPage::cbNotifyPageIcons(WebKitWebView *, void *,
                                   WebkitPage *page)
{
    page->onPageIconsNotify();
}

int WebkitPage::cbDecidePolicy(WebKitWebView *,
                               WebKitPolicyDecision *decision,
                               int type, WebkitPage *page)
{
    return page->onDecidePolicy(decision, type) ? TRUE : FALSE;
}

void WebkitPage::cbFindFound(WebKitFindController *,
                             unsigned matchCount, WebkitPage *page)
{
    page->onFindFound(matchCount);
}

void WebkitPage::cbFindCounted(WebKitFindController *,
                               unsigned matchCount, WebkitPage *page)
{
    page->onFindCounted(matchCount);
}

void WebkitPage::cbFindFailed(WebKitFindController *,
                              WebkitPage *page)
{
    page->onFindFailed();
}

void WebkitPage::cbScriptMessage(WebKitUserContentManager *,
                                 JSCValue *value, WebkitPage *page)
{
    page->onChannelMessage(value, nullptr);
}

int WebkitPage::cbScriptMessageWithReply(
        WebKitUserContentManager *, JSCValue *value,
        WebKitScriptMessageReply *reply, WebkitPage *page)
{
    page->onChannelMessage(value, reply);
    return TRUE;
}

int WebkitPage::cbLoadFailedTls(WebKitWebView *, void *uri,
                                void *certificate, unsigned flags,
                                WebkitPage *page)
{
    page->onTlsError(QString::fromUtf8(
        uri ? static_cast<const char *>(uri) : ""),
        G_TLS_CERTIFICATE(certificate), flags);
    // TRUE claims the failure — the interstitial contract answers it,
    // never WebKit's built-in error page (the cbLoadFailed shape).
    return TRUE;
}

int WebkitPage::cbContextMenu(WebKitWebView *, void *menu,
                              void *hit, WebkitPage *page)
{
    page->onContextMenu(WEBKIT_CONTEXT_MENU(menu),
                        WEBKIT_HIT_TEST_RESULT(hit));
    // Claimed either way — the view presents the popup; an ignored
    // menu is a deliberate refusal, not a missing handler.
    return TRUE;
}

int WebkitPage::cbPermissionRequest(WebKitWebView *, void *request,
                                    WebkitPage *page)
{
    page->onPermissionRequest(WEBKIT_PERMISSION_REQUEST(request));
    return TRUE;
}

#endif // ARKTOS_WEBKIT_LINKED

void WebkitPage::onBufferRendered(WPEBuffer *buffer)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!buffer || !m_view)
        return;
#if defined(ARKTOS_WEBKIT_GL)
    // WK20: try the dmabuf -> EGLImage -> GL texture import first.
    // Any decline (no EGL display, non-importable buffer, GL error)
    // falls through to the raster copy below — ARKTOS_WEBKIT_GL=0
    // pins raster for A/B timing runs and EGL-less environments.
    if (qgetenv("ARKTOS_WEBKIT_GL") != "0") {
        WkGlPipe &p = wkGlPipe();
        QImage glFrame;
        if (wkGlImport(buffer, &glFrame)) {
            if (!p.calibrated) {
                // First GL-imported frame doubles as the channel-order
                // calibration: compare its raw readback against the
                // raster ground truth, then present the trusted copy.
                QImage raster = wkRasterImage(buffer);
                if (!raster.isNull()) {
                    wkGlCalibrate(glFrame, raster);
                    m_view->adoptFrameImage(raster);
                    return;
                }
                // No ground truth — adopt GL with the identity
                // default and stop asking.
                p.calibrated = true;
            }
            if (p.state == WkGlPipe::Ready) {
                m_view->adoptGlFrame(glFrame);
                return;
            }
            // Calibration never converged — pipe is Broken, fall
            // through to raster for good.
        }
        m_view->noteGlImportFailure();
    }
#endif
    // WK03 contract: WebKit (the wpe_view_render_buffer caller) owns
    // the rendered/released bookkeeping — the embedder only imports +
    // copies inside the handler (wkRasterImage keeps the GBytes
    // borrowed, never unrefed).
    QElapsedTimer rasterTimer;
    rasterTimer.start();
    const QImage raster = wkRasterImage(buffer);
    if (raster.isNull())
        return;
    m_view->adoptFrameImage(raster);
#if defined(ARKTOS_WEBKIT_GL)
    // WK20 frame-time accounting — the raster side of the
    // ARKTOS_WEBKIT_FRAME_TIMING comparison line.
    WkGlPipe &p = wkGlPipe();
    ++p.rasterFrames;
    p.rasterNs += rasterTimer.nsecsElapsed();
#endif
#else
    Q_UNUSED(buffer);
#endif
}

void WebkitPage::onLoadChanged(int event)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (event == int(WEBKIT_LOAD_STARTED)) {
        m_loading = true;
        m_progressSeen = false;
        Q_EMIT loadStarted();
    } else if (event == int(WEBKIT_LOAD_REDIRECTED)
               || event == int(WEBKIT_LOAD_COMMITTED)) {
        // WK09: a redirect hop or the commit point both move the
        // page's uri — notify::uri carries the same update, this just
        // lands it at the event instead of a property pump later.
        onUriNotify();
        // WK14: find state is per-document — a commit retires it so
        // a repeated needle searches the new document instead of
        // stepping a dead controller query.
        m_findText.clear();
        m_findOrdinal = 0;
        m_findCount = 0;
    } else if (event == int(WEBKIT_LOAD_FINISHED)) {
        // A load-failed already answered this load — a stray FINISHED
        // for the dead navigation must not report success (the
        // error-page load itself is suppressed in cbLoadFailed).
        m_pendingBfNavUri.clear();
        if (!m_loading)
            return;
        m_loading = false;
        if (!m_progressSeen) {
            m_progressSeen = true;
            Q_EMIT loadProgress(100);
        }
        Q_EMIT loadFinished(true);
    }
#else
    Q_UNUSED(event);
#endif
}

void WebkitPage::onLoadFailed(const QString &failingUri,
                              bool cancelled)
{
    // WK10: a back/forward navigation's own provisional request is
    // cancelled by the history machinery between COMMITTED and
    // FINISHED — an artifact, not a failure: the navigation still
    // lands its FINISHED.  Suppress exactly that cancel (failing uri
    // == the bf target we armed) and keep m_loading so FINISHED
    // emits the real verdict.  A cancelled load of a DIFFERENT uri
    // (an in-flight load a bf hop displaced) still reports false —
    // but keeps the flag armed, the bf artifact is still coming.
    // stop()/load() disarm the flag so their cancels stay honest.
    if (cancelled) {
        if (!m_pendingBfNavUri.isEmpty()
                && failingUri == m_pendingBfNavUri)
            return;
    } else {
        m_pendingBfNavUri.clear();   // genuine failure — nav is dead
    }
    if (!m_loading)
        return;
    m_loading = false;
    Q_EMIT loadFinished(false);
}

void WebkitPage::onWebProcessTerminated()
{
    m_pendingBfNavUri.clear();
    if (m_loading) {
        m_loading = false;
        Q_EMIT loadFinished(false);
    }
}

void WebkitPage::onUriNotify()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    const char *uri = webkit_web_view_get_uri(m_webView);
    const QUrl url(QString::fromUtf8(uri ? uri : ""));
    if (url == m_url)
        return;
    m_url = url;
    Q_EMIT urlChanged(url);
#endif
}

void WebkitPage::onTitleNotify()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    const char *title = webkit_web_view_get_title(m_webView);
    const QString text = QString::fromUtf8(title ? title : "");
    if (text == m_title)
        return;
    m_title = text;
    Q_EMIT titleChanged(text);
#endif
}

void WebkitPage::onProgressNotify()
{
#ifdef ARKTOS_WEBKIT_LINKED
    // WebKit resets estimated-load-progress to 0 when a load dies —
    // emitting that reset after loadFinished would put a stale 0 on
    // the progress bar and break the started→progress→finished
    // contract.  The FINISHED fallback already delivers the 100.
    if (!m_webView || !m_loading)
        return;
    const double fraction =
        webkit_web_view_get_estimated_load_progress(m_webView);
    const int percent = qBound(0, int(fraction * 100.0), 100);
    Q_EMIT loadProgress(percent);
    if (percent >= 100)
        m_progressSeen = true;   // FINISHED won't re-emit the 100
#endif
}

void WebkitPage::onPageIconsNotify()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_webView)
        return;
    // get_page_icons hands back a borrowed WebKitImageList — WebKit
    // already re-queried the favicon database before notifying, so
    // reading it inline is the whole job.
    const QImage icon = bestPageIcon(webkit_web_view_get_page_icons(m_webView));
    if (icon.isNull())
        return;
    // Content-dedup: page-icons re-notifies while the candidate set
    // settles mid-load; the tab strip only needs iconChanged when the
    // picked icon actually differs.
    if (!m_iconImage.isNull() && m_iconImage == icon)
        return;
    m_iconImage = icon;
    Q_EMIT iconChanged(QIcon(QPixmap::fromImage(icon)));
#endif
}

bool WebkitPage::onDecidePolicy(WebKitPolicyDecision *decision, int type)
{
#ifdef ARKTOS_WEBKIT_LINKED
    // WK19 — the response lane's download verdict: WebKit asks the
    // embedder what to do with a response whose MIME it cannot
    // render or that arrived as Content-Disposition: attachment.
    // Answering "download" routes the transfer onto the session's
    // download channel (download-started → DownloadManager) —
    // Chromium's attachment auto-download parity.
    if (type == int(WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
            && WEBKIT_IS_RESPONSE_POLICY_DECISION(decision)) {
        WebKitResponsePolicyDecision *response =
            WEBKIT_RESPONSE_POLICY_DECISION(decision);
        const bool renderable =
            webkit_response_policy_decision_is_mime_type_supported(
                response);
        WebKitURIResponse *uriResponse =
            webkit_response_policy_decision_get_response(response);
        SoupMessageHeaders *headers = uriResponse
            ? webkit_uri_response_get_http_headers(uriResponse)
            : nullptr;
        const char *disposition = headers
            ? soup_message_headers_get_one(headers, "Content-Disposition")
            : nullptr;
        const bool attachment = disposition
            && g_ascii_strncasecmp(disposition, "attachment", 10) == 0;
        if (!renderable || attachment) {
            webkit_policy_decision_download(decision);
            return true;
        }
        return false;
    }
    const bool navigationDecision =
        (type == int(WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
         || type == int(WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION))
        && WEBKIT_IS_NAVIGATION_POLICY_DECISION(decision);
    if (!navigationDecision)
        return false;
    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    if (!action)
        return false;
    m_lastNavigationType = engineNavigationType(
        webkit_navigation_action_get_navigation_type(action),
        webkit_navigation_action_is_redirect(action));

    // WK17 — the navigation-layer RequestPolicy consult.  Response
    // decisions stay observe-only (the early return above): the
    // subresource lane is WK18's content-blocker answer.
    WebKitURIRequest *uriRequest =
        webkit_navigation_action_get_request(action);
    const char *uri = uriRequest
        ? webkit_uri_request_get_uri(uriRequest) : nullptr;
    const QUrl url = QUrl::fromEncoded(uri ? uri : "");
    // A new-window action's frame name is the TARGET (e.g. "_blank"),
    // not a subframe marker — the action itself is main-frame.
    const bool mainFrame =
        type == int(WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        || !webkit_navigation_action_get_frame_name(action);

    // WK19 — the cert-error interstitial contract (WebPage's
    // acceptNavigationRequest spelling): the published page navigates
    // to arktos-cert-error:interstitial?n=<nonce>, its action links
    // resolve the pending decision, and anything else on the scheme —
    // a forged or stale nonce — is dropped.  Main-frame only, before
    // the RequestPolicy consult so the interstitial's own navigations
    // can never be vetoed into a loop.
    if (url.scheme() == QLatin1String("arktos-cert-error")) {
        if (!mainFrame) {
            webkit_policy_decision_ignore(decision);
            return true;
        }
        const QString nonce = QUrlQuery(url)
            .queryItemValue(QLatin1String("n"));
        if (url.path() == QLatin1String("interstitial")) {
            if (SchemeAccessHandler::hasInterstitialPage(nonce)) {
                webkit_policy_decision_use(decision);
                m_lastNavTarget = url;
                return true;
            }
            webkit_policy_decision_ignore(decision);
            return true;
        }
        webkit_policy_decision_ignore(decision);
        if (m_certErrorPending && nonce == m_certErrorNonce)
            resolveCertificateErrorLink(url);
        return true;
    }

    Engine::RequestPolicy *policy =
        m_webProfile ? m_webProfile->requestPolicy() : nullptr;
    if (policy) {
        Engine::NavigationRequest request;
        request.url = url;
        request.resourceType = mainFrame
            ? Engine::ResourceType::MainFrame
            : Engine::ResourceType::SubFrame;
        request.navigationType = m_lastNavigationType;
        request.firstPartyUrl = m_url;
        request.isMainFrame = mainFrame;
        // initiatorOrigin stays empty — WebKit exposes no initiator
        // on a navigation action; purpose likewise (no fetch-metadata
        // surface).
        const Engine::RequestDecision verdict =
            policy->decide(request);
        if (verdict.action == Engine::RequestAction::Block) {
            webkit_policy_decision_ignore(decision);
            return true;
        }
        if (verdict.action == Engine::RequestAction::Redirect) {
            // A navigation decision cannot rewrite its request —
            // refuse and re-issue the load at the verdict's URL (an
            // arktos-resource: stub or a rewritten location both
            // serve through the scheme registry).  A NEW_WINDOW
            // redirect has no view to retarget — it degrades to the
            // refusal.  The referer override has no expression either:
            // WebKitURIRequest headers are not mutable on a
            // navigation decision — the spike's named gap, so nothing
            // is faked.
            webkit_policy_decision_ignore(decision);
            if (type == int(WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
                    && !verdict.redirectUrl.isEmpty()) {
                const QByteArray target =
                    verdict.redirectUrl.toString().toUtf8();
                webkit_web_view_load_uri(m_webView, target.constData());
            }
            return true;
        }
    }

    // External schemes — a mailto:/tel:/magnet: navigation can only
    // be serviced by handing the URL to the desktop, so the
    // navigation is always refused and the SEC02 consent prompt runs
    // queued (main-frame only — an iframe must not raise dialogs on
    // the user's behalf).  The allowlist is WebPage's own
    // isBrowserHandledScheme so both engines run the same list.
    if (!url.scheme().isEmpty()
            && !WebPage::isBrowserHandledScheme(url.scheme())) {
        webkit_policy_decision_ignore(decision);
        if (mainFrame)
            WebPage::promptOpenExternalUrl(view(), url,
                                           this->url().toString());
        return true;
    }

    // The directory-listing reroute — chrome-level on every engine:
    // file:// is built-in and takes no scheme handler, so a
    // main-frame file: navigation at a directory is served the
    // arktos-file: listing instead (WebPage::acceptNavigationRequest
    // parity).
    if (type == int(WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
            && mainFrame
            && url.scheme() == QLatin1String("file")
            && QFileInfo(url.toLocalFile()).isDir()) {
        webkit_policy_decision_ignore(decision);
        const QByteArray target = FileAccessHandler::urlForLocalPath(
            url.toLocalFile()).toString().toUtf8();
        webkit_web_view_load_uri(m_webView, target.constData());
        return true;
    }

    // Approved main-frame navigations record their target — the
    // uri-scheme handlers match it to authenticate a no-Referer
    // request as the document nav (the arktos-file: SEC02 path).
    if (policy) {
        // Policy consulted and allowed — record the affirmative
        // decision rather than leaving WebKit's default.
        webkit_policy_decision_use(decision);
        if (mainFrame)
            m_lastNavTarget = url;
        return true;
    }
    if (mainFrame)
        m_lastNavTarget = url;
#else
    Q_UNUSED(decision);
    Q_UNUSED(type);
#endif
    return false;   // no policy installed — WebKit's default "use"
}

// ---- WK14 — WebKitFindController sinks -------------------------------
//
// found-text delivers the terminal "found" verdict; its match count
// can still be a partial figure (counting continues and lands on
// counted-matches), so a disagreeing counted-matches re-emits the
// corrected result — callers treat findTextFinished as a broadcast,
// the last emission wins.  failed-to-find-text is the terminal
// "miss".  WebKit exposes no active-match ordinal anywhere on the
// controller — FindResult::activeMatch is the locally tracked
// request-sequence ordinal (fresh forward find = 1, fresh backward =
// the count once known, each next/previous steps it and the real
// count wraps it).  Best-effort: scrolling mid-search can drift it;
// the count is authoritative.

void WebkitPage::onFindFound(unsigned matchCount)
{
    m_findCount = int(matchCount);
    if (m_findOrdinal == 0)
        m_findOrdinal = (m_findFlags & Engine::FindBackward)
            ? m_findCount : 1;
    else if (m_findCount > 0)
        m_findOrdinal = ((m_findOrdinal - 1) % m_findCount
                         + m_findCount) % m_findCount + 1;
    Engine::FindResult result;
    result.numberOfMatches = m_findCount;
    result.activeMatch = m_findOrdinal;
    Q_EMIT findTextFinished(result);
}

void WebkitPage::onFindCounted(unsigned matchCount)
{
    if (m_findText.isEmpty() || int(matchCount) == m_findCount)
        return;
    m_findCount = int(matchCount);
    if (m_findCount > 0 && m_findOrdinal > 0)
        m_findOrdinal = ((m_findOrdinal - 1) % m_findCount
                         + m_findCount) % m_findCount + 1;
    Engine::FindResult result;
    result.numberOfMatches = m_findCount;
    result.activeMatch = m_findOrdinal;
    Q_EMIT findTextFinished(result);
}

void WebkitPage::onFindFailed()
{
    m_findCount = 0;
    m_findOrdinal = 0;
    Engine::FindResult result;
    Q_EMIT findTextFinished(result);
}

// ---- WK19 — the user-facing surfaces --------------------------------
//
// Four surfaces land here: the certificate-error interstitial
// (WebPage's nonce contract, verbatim), the context menu (WebKit's
// own WebKitContextMenu item model driven through a Qt popup),
// WebKitPermissionRequest routed onto the SEC05 WebPermissionManager
// broker, and WebKitDownload wrapped as Engine::DownloadRequest —
// plus the capabilities/truth pass each one was verified against.

#ifdef ARKTOS_WEBKIT_LINKED

// GTlsCertificateFlags -> the symbolic name the interstitial's
// "Error: <description> (<name>)" line prints (the
// QWebEngineCertificateError::Type QMetaEnum-key role) and the
// human-readable description.  G_TLS_CERTIFICATE_* is a bitmask —
// every set flag contributes.
static QStringList tlsFlagEntries(unsigned flags)
{
    QStringList entries;
    if (flags & G_TLS_CERTIFICATE_UNKNOWN_CA)
        entries << QStringLiteral("UnknownCa|Certificate authority is unknown");
    if (flags & G_TLS_CERTIFICATE_BAD_IDENTITY)
        entries << QStringLiteral("BadIdentity|Certificate does not match the host");
    if (flags & G_TLS_CERTIFICATE_NOT_ACTIVATED)
        entries << QStringLiteral("NotActivated|Certificate is not yet valid");
    if (flags & G_TLS_CERTIFICATE_EXPIRED)
        entries << QStringLiteral("Expired|Certificate has expired");
    if (flags & G_TLS_CERTIFICATE_REVOKED)
        entries << QStringLiteral("Revoked|Certificate has been revoked");
    if (flags & G_TLS_CERTIFICATE_INSECURE)
        entries << QStringLiteral("Insecure|Certificate is insecure");
    if (flags & G_TLS_CERTIFICATE_GENERIC_ERROR)
        entries << QStringLiteral("GenericError|TLS validation failed");
    return entries;
}

static QString tlsFlagsName(unsigned flags)
{
    QStringList names;
    for (const QString &entry : tlsFlagEntries(flags))
        names << entry.section(QLatin1Char('|'), 0, 0);
    return names.isEmpty() ? QStringLiteral("Unknown")
                           : names.join(QLatin1Char('+'));
}

static QString tlsFlagsDescription(unsigned flags)
{
    QStringList phrases;
    for (const QString &entry : tlsFlagEntries(flags))
        phrases << entry.section(QLatin1Char('|'), 1, 1);
    return phrases.isEmpty()
        ? WebPage::tr("The certificate failed TLS validation")
        : phrases.join(QLatin1String("; "));
}

// The proceed path's session-scoped exception — WebKit's own
// allow_tls_certificate_for_host keeps the (host, certificate) pair
// for the session's life only (ephemeral sessions, OTR included,
// drop it with the session — the SEC06 rule the WebEngine whitelist
// serves).  The padlock-side record marks the host red so a
// proceeded-past chain never wears a green verdict.
static void allowCertificateForHost(WebKitNetworkSession *session,
                                    GTlsCertificate *certificate,
                                    const QString &host)
{
    if (!session || !certificate || host.isEmpty())
        return;
    webkit_network_session_allow_tls_certificate_for_host(
        session, certificate, host.toUtf8().constData());
    WebPage::noteCertErrorOverride(host);
}

#endif // ARKTOS_WEBKIT_LINKED

void WebkitPage::onTlsError(const QString &failingUri,
                            GTlsCertificate *certificate,
                            unsigned flags)
{
#ifdef ARKTOS_WEBKIT_LINKED
    const QUrl url = QUrl::fromEncoded(failingUri.toUtf8());

    // The neutral info WebPage's interstitial builder consumes:
    // chain DER-decoded into QSslCertificate, the flags bitmask
    // decoded to the symbolic name + description, overridable gated
    // on there being a certificate to except at all.
    Engine::CertificateErrorInfo info;
    info.url = url;
    info.error = int(flags);
    info.isMainFrame = true;
    info.overridable = certificate != nullptr;
    info.description = tlsFlagsDescription(flags);
    for (GTlsCertificate *cert = certificate; cert;
         cert = g_tls_certificate_get_issuer(cert)) {
        // The DER lives on the "certificate" GByteArray property —
        // there is no flat getter on glib's GTlsCertificate API.
        GByteArray *der = nullptr;
        g_object_get(cert, "certificate", &der, nullptr);
        if (der) {
            if (der->len) {
                const QByteArray bytes(
                    reinterpret_cast<const char *>(der->data),
                    int(der->len));
                QSslCertificate qcert(bytes, QSsl::Der);
                if (qcert.isNull())
                    qcert = QSslCertificate(bytes, QSsl::Pem);
                if (!qcert.isNull())
                    info.certificateChain << qcert;
            }
            g_byte_array_unref(der);
        }
    }

    // The WebPage contract, in order: the neutral signal first (a
    // listener that accepts installs the exception and reloads),
    // then the dead navigation's loadFinished(false), then the
    // nonce-bound interstitial for unanswered failures.
    bool accepted = false;
    Q_EMIT certificateError(info, &accepted);
    if (m_loading) {
        m_loading = false;
        Q_EMIT loadFinished(false);
    }
    if (accepted && info.overridable) {
        allowCertificateForHost(m_session, certificate, url.host());
        const QPointer<WebkitPage> page(this);
        QTimer::singleShot(0, this, [page, url]() {
            if (page)
                page->load(url);
        });
        return;
    }
    // A second error while a decision pends is refused without
    // another interstitial (WebPage's contract — the prompt would
    // otherwise stack).
    if (m_certErrorPending)
        return;
    m_certErrorPending = true;
    m_certErrorUrl = url;
    m_certErrorHost = url.host();
    m_certErrorOverridable = info.overridable;
    g_clear_object(&m_certErrorCert);
    if (certificate)
        m_certErrorCert =
            G_TLS_CERTIFICATE(g_object_ref(certificate));
    m_certErrorNonce =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    SchemeAccessHandler::publishInterstitialPage(
        m_certErrorNonce,
        WebPage::certificateErrorHtml(
            info, tlsFlagsName(flags), m_certErrorNonce));
    // Queued: navigating inside the load-failed-with-tls-errors
    // signal would re-enter WebKit's navigation machinery.
    const QUrl interstitial(QStringLiteral(
        "arktos-cert-error:interstitial?n=") + m_certErrorNonce);
    const QPointer<WebkitPage> page(this);
    QTimer::singleShot(0, this, [page, interstitial]() {
        if (page)
            page->load(interstitial);
    });
#else
    Q_UNUSED(failingUri);
    Q_UNUSED(certificate);
    Q_UNUSED(flags);
#endif
}

void WebkitPage::resolveCertificateErrorLink(const QUrl &command)
{
#ifdef ARKTOS_WEBKIT_LINKED
    const bool proceed = command.path() == QLatin1String("proceed")
        && m_certErrorOverridable && m_certErrorCert;
    m_certErrorPending = false;
    const QPointer<WebkitPage> page(this);
    if (proceed) {
        // Session-scoped only — nothing is persisted to a trust
        // store; a fresh session (or a different profile) prompts
        // again, the SEC06 rule.
        allowCertificateForHost(m_session, m_certErrorCert,
                                m_certErrorHost);
        const QUrl url = m_certErrorUrl;
        QTimer::singleShot(0, this, [page, url]() {
            if (page)
                page->load(url);
        });
        return;
    }
    // "back to safety" (or anything unrecognised): leave the
    // interstitial — back in history when there is one, otherwise
    // the start page.  Queued for the same re-entrancy reason as
    // WebPage's resolveCertificateErrorLink.
    QTimer::singleShot(0, this, [page]() {
        if (!page)
            return;
        if (page->canGoBack())
            page->back();
        else
            page->load(BrowserApplication::startPageUrl(
                BrowserApplication::defaultEngineId(),
                page->isOffTheRecord()));
    });
#else
    Q_UNUSED(command);
#endif
}

void WebkitPage::onContextMenu(WebKitContextMenu *menu,
                               WebKitHitTestResult *hitTestResult)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!menu || !hitTestResult)
        return;
    // Retain the menu for the view's popup — WebKit's ownership is
    // gesture-scoped and the popup reads it after the signal
    // returns.
    g_clear_object(&m_contextMenu);
    m_contextMenu = WEBKIT_CONTEXT_MENU(g_object_ref(menu));

    Engine::ContextMenuInfo info;
    info.pageUrl = m_url;
    if (m_view)
        info.position = m_view->m_contextMenuPos.toPoint();
    if (webkit_hit_test_result_context_is_link(hitTestResult)) {
        const char *uri =
            webkit_hit_test_result_get_link_uri(hitTestResult);
        info.linkUrl = QUrl::fromEncoded(uri ? uri : "");
        const char *label =
            webkit_hit_test_result_get_link_label(hitTestResult);
        const char *title =
            webkit_hit_test_result_get_link_title(hitTestResult);
        info.linkText = QString::fromUtf8(
            label && *label ? label : (title ? title : ""));
    }
    if (webkit_hit_test_result_context_is_image(hitTestResult)) {
        info.hasImage = true;
        const char *uri =
            webkit_hit_test_result_get_image_uri(hitTestResult);
        info.mediaUrl = QUrl::fromEncoded(uri ? uri : "");
    }
    if (webkit_hit_test_result_context_is_media(hitTestResult)) {
        info.hasMedia = true;
        const char *uri =
            webkit_hit_test_result_get_media_uri(hitTestResult);
        info.mediaUrl = QUrl::fromEncoded(uri ? uri : "");
    }
    info.isContentEditable =
        webkit_hit_test_result_context_is_editable(hitTestResult);
    // Honest gaps: the WPE hit-test exposes no selection text, no
    // canvas marker and no video/audio split — the ContextMenuInfo
    // fields there is no evidence for stay false/empty rather than
    // guessing.
    Q_EMIT contextMenuRequested(info);
    if (m_view)
        m_view->showContextMenu(info);
#else
    Q_UNUSED(menu);
    Q_UNUSED(hitTestResult);
#endif
}

#ifdef ARKTOS_WEBKIT_LINKED
// The pointer-lock request type's public header sits behind
// WebKit's own ENABLE_POINTER_LOCK build flag (webkit.h gates the
// include on a consumer macro nothing sets) — the symbol itself is
// exported on 2.54, so declare the type function directly.
extern "C" GType webkit_pointer_lock_permission_request_get_type(void);
#endif

void WebkitPage::onPermissionRequest(WebKitPermissionRequest *request)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!request)
        return;
    ++m_permissionSeen;
    QWebEnginePermission::PermissionType type;
    if (G_TYPE_CHECK_INSTANCE_TYPE(request,
            WEBKIT_TYPE_GEOLOCATION_PERMISSION_REQUEST))
        type = QWebEnginePermission::PermissionType::Geolocation;
    else if (G_TYPE_CHECK_INSTANCE_TYPE(request,
            WEBKIT_TYPE_NOTIFICATION_PERMISSION_REQUEST))
        type = QWebEnginePermission::PermissionType::Notifications;
    else if (G_TYPE_CHECK_INSTANCE_TYPE(request,
            webkit_pointer_lock_permission_request_get_type()))
        type = QWebEnginePermission::PermissionType::MouseLock;
    else if (G_TYPE_CHECK_INSTANCE_TYPE(request,
            WEBKIT_TYPE_USER_MEDIA_PERMISSION_REQUEST)
             || G_TYPE_CHECK_INSTANCE_TYPE(request,
            WEBKIT_TYPE_DEVICE_INFO_PERMISSION_REQUEST))
        // Camera/mic enumerate + capture — the SEC05 deny-list's
        // surface; the broker answers false without prompting.
        type = QWebEnginePermission::PermissionType::MediaAudioVideoCapture;
    else {
        // Media-key-system (EME), website-data-access, XR,
        // install-missing-plugins and anything new — no SEC05
        // mapping exists, so the request denies outright rather
        // than hanging the page.
        webkit_permission_request_deny(request);
        return;
    }
    g_object_ref(request);
    QPointer<WebkitPage> page(this);
    WebPermissionManager::instance()->handleRequest(
        m_view, m_url, type, isOffTheRecord(),
        [page, request](bool granted) {
            // The page may have died while the prompt queued — the
            // request ref keeps the object valid and allow/deny on a
            // dead view's request is a no-op.
            if (page && granted)
                ++page->m_permissionGranted;
            if (granted)
                webkit_permission_request_allow(request);
            else
                webkit_permission_request_deny(request);
            g_object_unref(request);
        });
#else
    Q_UNUSED(request);
#endif
}

// ---- WebkitView — the context-menu presentation ---------------------

#ifdef ARKTOS_WEBKIT_LINKED

// WebKitContextMenuAction -> the menu label.  WebKit's own gettext
// catalog lives behind GTK internals WPE doesn't expose, so the
// stock actions carry Qt-side translations matching the app's menu
// voice; a CUSTOM action falls back to its GAction name.
static QString webkitStockActionLabel(WebKitContextMenuAction action)
{
    switch (action) {
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK:
        return WebPage::tr("Open Link");
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW:
        return WebPage::tr("Open Link in New Window");
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_LINK_TO_DISK:
        return WebPage::tr("Download Linked File");
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD:
        return WebPage::tr("Copy Link");
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW:
        return WebPage::tr("Open Image in New Window");
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK:
        return WebPage::tr("Save Image");
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD:
        return WebPage::tr("Copy Image");
    case WEBKIT_CONTEXT_MENU_ACTION_GO_BACK:
        return WebPage::tr("Back");
    case WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD:
        return WebPage::tr("Forward");
    case WEBKIT_CONTEXT_MENU_ACTION_STOP:
        return WebPage::tr("Stop");
    case WEBKIT_CONTEXT_MENU_ACTION_RELOAD:
        return WebPage::tr("Reload");
    case WEBKIT_CONTEXT_MENU_ACTION_COPY:
        return WebPage::tr("Copy");
    case WEBKIT_CONTEXT_MENU_ACTION_CUT:
        return WebPage::tr("Cut");
    case WEBKIT_CONTEXT_MENU_ACTION_PASTE:
        return WebPage::tr("Paste");
    case WEBKIT_CONTEXT_MENU_ACTION_BOLD:
        return WebPage::tr("Bold");
    case WEBKIT_CONTEXT_MENU_ACTION_ITALIC:
        return WebPage::tr("Italic");
    case WEBKIT_CONTEXT_MENU_ACTION_UNDERLINE:
        return WebPage::tr("Underline");
    case WEBKIT_CONTEXT_MENU_ACTION_INSPECT_ELEMENT:
        return WebPage::tr("Inspect Element");
    case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PLAY:
        return WebPage::tr("Play");
    case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PAUSE:
        return WebPage::tr("Pause");
    case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_MUTE:
        return WebPage::tr("Mute");
    case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_CONTROLS:
        return WebPage::tr("Toggle Media Controls");
    case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_LOOP:
        return WebPage::tr("Toggle Media Loop");
    case WEBKIT_CONTEXT_MENU_ACTION_ENTER_VIDEO_FULLSCREEN:
        return WebPage::tr("Enter Video Fullscreen");
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_VIDEO_LINK_TO_CLIPBOARD:
        return WebPage::tr("Copy Video Link");
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_AUDIO_LINK_TO_CLIPBOARD:
        return WebPage::tr("Copy Audio Link");
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_VIDEO_IN_NEW_WINDOW:
        return WebPage::tr("Open Video in New Window");
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_AUDIO_IN_NEW_WINDOW:
        return WebPage::tr("Open Audio in New Window");
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_VIDEO_TO_DISK:
        return WebPage::tr("Save Video");
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_AUDIO_TO_DISK:
        return WebPage::tr("Save Audio");
    case WEBKIT_CONTEXT_MENU_ACTION_NO_GUESSES_FOUND:
        return WebPage::tr("No Guesses Found");
    case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_SPELLING:
        return WebPage::tr("Ignore Spelling");
    case WEBKIT_CONTEXT_MENU_ACTION_LEARN_SPELLING:
        return WebPage::tr("Learn Spelling");
    case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_GRAMMAR:
        return WebPage::tr("Ignore Grammar");
    case WEBKIT_CONTEXT_MENU_ACTION_FONT_MENU:
        return WebPage::tr("Font");
    default:
        break;
    }
    return QString();
}

// WebKit owns the item semantics — the popup only names them and
// activates their GActions on trigger.  Item labels: the item's
// title (submenu headers), else the stock-action table, else the
// GAction name (custom items).
static void populateWebkitMenu(QMenu *popup, WebKitContextMenu *menu)
{
    for (GList *entry = webkit_context_menu_get_items(menu);
         entry; entry = entry->next) {
        WebKitContextMenuItem *item =
            WEBKIT_CONTEXT_MENU_ITEM(entry->data);
        if (webkit_context_menu_item_is_separator(item)) {
            popup->addSeparator();
            continue;
        }
        const char *title = webkit_context_menu_item_get_title(item);
        if (WebKitContextMenu *sub =
                webkit_context_menu_item_get_submenu(item)) {
            QMenu *submenu = popup->addMenu(
                QString::fromUtf8(title ? title : ""));
            populateWebkitMenu(submenu, sub);
            continue;
        }
        QString label = QString::fromUtf8(title ? title : "");
        if (label.isEmpty())
            label = webkitStockActionLabel(
                webkit_context_menu_item_get_stock_action(item));
        GAction *action = webkit_context_menu_item_get_gaction(item);
        if (label.isEmpty() && action)
            label = QString::fromUtf8(g_action_get_name(action));
        if (label.isEmpty() || !action)
            continue;   // no name, no verb — an item we can't present
        g_object_ref(action);
        GVariant *target =
            webkit_context_menu_item_get_gaction_target(item);
        if (target)
            g_variant_ref(target);
        QAction *entryAction = popup->addAction(label, [action, target]() {
            g_action_activate(action, target);
        });
        // The GAction/GVariant refs ride the QAction's lifetime — the
        // popup may outlive the WebKitContextMenu that produced it.
        QObject::connect(entryAction, &QObject::destroyed,
                popup, [action, target]() {
            if (target)
                g_variant_unref(target);
            g_object_unref(action);
        });
    }
}

#endif // ARKTOS_WEBKIT_LINKED

void WebkitView::showContextMenu(const Engine::ContextMenuInfo &info)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_page || !m_page->m_contextMenu)
        return;
    // Heap + WA_DeleteOnClose — the handler runs inside the glib
    // context-menu signal, so exec() (a nested modal loop) is out;
    // popup() presents and the menu deletes itself on close.
    QMenu *popup = new QMenu(this);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    populateWebkitMenu(popup, m_page->m_contextMenu);
    if (popup->isEmpty()) {
        delete popup;
        return;
    }
    popup->popup(mapToGlobal(info.position));
#else
    Q_UNUSED(info);
#endif
}

// ---- WK19 — WebkitDownloadRequest ------------------------------------
//
// The class is in the TU for inert builds too (the file's own
// compile-but-inert contract) — every body guards on
// ARKTOS_WEBKIT_LINKED, so an unlinked TU still emits the vtable.

WebkitDownloadRequest::WebkitDownloadRequest(WebKitDownload *download,
        WebkitPage *page, QObject *parent)
    : Engine::DownloadRequest(parent)
    , m_page(page)
{
#ifdef ARKTOS_WEBKIT_LINKED
    m_download = WEBKIT_DOWNLOAD(g_object_ref(download));
    g_signal_connect(m_download, "decide-destination",
                     G_CALLBACK(cbDecideDestination), this);
    g_signal_connect(m_download, "received-data",
                     G_CALLBACK(cbReceivedData), this);
    g_signal_connect(m_download, "finished",
                     G_CALLBACK(cbFinished), this);
    g_signal_connect(m_download, "failed",
                     G_CALLBACK(cbFailed), this);
#else
    Q_UNUSED(download);
    Q_UNUSED(page);
#endif
}

WebkitDownloadRequest::~WebkitDownloadRequest()
{
#ifdef ARKTOS_WEBKIT_LINKED
    g_clear_object(&m_download);
#endif
}

QUrl WebkitDownloadRequest::url() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    WebKitURIRequest *request = m_download
        ? webkit_download_get_request(m_download) : nullptr;
    const char *uri = request
        ? webkit_uri_request_get_uri(request) : nullptr;
    return QUrl::fromEncoded(uri ? uri : "");
#else
    return QUrl();
#endif
}

QString WebkitDownloadRequest::suggestedFileName() const
{
    return m_suggested;
}

QString WebkitDownloadRequest::mimeType() const
{
    return m_mime;
}

void WebkitDownloadRequest::accept(const QString &filePath)
{
    if (!filePath.isEmpty()) {
        const QFileInfo info(filePath);
        m_directory = info.absolutePath();
        m_fileName = info.fileName();
    }
    m_decided = true;
}

void WebkitDownloadRequest::cancel()
{
    m_cancelled = true;
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_download)
        webkit_download_cancel(m_download);
#endif
}

void WebkitDownloadRequest::pause()
{
    // WebKitDownload has no pause primitive — an honest no-op; the
    // sidebar shows state instead of a fake toggle.
}

void WebkitDownloadRequest::resume()
{
}

bool WebkitDownloadRequest::isPaused() const
{
    return false;
}

qint64 WebkitDownloadRequest::receivedBytes() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    return m_download
        ? qint64(webkit_download_get_received_data_length(m_download))
        : 0;
#else
    return 0;
#endif
}

qint64 WebkitDownloadRequest::totalBytes() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    WebKitURIResponse *response = m_download
        ? webkit_download_get_response(m_download) : nullptr;
    return response
        ? qint64(webkit_uri_response_get_content_length(response))
        : -1;
#else
    return -1;
#endif
}

Engine::DownloadRequest::State WebkitDownloadRequest::state() const
{
    return m_state;
}

bool WebkitDownloadRequest::isFinished() const
{
    return m_state == State::Completed;
}

QString WebkitDownloadRequest::interruptReasonString() const
{
    return m_interrupt;
}

void WebkitDownloadRequest::setDownloadDirectory(const QString &directory)
{
    m_directory = directory;
}

void WebkitDownloadRequest::setDownloadFileName(const QString &fileName)
{
    m_fileName = fileName;
}

QString WebkitDownloadRequest::downloadDirectory() const
{
    return m_directory;
}

QString WebkitDownloadRequest::downloadFileName() const
{
    return m_fileName;
}

Engine::Page *WebkitDownloadRequest::page() const
{
    return m_page;
}

void WebkitDownloadRequest::setState(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    Q_EMIT stateChanged(state);
}

int WebkitDownloadRequest::onDecideDestination(
        const char *suggestedFileName)
{
#ifdef ARKTOS_WEBKIT_LINKED
    m_suggested = QString::fromUtf8(
        suggestedFileName ? suggestedFileName : "");
    WebKitURIResponse *response = m_download
        ? webkit_download_get_response(m_download) : nullptr;
    if (response) {
        const char *mime = webkit_uri_response_get_mime_type(response);
        if (mime)
            m_mime = QString::fromUtf8(mime);
        Q_EMIT totalBytesChanged();
    }

    // The handoff — every DownloadItem path resolves synchronously
    // inside the emission (accept/cancel), so by the time it returns
    // m_decided/m_cancelled carry the answer.
    Engine::Profile *profile = m_page ? m_page->profile() : nullptr;
    if (profile)
        Q_EMIT profile->downloadRequested(this);
    if (!m_decided || m_cancelled) {
        // No consumer, a declined prompt, or a rustdl/external
        // handoff — never let WebKit fall back to a silent
        // default-directory write: cancel the transfer ourselves and
        // claim the decision.
        m_cancelled = true;
        webkit_download_cancel(m_download);
        setState(State::Cancelled);
        return TRUE;
    }
    QString name = m_fileName.isEmpty() ? m_suggested : m_fileName;
    if (name.isEmpty())
        name = QStringLiteral("download");
    QString dir = m_directory;
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(
            QStandardPaths::DownloadLocation);
    const QString path = QDir(dir).absoluteFilePath(name);
    // The destination is a filesystem path, not a file:// URI —
    // webkit_download_set_destination asserts g_path_is_absolute.
    webkit_download_set_destination(
        m_download, QFile::encodeName(path).constData());
    setState(State::InProgress);
    return TRUE;
#else
    Q_UNUSED(suggestedFileName);
    return 0;
#endif
}

void WebkitDownloadRequest::onReceivedData()
{
    Q_EMIT receivedBytesChanged();
}

void WebkitDownloadRequest::onFinished()
{
    setState(State::Completed);
    deleteLater();
}

void WebkitDownloadRequest::onFailed(const QString &message,
                                     bool cancelledByUser)
{
    m_interrupt = message;
    setState(cancelledByUser ? State::Cancelled : State::Interrupted);
    deleteLater();
}

int WebkitDownloadRequest::cbDecideDestination(
        WebKitDownload *, void *suggested,
        WebkitDownloadRequest *self)
{
    return self->onDecideDestination(
        suggested ? static_cast<const char *>(suggested) : nullptr);
}

void WebkitDownloadRequest::cbReceivedData(WebKitDownload *,
                                           quint64,
                                           WebkitDownloadRequest *self)
{
    self->onReceivedData();
}

void WebkitDownloadRequest::cbFinished(WebKitDownload *,
                                       WebkitDownloadRequest *self)
{
    self->onFinished();
}

void WebkitDownloadRequest::cbFailed(WebKitDownload *, void *error,
                                     WebkitDownloadRequest *self)
{
#ifdef ARKTOS_WEBKIT_LINKED
    const GError *gerror = static_cast<const GError *>(error);
    const bool cancelled = gerror
        && g_error_matches(gerror, WEBKIT_DOWNLOAD_ERROR,
                           WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER);
    self->onFailed(QString::fromUtf8(
        gerror ? gerror->message : ""), cancelled);
#else
    Q_UNUSED(error);
    self->onFailed(QString(), true);
#endif
}

#ifdef ARKTOS_WEBKIT_LINKED

// The "download-started" session signal — resolve the owning page
// (the download's originating web view) and hand the transfer to
// its profile.  A download no page owns is refused outright — the
// only producers are web_view_download_uri and the decide-policy
// download verdict, both page-bound, so an unattributed transfer is
// an anomaly that must not write to disk silently.
static void cbDownloadStarted(WebKitNetworkSession *,
                              WebKitDownload *download, void *)
{
    if (!download)
        return;
    WebKitWebView *view = webkit_download_get_web_view(download);
    WebkitPage *page = view ? schemePageMap().value(view) : nullptr;
    Engine::Profile *profile = page ? page->profile() : nullptr;
    if (!profile) {
        webkit_download_cancel(download);
        return;
    }
    new WebkitDownloadRequest(download, page, profile);
}

void ensureDownloadHook(WebKitNetworkSession *session)
{
    static QSet<WebKitNetworkSession *> hooked;
    if (!session || hooked.contains(session))
        return;
    hooked.insert(session);
    g_signal_connect(session, "download-started",
                     G_CALLBACK(cbDownloadStarted), nullptr);
}

#endif // ARKTOS_WEBKIT_LINKED

// ---- WebkitProfile --------------------------------------------------

#ifdef ARKTOS_WEBKIT_LINKED
// WK11 — a foreign (non-persona) storageName still needs its own
// directory leaf: reduce it to the profile-slug charset (isProfileId
// allows [a-z0-9_-] only — '.' is deliberately excluded so a ".." or
// dotfile name can never carry the leaf outside the WebKit tree).
static QString webkitStorageLeaf(const QString &storageName)
{
    QString leaf;
    leaf.reserve(qMin(storageName.size(), 64));
    for (const QChar c : storageName) {
        if (leaf.size() >= 64)
            break;
        const QChar lower = c.toLower();
        leaf += (lower.isLetterOrNumber() && lower.unicode() < 128)
                || lower == QLatin1Char('-')
                || lower == QLatin1Char('_')
            ? lower : QLatin1Char('-');
    }
    return leaf.isEmpty() ? QStringLiteral("store") : leaf;
}

// WK11 — the persona id a storageName encodes, mirroring the parse
// BrowserProfile::profileIdFor applies to QtWebEngine storage names:
// "arktos" is the default persona, "arktos-<slug>" is persona <slug>;
// "arktos-container-*" is deliberately NOT a persona (the container's
// owner rides ProfileOptions::dataPath) and neither is any other
// foreign name — those stay on the ambient persona's tree.
static QString webkitPersonaId(const QString &storageName)
{
    if (storageName == QLatin1String("arktos"))
        return QStringLiteral("default");
    if (storageName.startsWith(QLatin1String("arktos-"))
            && !storageName.startsWith(
                   QLatin1String("arktos-container-"))) {
        const QString id = storageName.mid(7);
        if (BrowserPaths::isProfileId(id))
            return id;
    }
    return QString();
}

// ---- WK17 — custom-scheme serving ----------------------------------
//
// webkit_web_context_register_uri_scheme is the WPE half of the
// custom-scheme contract — every URISchemeRequest callback lands on
// the global-default main context the Qt GUI thread already pumps, so
// the serving adapters may do GUI work inline (QFileIconProvider
// pixmaps, the abp: subscribe prompt) where backend-0's handlers had
// to queue off the IO thread.  The serving LOGIC is the shared
// engine-agnostic half of the same adapters backend-0 uses —
// FileAccessHandler's listing builder + SEC02 allowlist, the
// interstitial nonce registry, the abp: subscribe shape, the
// compiled stub table — so a second engine refuses and serves
// identically rather than carrying a divergent copy.

// web_view -> owning WebkitPage: scheme requests resolve the
// originating page for the persona lookup (abp:) and the initiator
// fallback (arktos-file: SEC02).  Pages register at construction and
// remove at destruction; a request whose view vanished still gets
// answered (ambient persona / no initiator).
static QHash<WebKitWebView *, WebkitPage *> &schemePageMap()
{
    static QHash<WebKitWebView *, WebkitPage *> map;
    return map;
}

static void finishSchemeBytes(WebKitURISchemeRequest *request,
                              const QByteArray &body,
                              const QByteArray &mime)
{
    GInputStream *stream = g_memory_input_stream_new_from_data(
        g_memdup2(body.constData(), gsize(body.size())),
        gsize(body.size()), g_free);
    webkit_uri_scheme_request_finish(
        request, stream, gint64(body.size()),
        mime.isEmpty() ? "application/octet-stream" : mime.constData());
    g_object_unref(stream);
}

static void failSchemeRequest(WebKitURISchemeRequest *request, int code,
                              const char *message)
{
    GError *error = g_error_new_literal(WEBKIT_NETWORK_ERROR, code,
                                        message);
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
}

// arktos-file: — the FileAccessHandler contract: GET only, SEC02
// initiator rules, directory listings only.  WebKit's scheme request
// carries no initiator origin — the Referer header is the closest
// provenance (a remote page reaching for the listing — iframe,
// subresource, or a poked navigation — always leaves one).  With no
// Referer the request is only legitimate as the main-frame
// navigation decide-policy just approved on the same view; anything
// else is refused.
static void cbSchemeArktosFile(WebKitURISchemeRequest *request, gpointer)
{
    const char *method =
        webkit_uri_scheme_request_get_http_method(request);
    if (!method || QByteArray(method) != "GET") {
        failSchemeRequest(request, WEBKIT_NETWORK_ERROR_FAILED,
                          "invalid method");
        return;
    }
    const QUrl url(QString::fromUtf8(
        webkit_uri_scheme_request_get_uri(request)));
    QString initiatorScheme;
    if (SoupMessageHeaders *headers =
            webkit_uri_scheme_request_get_http_headers(request)) {
        const char *referer =
            soup_message_headers_get_one(headers, "Referer");
        if (referer)
            initiatorScheme = QUrl(QString::fromUtf8(referer)).scheme();
    }
    bool allowed = !initiatorScheme.isEmpty()
        && FileAccessHandler::initiatorAllowed(initiatorScheme);
    if (!allowed && initiatorScheme.isEmpty()) {
        WebkitPage *page = schemePageMap().value(
            webkit_uri_scheme_request_get_web_view(request));
        allowed = page && page->lastNavTarget() == url;
    }
    if (!allowed) {
        failSchemeRequest(request, WEBKIT_NETWORK_ERROR_FAILED,
                          "initiator denied");
        return;
    }
    int iconSize = qApp->font().pixelSize();
    if (iconSize <= 0)
        iconSize = 16;
    QByteArray html;
    const FileAccessHandler::ListingError error =
        FileAccessHandler::directoryListing(url.path(), iconSize,
                                            &html);
    if (error == FileAccessHandler::ListingError::None) {
        finishSchemeBytes(request, html, "text/html");
        return;
    }
    failSchemeRequest(
        request,
        error == FileAccessHandler::ListingError::NotFound
            ? WEBKIT_NETWORK_ERROR_FILE_DOES_NOT_EXIST
            : WEBKIT_NETWORK_ERROR_FAILED,
        "directory listing failed");
}

// arktos-resource: — the compiled $redirect stub table
// (AdBlockResourceHandler::resourceFor is already engine-agnostic).
static void cbSchemeArktosResource(WebKitURISchemeRequest *request,
                                   gpointer)
{
    const char *method =
        webkit_uri_scheme_request_get_http_method(request);
    const QUrl url(QString::fromUtf8(
        webkit_uri_scheme_request_get_uri(request)));
    QString path = url.path();
    if (path.startsWith(QLatin1Char('/')))
        path = path.mid(1);
    QByteArray mime, body;
    if (!method || QByteArray(method) != "GET"
        || !AdBlockResourceHandler::resourceFor(path.toUtf8(), &mime,
                                              &body)) {
        failSchemeRequest(request, WEBKIT_NETWORK_ERROR_FAILED,
                          "unknown stub resource");
        return;
    }
    finishSchemeBytes(request, body, mime);
}

// abp: — the subscribe endpoint: the URL only carries parameters; the
// persona's own AdBlockManager answers the prompt (the MPROF rule —
// an ambient instance() would attribute the subscription to the
// wrong profile).  Nothing is ever served.
static void cbSchemeAbp(WebKitURISchemeRequest *request, gpointer)
{
    const char *method =
        webkit_uri_scheme_request_get_http_method(request);
    const QUrl url(QString::fromUtf8(
        webkit_uri_scheme_request_get_uri(request)));
    if (!method || QByteArray(method) != "GET"
        || !AdBlockSchemeAccessHandler::isSubscribeRequest(url)) {
        failSchemeRequest(request, WEBKIT_NETWORK_ERROR_FAILED,
                          "invalid abp: request");
        return;
    }
    AdBlockManager *manager = nullptr;
    if (WebkitPage *page = schemePageMap().value(
            webkit_uri_scheme_request_get_web_view(request))) {
        QString personaId;
        if (page->profile())
            personaId =
                webkitPersonaId(page->profile()->storageName());
        if (personaId.isEmpty())
            personaId = BrowserPaths::activeProfileId();
        manager = AdBlockManager::instanceFor(personaId);
    }
    if (manager)
        manager->promptSubscribe(url);
    failSchemeRequest(request, WEBKIT_NETWORK_ERROR_CANCELLED,
                      "abp: requests are never served");
}

// arktos-cert-error: / arktos-http-warning: / arktos-site-block: —
// the nonce registry IS already the engine-agnostic store
// (SchemeAccessHandler statics); only the serving transport differs.
static void cbSchemeInterstitial(WebKitURISchemeRequest *request,
                                 gpointer)
{
    const QUrl url(QString::fromUtf8(
        webkit_uri_scheme_request_get_uri(request)));
    QString html;
    if (url.path() == QLatin1String("interstitial")) {
        QMutexLocker lock(&SchemeAccessHandler::interstitialMutex());
        html = SchemeAccessHandler::interstitialPages().value(
            QUrlQuery(url).queryItemValue(QLatin1String("n")));
    }
    if (html.isEmpty()) {
        failSchemeRequest(request, WEBKIT_NETWORK_ERROR_FAILED,
                          "no interstitial for nonce");
        return;
    }
    finishSchemeBytes(request, html.toUtf8(), "text/html");
}

static void installUriSchemeHandlers(WebKitWebContext *context)
{
    webkit_web_context_register_uri_scheme(
        context, "arktos-file", cbSchemeArktosFile, nullptr, nullptr);
    webkit_web_context_register_uri_scheme(
        context, "abp", cbSchemeAbp, nullptr, nullptr);
    webkit_web_context_register_uri_scheme(
        context, "arktos-resource", cbSchemeArktosResource,
        nullptr, nullptr);
    webkit_web_context_register_uri_scheme(
        context, "arktos-cert-error", cbSchemeInterstitial,
        nullptr, nullptr);
    webkit_web_context_register_uri_scheme(
        context, "arktos-http-warning", cbSchemeInterstitial,
        nullptr, nullptr);
    webkit_web_context_register_uri_scheme(
        context, "arktos-site-block", cbSchemeInterstitial,
        nullptr, nullptr);

    // Security flags mirroring backend-0's QWebEngineUrlScheme
    // registrations: arktos-file is Local (its file:// entry links
    // must resolve); the stub resources are Secure+CorsEnabled so an
    // https page accepts a $redirect serving; the interstitial
    // carriers are Secure like backend-0.
    WebKitSecurityManager *security =
        webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_local(
        security, "arktos-file");
    webkit_security_manager_register_uri_scheme_as_secure(
        security, "arktos-resource");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(
        security, "arktos-resource");
    webkit_security_manager_register_uri_scheme_as_secure(
        security, "arktos-cert-error");
    webkit_security_manager_register_uri_scheme_as_secure(
        security, "arktos-http-warning");
    webkit_security_manager_register_uri_scheme_as_secure(
        security, "arktos-site-block");
}
#endif // ARKTOS_WEBKIT_LINKED

WebkitProfile::WebkitProfile(const Engine::ProfileOptions &options,
                             QObject *parent)
    : Engine::Profile(parent)
    , m_options(options)
{
}

WebkitProfile::~WebkitProfile()
{
#ifdef ARKTOS_WEBKIT_LINKED
    // WK18: the retained filter is a boxed type — unref, not
    // g_clear_object.
    if (m_filter)
        webkit_user_content_filter_unref(m_filter);
    g_clear_object(&m_networkSession);
#endif
}

WebKitNetworkSession *WebkitProfile::networkSession()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_networkSession) {
        if (m_options.offTheRecord) {
            // WK11: real ephemeral session — the data manager owns
            // no base directories, every store is memory-only and
            // dies with the session.
            m_networkSession = webkit_network_session_new_ephemeral();
        } else if (m_options.storageName.isEmpty()
                   && m_options.dataPath.isEmpty()
                   && m_options.cachePath.isEmpty()) {
            // No storage identity at all — the engine-default shared
            // session (the WK08 minimal contract an engine-swap tab
            // still gets).
            m_networkSession =
                g_object_ref(webkit_network_session_get_default());
        } else {
            // WK11: a named/path-rooted session gets a private
            // WebKitWebsiteDataManager under the profile's storage
            // tree.  Root resolution:
            //   dataPath set   -> <dataPath>/WebKit  (caller-owned
            //                     root — the container contract —
            //                     the WebKit leaf keeps it disjoint
            //                     from a QtWebEngine store at the
            //                     same root)
            //   persona name   -> profiles/<id>/WebKit
            //                     ("arktos"->default, "arktos-<id>"->
            //                     <id> — the VSEC02 sibling of
            //                     profiles/<id>/QtWebEngine)
            //   foreign name   -> profiles/<active>/WebKit/<slug>
            // The cache mirrors the data root: cachePath when given,
            // else the root's own "cache" subdir — the VSEC02 parity
            // is profiles/<id>/cache/WebKit (the vault's "cache"
            // exclusion).
            const QString name = m_options.storageName;
            QString personaId = webkitPersonaId(name);
            QString leaf;
            if (personaId.isEmpty()) {
                personaId = BrowserPaths::activeProfileId();
                if (!name.isEmpty() && m_options.dataPath.isEmpty())
                    leaf = webkitStorageLeaf(name);
            }
            const QString dataRoot = !m_options.dataPath.isEmpty()
                ? m_options.dataPath
                : BrowserPaths::profileDataDir(personaId);
            const QString cacheRoot = !m_options.cachePath.isEmpty()
                ? m_options.cachePath
                : dataRoot + QLatin1String("/cache");
            QString dataDir = dataRoot + QLatin1String("/WebKit");
            QString cacheDir = cacheRoot + QLatin1String("/WebKit");
            if (!leaf.isEmpty()) {
                dataDir += QLatin1Char('/') + leaf;
                cacheDir += QLatin1Char('/') + leaf;
            }
            QDir().mkpath(dataDir);
            QDir().mkpath(cacheDir);
            m_sessionDataDir = dataDir;
            m_sessionCacheDir = cacheDir;
            m_networkSession = webkit_network_session_new(
                QFile::encodeName(dataDir).constData(),
                QFile::encodeName(cacheDir).constData());
            if (!m_networkSession) {
                // webkit_network_session_new can refuse (e.g. a dir
                // that exists as a file); a null session would fall
                // back to default in the page ctor and silently share
                // the global store — fail loud instead.
                qWarning("webkit: network session creation failed for"
                         " data dir %s — profile stays sessionless",
                         qPrintable(dataDir));
                m_sessionDataDir.clear();
                m_sessionCacheDir.clear();
            }
        }
    }
    // WK12: the persona's cookie policy rides on the session's cookie
    // manager — applied once on materialization.
    if (m_networkSession && !m_cookiePolicyApplied) {
        m_cookiePolicyApplied = true;
        applyCookiePolicy();
    }
#endif
    return m_networkSession;
}

void WebkitProfile::applyCookiePolicy()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!m_networkSession)
        return;
    WebKitCookieManager *manager =
        webkit_network_session_get_cookie_manager(m_networkSession);
    if (!manager)
        return;

    // The persona whose policy this session answers to: a persona
    // storageName resolves its own id; path-rooted containers and
    // foreign names ride the ambient persona (the owning-window
    // contract — container cookies share the owner persona's rules
    // on the WebEngine side too); an OTR profile applies the ambient
    // persona's policy in memory only.
    QString personaId = webkitPersonaId(m_options.storageName);
    if (personaId.isEmpty())
        personaId = BrowserPaths::activeProfileId();

    // The persisted values are CookieJar's QMetaEnum keys — the same
    // strings cookiejar.cpp's loadSettings reads, with its defaults.
    QByteArray accept = "AcceptOnlyFromSitesNavigatedTo";
    QByteArray keep = "KeepUntilExpire";
    bool blockThirdParty = true;
    // ProfileSettings(profileId) mkpaths profiles/<id>/ on
    // construction (ProfilePaths::dirFor) — only touch it when the
    // persona already has a store, so merely reading the cookie
    // policy can never mint a persona dir (an OTR profile creates
    // nothing).
    if (QFileInfo::exists(BrowserPaths::dataRoot()
            + QLatin1String("/profiles/") + personaId)) {
        ProfileSettings settings(personaId);
        settings.beginGroup(QStringLiteral("cookies"));
        accept = settings.value(
            QLatin1String("acceptCookies"),
            QLatin1String("AcceptOnlyFromSitesNavigatedTo")).toByteArray();
        keep = settings.value(
            QLatin1String("keepCookiesUntil"),
            QLatin1String("KeepUntilExpire")).toByteArray();
        blockThirdParty = settings.value(
            QLatin1String("blockThirdPartyCookies"), true).toBool();
    }

    // AcceptPolicy -> WebKitCookieAcceptPolicy.  WebKit's three-state
    // model carries all three app policies: NEVER for AcceptNever,
    // ALWAYS for AcceptAlways — dropped to NO_THIRD_PARTY when the
    // PRIV01 blockThirdPartyCookies toggle is on — and
    // NO_THIRD_PARTY (first-party only) for the default
    // AcceptOnlyFromSitesNavigatedTo.
    WebKitCookieAcceptPolicy policy =
        WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
    if (accept == "AcceptNever")
        policy = WEBKIT_COOKIE_POLICY_ACCEPT_NEVER;
    else if (accept == "AcceptAlways" && !blockThirdParty)
        policy = WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;
    m_cookieAcceptPolicy = int(policy);
    webkit_cookie_manager_set_accept_policy(manager, policy);

    // KeepPolicy -> set_persistent_storage.  WebKit holds cookies in
    // memory only until given a file — KeepUntilExit maps to exactly
    // that (never called, the session store dies with the session);
    // the other keep policies get cookies.sqlite under the named
    // session's data root.  Honest gap: KeepUntilTimeLimit's 90-day
    // expiry cap is a CookieJar-side cookieAdded rewrite with no
    // WebKitCookieManager surface — it persists to natural expiry
    // like KeepUntilExpire.  The shared default session keeps its
    // own storage — we never point it at a per-persona file.
    if (!m_sessionDataDir.isEmpty() && keep != "KeepUntilExit") {
        m_cookieStoragePath =
            m_sessionDataDir + QLatin1String("/cookies.sqlite");
        webkit_cookie_manager_set_persistent_storage(
            manager,
            QFile::encodeName(m_cookieStoragePath).constData(),
            WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }
#endif
}

bool WebkitProfile::sessionIsEphemeral() const
{
#ifdef ARKTOS_WEBKIT_LINKED
    return m_networkSession
        && webkit_network_session_is_ephemeral(m_networkSession);
#else
    return false;
#endif
}

bool WebkitProfile::isOffTheRecord() const
{
    return m_options.offTheRecord;
}

QString WebkitProfile::storageName() const
{
    return m_options.storageName;
}

void WebkitProfile::setUserAgent(const QString &userAgent)
{
    // No UA override hook on this backend — accepted and ignored.
    Q_UNUSED(userAgent);
}

void WebkitProfile::setRequestPolicy(Engine::RequestPolicy *policy)
{
    m_policy = policy;
    // WK18: the declarative lane pulls its rules document at install
    // time.  The app's adblock wiring re-invokes this setter on every
    // rulesChanged (installOnProfile), which is the refresh hook —
    // a repeat pull that produces the same document short-circuits
    // on the content hash before it ever reaches the compiler.
    syncContentBlocker();
}

Engine::RequestPolicy *WebkitProfile::requestPolicy() const
{
    return m_policy;
}

// ---- WK18: declarative content blocking --------------------------
//
// WebKit has no per-request subresource veto — decide-policy (WK17)
// only sees navigations.  The coverage answer is the Safari
// content-blocker lane: the profile pulls the policy's compiled
// rules document (RequestPolicy::contentBlockerJson — the
// AdBlockEnginePolicy override renders the persona's lists through
// the WKB01 Rust converter), extracts the "rules" array for the
// store, and keeps the "dropped"/"meta" channels as the counted +
// logged honesty trail.  Main-frame navigations are deliberately
// NOT the filter's job — the converter's trailing
// ignore-previous-rules fp-document guard approximates ABP's
// never-block-the-document rule, and live vetoes stay with
// decide-policy.

void WebkitProfile::cbContentFilterLoad(void *object, void *result,
                                        void *data)
{
#ifdef ARKTOS_WEBKIT_LINKED
    ContentFilterCall *call = static_cast<ContentFilterCall *>(data);
    GError *error = nullptr;
    WebKitUserContentFilter *filter =
        webkit_user_content_filter_store_load_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(object),
            static_cast<GAsyncResult *>(result), &error);
    if (!filter) {
        // A load miss just means this content hash was never
        // compiled — the save hop compiles it now (the call rides
        // on; it is still owned by the async chain).
        g_clear_error(&error);
        // source is transfer-none: the async task refs the GBytes for
        // its own lifetime — our ref is released right after the call.
        GBytes *source = g_bytes_new(call->source.constData(),
                                     gsize(call->source.size()));
        webkit_user_content_filter_store_save(
            WEBKIT_USER_CONTENT_FILTER_STORE(object),
            call->identifier.constData(),
            source,
            nullptr,
            reinterpret_cast<GAsyncReadyCallback>(
                &WebkitProfile::cbContentFilterSave), call);
        g_bytes_unref(source);
        return;
    }
    if (call->profile)
        call->profile->contentFilterCompiled(call->identifier,
                                             filter);
    else
        webkit_user_content_filter_unref(filter);
    delete call;
#else
    Q_UNUSED(object);
    Q_UNUSED(result);
    Q_UNUSED(data);
#endif
}

void WebkitProfile::cbContentFilterSave(void *object, void *result,
                                        void *data)
{
#ifdef ARKTOS_WEBKIT_LINKED
    ContentFilterCall *call = static_cast<ContentFilterCall *>(data);
    GError *error = nullptr;
    WebKitUserContentFilter *filter =
        webkit_user_content_filter_store_save_finish(
            WEBKIT_USER_CONTENT_FILTER_STORE(object),
            static_cast<GAsyncResult *>(result), &error);
    if (!filter) {
        qWarning("webkit: content-blocker compile failed: %s",
                 error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    if (call->profile)
        call->profile->contentFilterCompiled(call->identifier,
                                             filter);
    else if (filter)
        webkit_user_content_filter_unref(filter);
    delete call;
#else
    Q_UNUSED(object);
    Q_UNUSED(result);
    Q_UNUSED(data);
#endif
}

void WebkitProfile::syncContentBlocker()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_cbCompiling) {
        // A push landing mid-compile just re-pulls at finish — the
        // policy answers with its newest document then, so no
        // snapshot of the stale one is needed here.
        m_cbDirty = true;
        return;
    }
    const QByteArray doc =
        m_policy ? m_policy->contentBlockerJson() : QByteArray();

    // The doc wraps the Safari rule list with the honesty channels:
    // "rules" compiles verbatim; "dropped" lists each rule with no
    // Safari expression and why (counted + sampled into the log —
    // the no-silent-drop contract); "meta" feeds the diagnostic
    // counters the autotest prints.
    QByteArray rulesJson;
    int emitted = 0, dropped = 0, degraded = 0;
    if (!doc.isEmpty()) {
        const QJsonObject root = QJsonDocument::fromJson(doc).object();
        rulesJson = QJsonDocument(
                root.value(QLatin1String("rules")).toArray())
            .toJson(QJsonDocument::Compact);
        const QJsonObject meta =
            root.value(QLatin1String("meta")).toObject();
        emitted = meta.value(QLatin1String("emitted")).toInt();
        dropped = meta.value(QLatin1String("dropped")).toInt();
        degraded = meta.value(QLatin1String("degraded")).toInt();
        const QJsonArray drops =
            root.value(QLatin1String("dropped")).toArray();
        for (int i = 0; i < drops.size() && i < 8; ++i) {
            const QJsonObject drop = drops.at(i).toObject();
            qDebug("webkit: content-blocker dropped rule %s (%s)",
                   qPrintable(drop.value(QLatin1String("rule"))
                                  .toString()),
                   qPrintable(drop.value(QLatin1String("why"))
                                  .toString()));
        }
        if (drops.size() > 8)
            qDebug("webkit: ... and %d more dropped rules",
                   int(drops.size()) - 8);
    }
    m_cbEmitted = emitted;
    m_cbDropped = dropped;
    m_cbDegraded = degraded;

    if (rulesJson.isEmpty() || rulesJson == "[]") {
        // No expressible rules — strip the filter outright rather
        // than compile an empty list; a blocker toggled off lands
        // here through the rulesChanged re-push.
        if (m_filter || !m_cbIdentifier.isEmpty()) {
            applyContentFilter(nullptr, QByteArray());
            ++m_cbSyncs;
        }
        m_cbReady = true;
        return;
    }

    const QByteArray identifier = QByteArray("arktos-")
        + QCryptographicHash::hash(rulesJson,
                                   QCryptographicHash::Sha256).toHex();
    if (identifier == m_cbIdentifier)
        return;   // this exact ruleset is already live
    m_cbCompiling = true;
    WebKitUserContentFilterStore *store = contentFilterStore();
    ContentFilterCall *call = new ContentFilterCall;
    call->profile = this;
    call->identifier = identifier;
    call->source = rulesJson;
    webkit_user_content_filter_store_load(
        store, identifier.constData(), nullptr,
        reinterpret_cast<GAsyncReadyCallback>(
            &WebkitProfile::cbContentFilterLoad), call);
#endif
}

void WebkitProfile::contentFilterCompiled(
        const QByteArray &identifier, WebKitUserContentFilter *filter)
{
#ifdef ARKTOS_WEBKIT_LINKED
    m_cbCompiling = false;
    if (filter) {
        applyContentFilter(filter, identifier);
        // The caller's ref — applyContentFilter keeps its own.
        webkit_user_content_filter_unref(filter);
        ++m_cbSyncs;
        qInfo("webkit: content blocker installed — %d rules "
              "(%d dropped, %d degraded to block)",
              m_cbEmitted, m_cbDropped, m_cbDegraded);
    }
    // A failed compile leaves the previous filter live and marks the
    // pass settled — the next rulesChanged re-push retries.
    m_cbReady = true;
    if (m_cbDirty) {
        m_cbDirty = false;
        syncContentBlocker();
    }
#else
    Q_UNUSED(identifier);
    Q_UNUSED(filter);
#endif
}

void WebkitProfile::applyContentFilter(WebKitUserContentFilter *filter,
                                       const QByteArray &identifier)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (filter)
        webkit_user_content_filter_ref(filter);
    WebKitUserContentFilter *old = m_filter;
    m_filter = filter;
    m_cbIdentifier = identifier;
    // Swap order keeps coverage continuous on a live page: the new
    // filter lands before the old one leaves its UCM.
    for (WebkitPage *page : std::as_const(m_pages)) {
        if (!page->m_webView)
            continue;
        WebKitUserContentManager *manager =
            webkit_web_view_get_user_content_manager(page->m_webView);
        if (filter)
            webkit_user_content_manager_add_filter(manager, filter);
        if (old)
            webkit_user_content_manager_remove_filter(manager, old);
    }
    if (old)
        webkit_user_content_filter_unref(old);
#else
    Q_UNUSED(filter);
    Q_UNUSED(identifier);
#endif
}

void WebkitProfile::setCookieFilter(
        const std::function<bool(const Engine::CookieAttempt &)> &filter)
{
    // WebKitCookieManager offers only a 3-state accept policy — no
    // per-attempt consult exists, so the filter is accepted and
    // ignored (capabilities().cookieFilter stays false).
    Q_UNUSED(filter);
}

// WK16 — the live-page set the script propagation iterates; the
// page ctor attaches after its UCM work, the dtor detaches.
void WebkitProfile::attachPage(WebkitPage *page)
{
    m_pages.insert(page);
}

void WebkitProfile::detachPage(WebkitPage *page)
{
    m_pages.remove(page);
}

void WebkitProfile::insertScript(const Engine::Script &script)
{
    m_scripts.insert(script.name, script);
    // Per-view UCMs mean the collection physically lives on each
    // page — push the new script to pages that already exist (the
    // reach-existing-pages semantics WebEngine's profile collection
    // has).
    for (WebkitPage *page : std::as_const(m_pages))
        page->installProfileScript(script);
}

void WebkitProfile::removeScript(const QString &name)
{
    m_scripts.remove(name);
    for (WebkitPage *page : std::as_const(m_pages))
        page->removeProfileScript(name);
}

QList<Engine::Script> WebkitProfile::scripts() const
{
    return m_scripts.values();
}

void WebkitProfile::clear(Engine::StorageAreas areas)
{
#ifdef ARKTOS_WEBKIT_LINKED
    // WK12 — StorageAreas -> WebKitWebsiteDataTypes.  The engine-
    // neutral names map onto the stores WebKit actually persists;
    // VisitedLinksArea has no WebsiteDataManager store at all
    // (visited-link state lives in the web process's memory) and is
    // an honest no-op rather than a silently widened wipe.
    int types = 0;
    if (areas & Engine::HttpCacheArea)
        types |= WEBKIT_WEBSITE_DATA_MEMORY_CACHE
               | WEBKIT_WEBSITE_DATA_DISK_CACHE;
    if (areas & Engine::CookiesArea)
        types |= WEBKIT_WEBSITE_DATA_COOKIES;
    if (areas & Engine::DomStorageArea)
        types |= WEBKIT_WEBSITE_DATA_SESSION_STORAGE
               | WEBKIT_WEBSITE_DATA_LOCAL_STORAGE
               | WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES
               | WEBKIT_WEBSITE_DATA_DOM_CACHE;
    if (areas & Engine::ServiceWorkerArea)
        types |= WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS;
    if (areas == Engine::AllAreas) {
        // The interface's All is a full wipe — take every WebKit
        // data class, including the ones the flag map has no
        // neutral name for (offline app cache, HSTS cache, ITP
        // state, device-id hash salt).
        types = WEBKIT_WEBSITE_DATA_ALL;
    }
    if (!types)
        return;
    // Materialize the session to reach the manager that owns the
    // store — a persistent profile's data may already be on disk
    // from earlier loads (or an earlier process), so a pre-page
    // clear() still has work to do.  On an ephemeral session the
    // clear hits the memory stores — still the right wipe.
    WebKitNetworkSession *session = networkSession();
    WebKitWebsiteDataManager *manager =
        session ? webkit_network_session_get_website_data_manager(
                      session)
                : nullptr;
    if (!manager)
        return;
    webkit_website_data_manager_clear(
        manager, WebKitWebsiteDataTypes(types), 0, nullptr,
        clearFinished, nullptr);
#else
    Q_UNUSED(areas);
#endif
}

void WebkitProfile::fetchCookies(
        const std::function<void(const QStringList &)> &callback)
{
#ifdef ARKTOS_WEBKIT_LINKED
    // No session materialization here — a store that was never
    // created answers empty (unlike clear(), a read-back has no
    // on-disk work to reach).
    WebKitCookieManager *manager =
        m_networkSession
            ? webkit_network_session_get_cookie_manager(
                  m_networkSession)
            : nullptr;
    if (manager) {
        FetchCookiesCall *call = new FetchCookiesCall;
        call->callback = callback;
        webkit_cookie_manager_get_all_cookies(
            manager, nullptr, cookiesReady, call);
        return;
    }
#else
    Q_UNUSED(callback);
#endif
    if (callback)
        callback(QStringList());
}

void WebkitProfile::setProfileAttribute(const QString &name,
                                        const QVariant &value)
{
    // Free-form keys land here when the apply path doesn't know the
    // backend — ignored rather than misapplied.
    Q_UNUSED(name);
    Q_UNUSED(value);
}

// ---- WebkitBackend --------------------------------------------------

WebkitBackend::WebkitBackend(QObject *parent)
    : Engine::Backend(parent)
{
}

bool WebkitBackend::librariesLinked()
{
#ifdef ARKTOS_WEBKIT_LINKED
    return true;
#else
    return false;
#endif
}

QString WebkitBackend::wpeVersion()
{
#ifdef ARKTOS_WEBKIT_LINKED
    // The call is also what keeps the DT_NEEDED on libWPEWebKit real
    // — an unreferenced link would let --as-needed drop the library
    // and make the gate's "links with libs present" claim vacuous.
    return QStringLiteral("%1.%2.%3")
            .arg(webkit_get_major_version())
            .arg(webkit_get_minor_version())
            .arg(webkit_get_micro_version());
#else
    return QString();
#endif
}

QString WebkitBackend::wpePrefix()
{
    const QByteArray env = qgetenv("ARKTOS_WPE_PREFIX");
    if (!env.isEmpty())
        return QString::fromUtf8(env);
#ifdef ARKTOS_WPE_PREFIX_DIR
    return QStringLiteral(ARKTOS_WPE_PREFIX_DIR);
#else
    return QString();
#endif
}

void WebkitBackend::applyRuntimeEnvironment()
{
#ifdef ARKTOS_WEBKIT_LINKED
    // Headless/no-GPU embed: the web process must raster on CPU or it
    // produces dmabuf/EGL buffers wpe_buffer_import_to_pixels cannot
    // read back (WK03 render verdict).  WK20 re-measured: this stack
    // hands out identical XR24 X-tiled dmabufs either way and imports
    // them as EGLImages fine, but the flag stays — the raster
    // fallback's import_to_pixels must never meet an unmappable GPU
    // buffer on a stack whose producer differs.  An explicit operator
    // setting wins.
    if (!qEnvironmentVariableIsSet("WEBKIT_SKIA_ENABLE_CPU_RENDERING"))
        qputenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1");
    const QString prefix = wpePrefix();
    if (!prefix.isEmpty()
            && !qEnvironmentVariableIsSet("WEBKIT_INJECTED_BUNDLE_PATH")) {
        const QString bundle = prefix
            + QStringLiteral("/usr/lib/x86_64-linux-gnu/wpe-webkit-2.0"
                             "/injected-bundle/libWPEInjectedBundle.so");
        if (QFileInfo::exists(bundle))
            qputenv("WEBKIT_INJECTED_BUNDLE_PATH", bundle.toUtf8());
    }
#endif
}

WPEDisplay *WebkitBackend::sharedDisplay()
{
#ifdef ARKTOS_WEBKIT_LINKED
    static WPEDisplay *display = [] {
        WPEDisplay *candidate = wpe_display_headless_new();
        if (!candidate)
            return static_cast<WPEDisplay *>(nullptr);
        GError *error = nullptr;
        if (!wpe_display_connect(candidate, &error)) {
            qWarning("webkit: wpe_display_connect failed: %s",
                     error ? error->message : "?");
            g_clear_error(&error);
            g_object_unref(candidate);
            return static_cast<WPEDisplay *>(nullptr);
        }
        // Anything that consults wpe_display_get_primary() internally
        // lands here.
        wpe_display_set_primary(candidate);
        return candidate;
    }();
    return display;
#else
    return nullptr;
#endif
}

QString WebkitBackend::id() const
{
    return QStringLiteral("webkit");
}

QString WebkitBackend::displayName() const
{
    const QString version = wpeVersion();
    if (!version.isEmpty())
        return QStringLiteral("WebKit %1").arg(version);
    return QStringLiteral("WebKit");
}

QString WebkitBackend::engineNote() const
{
    return tr("Experimental engine — WPE WebKit backend (early)");
}

Engine::Capabilities WebkitBackend::capabilities() const
{
    // WK07: the conservative spike-verified table
    // (.devin/WEBKIT-SPIKE.md "Capabilities recommendations") — only
    // what WK01-WK04 proved or the 2.54 headers pin down.  The named
    // gaps stay off: no per-attempt CookieAttempt functor
    // (WebKitCookieManager offers only a 3-state accept policy).
    // WK21 measured the two stretch flags to false: devTools — the
    // build exports no WebKitWebInspector API at all (toggle only)
    // and the inspector's own gresource isn't installed; extensions —
    // the WebKitWebExtension entry points are compiled-out stubs
    // (webkit_web_extension_new is literally `return nullptr`).
    // An unlinked TU reports all-false — nothing is faked.
    Engine::Capabilities caps;
    caps.downloads = false;
    if (!librariesLinked())
        return caps;
    caps.requestInterception = true;
    // WK18: the declarative lane is real — the policy's rules
    // compile into a WebKitUserContentFilterStore filter that gates
    // subresource loads (block/block-cookies/css-display-none);
    // $redirect degrades to block and the inexpressible drops are
    // counted, not faked.
    caps.declarativeBlocking = true;
    caps.scriptInjection = true;
    caps.scriptEvaluation = true;
    caps.perPageSettings = true;
    caps.lifecycleDiscard = true;
    // WK19: these three were claimed ahead of the wiring — the
    // surfaces now exist and were verified: context-menu -> the
    // ContextMenuInfo marshal + WebKit-item popup; TLS errors -> the
    // SEC06 nonce interstitial + session-scoped certificate
    // exception; download-started -> Engine::DownloadRequest ->
    // DownloadManager (decide-destination is the request point —
    // WPE's only honest one; pause/resume stay honest no-ops).
    caps.contextMenuInfo = true;
    caps.certificateOverride = true;
    caps.downloads = true;
    return caps;
}

bool WebkitBackend::initialize()
{
    // Direct linking IS the loadable probe — a libWPEWebKit that
    // failed to resolve would have stopped the process at exec.
    if (!librariesLinked())
        return false;
    applyRuntimeEnvironment();
    return true;
}

Engine::Profile *WebkitBackend::createProfile(
        const Engine::ProfileOptions &options, QObject *parent)
{
    return new WebkitProfile(options, parent);
}

Engine::Page *WebkitBackend::createPage(Engine::Profile *profile,
                                      QObject *parent)
{
    // WK08: real pages now — the refusal is only a dead runtime (no
    // display, no webview), the same "never manufacture a doomed tab"
    // contract Servo's createPage keeps.
    if (!librariesLinked())
        return nullptr;
    applyRuntimeEnvironment();
    WebkitPage *page = new WebkitPage(profile, parent);
    if (!page->hasWebView()) {
        delete page;
        return nullptr;
    }
    return page;
}

QString WebkitBackend::pageUnavailableReason() const
{
    if (!librariesLinked())
        return tr("the WPE WebKit libraries were not found at build "
                  "time");
    if (!sharedDisplay())
        return tr("the WPE headless display could not be created");
    return QString();
}

QWidget *WebkitBackend::createView(Engine::Page *page, QWidget *parent)
{
    WebkitView *view = new WebkitView(parent);
    if (auto *webPage = qobject_cast<WebkitPage *>(page))
        webPage->attachView(view);
    return view;
}

namespace {

// WK07: self-registration through the addProbe seam — the servo
// shape: this TU exists only in webkit-enabled objects while
// engineregistry.o is linked into subproject binaries (tools,
// autotests) that never see it.  The probe runs on the first
// EngineRegistry::backends() call; a compile-but-inert build
// (webkit=1, no pkg-config) fails initialize() and stays unlisted —
// the same "missing artifact hides" contract a servo-less host gets.
void probeWebkitBackend()
{
    static bool probed = false;
    if (probed)
        return;
    probed = true;
    // qApp-owned: the registry lists backends without owning them,
    // so an unparented registered backend would leak at exit.
    WebkitBackend *candidate = new WebkitBackend(qApp);
    if (candidate->initialize())
        EngineRegistry::registerBackend(candidate);
    else
        delete candidate;
}

struct WebkitProbeRegistration {
    WebkitProbeRegistration()
    {
        EngineRegistry::addProbe(&probeWebkitBackend);
    }
} webkitProbeRegistration;

} // namespace
