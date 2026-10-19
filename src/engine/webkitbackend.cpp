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

#include "browserpaths.h"
#include "engineregistry.h"
#include "profilesettings.h"

#include <qapplication.h>
#include <qevent.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qpainter.h>
#include <qpixmap.h>

#include <cstring>

// ---- shared WebKit-side helpers (linked builds only) ---------------

#ifdef ARKTOS_WEBKIT_LINKED
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
        return ctx;
    }();
    return context;
}

// JSCValue -> QVariant.  Scalar types map directly; undefined and
// null both become an invalid QVariant; objects/arrays hand back
// their JSON text form (the documented marshaling limit — WK15 keeps
// the type matrix + error mapping).
QVariant jscValueToVariant(JSCValue *value)
{
    if (!value || jsc_value_is_undefined(value)
            || jsc_value_is_null(value))
        return QVariant();
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
// the image, so rows are deep-copied like adoptFrame.
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

} // namespace
#endif // ARKTOS_WEBKIT_LINKED

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

void WebkitView::adoptFrame(WPEBuffer *buffer, const void *data,
                            int stride)
{
#ifdef ARKTOS_WEBKIT_LINKED
    const int w = wpe_buffer_get_width(buffer);
    const int h = wpe_buffer_get_height(buffer);
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    const quint8 *src = static_cast<const quint8 *>(data);
    for (int y = 0; y < h; ++y)
        memcpy(img.scanLine(y), src + size_t(stride) * y,
               size_t(w) * 4);
    m_frame = img;
    m_frameSize = QSize(w, h);
    ++m_frameCount;
    update();
#else
    Q_UNUSED(buffer);
    Q_UNUSED(data);
    Q_UNUSED(stride);
#endif
}

void WebkitView::noteImportFailure()
{
    ++m_importFails;
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
#else
    Q_UNUSED(profile);
#endif
}

WebkitPage::~WebkitPage()
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (m_view)
        m_view->m_page = nullptr;
    // The WebKitWebView owns its WPEView; unrefing it tears the view
    // down and drops every g_signal handler bound to `this`.
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
    // WK16 maps worldId onto named script worlds; main world only
    // until then.
    Q_UNUSED(worldId);
    runJavaScript(source, resultCallback);
}

void WebkitPage::toHtml(
        const std::function<void(const QString &)> &resultCallback)
{
    // WK15: document.documentElement.outerHTML eval.
    if (resultCallback)
        resultCallback(QString());
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
    m_attributes.insert(name, on);
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
}

void WebkitPage::removeScript(const QString &name)
{
    m_scripts.remove(name);
}

QList<Engine::Script> WebkitPage::scripts() const
{
    return m_scripts.values();
}

void WebkitPage::download(const QUrl &url)
{
    Q_UNUSED(url);
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

#endif // ARKTOS_WEBKIT_LINKED

void WebkitPage::onBufferRendered(WPEBuffer *buffer)
{
#ifdef ARKTOS_WEBKIT_LINKED
    if (!buffer || !m_view)
        return;
    // WK03 contract: WebKit (the wpe_view_render_buffer caller) owns
    // the rendered/released bookkeeping — the embedder only imports +
    // copies inside the handler.  The returned GBytes is OWNED BY THE
    // BUFFER (borrowed, never unrefed).
    GError *error = nullptr;
    GBytes *pixels = wpe_buffer_import_to_pixels(buffer, &error);
    if (!pixels) {
        m_view->noteImportFailure();
        qWarning("webkit: wpe_buffer_import_to_pixels failed (%s): %s",
                 G_OBJECT_TYPE_NAME(buffer),
                 error ? error->message : "?");
        g_clear_error(&error);
        return;
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
        m_view->noteImportFailure();
        qWarning("webkit: imported pixels undersized: %zu for %dx%d "
                 "stride=%d", size_t(len), bw, bh, stride);
        return;
    }
    m_view->adoptFrame(buffer, data, stride);
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
    if ((type == int(WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
            || type == int(WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION))
            && WEBKIT_IS_NAVIGATION_POLICY_DECISION(decision)) {
        WebKitNavigationAction *action =
            webkit_navigation_policy_decision_get_navigation_action(
                WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        if (action)
            m_lastNavigationType = engineNavigationType(
                webkit_navigation_action_get_navigation_type(action),
                webkit_navigation_action_is_redirect(action));
    }
#else
    Q_UNUSED(decision);
    Q_UNUSED(type);
#endif
    return false;   // observe-only until WK17 — WebKit decides "use"
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
    m_userAgent = userAgent;
}

void WebkitProfile::setRequestPolicy(Engine::RequestPolicy *policy)
{
    m_policy = policy;
}

Engine::RequestPolicy *WebkitProfile::requestPolicy() const
{
    return m_policy;
}

void WebkitProfile::setCookieFilter(
        const std::function<bool(const Engine::CookieAttempt &)> &filter)
{
    m_cookieFilter = filter;
}

void WebkitProfile::insertScript(const Engine::Script &script)
{
    m_scripts.insert(script.name, script);
}

void WebkitProfile::removeScript(const QString &name)
{
    m_scripts.remove(name);
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
    m_attributes.insert(name, value);
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
    // read back (WK03 render verdict).  An explicit operator setting
    // wins; WK20's dmabuf/EGLImage→GL path revisits the default.
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
    // (WebKitCookieManager offers only a 3-state accept policy), no
    // embedder devtools channel (the built-in inspector exists but
    // exports no attach API), WebKitWebExtension present-but-unprobed.
    // An unlinked TU reports all-false — nothing is faked.
    Engine::Capabilities caps;
    caps.downloads = false;
    if (!librariesLinked())
        return caps;
    caps.requestInterception = true;
    caps.scriptInjection = true;
    caps.scriptEvaluation = true;
    caps.perPageSettings = true;
    caps.lifecycleDiscard = true;
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
