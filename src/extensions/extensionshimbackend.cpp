/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "extensionshimbackend.h"

#include "autofillmanager.h"
#include "bookmarksmanager.h"
#include "bookmarknode.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "clearprivatedata.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "declarativenetrequest.h"
#include "downloadmanager.h"
#include "engineinterface.h"
#include "extensionbackgroundhost.h"
#include "extensioncommands.h"
#include "extensioneventhub.h"
#include "extensionidlewatcher.h"
#include "extensionmanager.h"
#include "extensionnotificationcenter.h"
#include "extensionreviewdialog.h"
#include "historymanager.h"
#include "opensearchmanager.h"
#include "privacyrequestinterceptor.h"
#include "profilesettings.h"
#include "tabbar.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webenginebackend.h"
#include "webview.h"

#include <cmath>

#include <qapplication.h>
#include <qbuffer.h>
#include <qcolor.h>
#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileiconprovider.h>
#include <qfontdatabase.h>
#include <qguiapplication.h>
#include <qhostinfo.h>
#include <qimage.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qlocale.h>
#include <qpixmap.h>
#include <qpointer.h>
#include <qregularexpression.h>
#include <qsavefile.h>
#include <qscreen.h>
#include <qset.h>
#include <qstorageinfo.h>
#include <qstyle.h>
#include <qsysinfo.h>
#include <qthread.h>
#include <qtimer.h>
#include <qvariant.h>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

#include <qnetworkcookie.h>
#include <qwebenginecookiestore.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

#if defined(ARKTOS_RUSTCORE)
#include "rustcorebridge.h"
#include <arktos_core.h>
#include <rustcore.h>
#endif

#include <algorithm>
#include <limits>

namespace {

// Reply shapes: every invoke() answers {ok:true,result:...} or
// {ok:false,error:"<typed code>"} — the JS shim turns failures into
// health-surface gaps.
QJsonObject okReply(const QJsonValue &result = QJsonValue())
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    if (!result.isUndefined())
        o.insert(QStringLiteral("result"), result);
    return o;
}

QJsonObject errReply(const QString &code)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    return o;
}

// Per-extension action state — chrome.action/browserAction setters
// land here; EXT08's action bar (main lane) reads it through
// ExtensionInfo::action* fields on merge.
QHash<QString, ExtensionShimBackend::ActionState> &actionStates()
{
    static QHash<QString, ExtensionShimBackend::ActionState> states;
    return states;
}

// EXT16 shared contextMenus registry — the page context menu reads
// one ordered item list per extension (registration order kept;
// QHash iteration order would scramble a menu).  The owners map
// keys the registry to the live backend: a respawning host hands
// its slot to the new backend, and a stale destructor clears only
// what it still owns.
struct MenuRegistry {
    QHash<QString, QList<QPair<QString, QJsonObject>>> items;
    QHash<QString, QString> names;
    QHash<QString, const ExtensionShimBackend *> owners;
};

MenuRegistry &menuRegistry()
{
    static MenuRegistry registry;
    return registry;
}

// Session-stable id minting for tab/window objects — a dynamic
// property on the object itself, so a destroyed object takes its id
// with it and a recycled address can never inherit it.
int idFor(const QObject *object)
{
    if (!object)
        return -1;
    static int next = 1;
    const QVariant v = object->property("arktosExtObjectId");
    if (v.isValid() && v.toInt() > 0)
        return v.toInt();
    const int id = next++;
    const_cast<QObject *>(object)->setProperty("arktosExtObjectId", id);
    return id;
}

// The window a chrome.tabs call targets for currentWindow /
// lastFocusedWindow — the active top-level, else the first seen.
// Enumerates widgets like the EXT16 hub so the set covers windows
// the app registry does not track (private windows, qtest windows)
// and never dereferences a null instance() under autotests.
BrowserMainWindow *currentWindow()
{
    BrowserMainWindow *fallback = nullptr;
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window)
            continue;
        if (window->isActiveWindow())
            return window;
        if (!fallback)
            fallback = window;
    }
    return fallback;
}

// One resolved tab — every tabs.* op lands on this four-tuple so a
// mid-dispatch strip reorder never confuses (window, index, view)
// again.  index is the strip position at resolve time; ops that
// move things re-resolve through webViewIndex().
struct LocatedTab {
    BrowserMainWindow *window = nullptr;
    TabWidget *tabs = nullptr;
    WebView *view = nullptr;
    int index = -1;
    int windowId = -1;
};

// Resolve a chrome.tabs tabId to the live strip position — ids
// minted by idFor() only describe trackable tabs (the same set
// query enumerates).  tabId < 0 names the current window's current
// tab, the default Chrome gives ops called without one.
LocatedTab locateTab(int tabId)
{
    BrowserMainWindow *current = currentWindow();
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window || !window->tabWidget())
            continue;
        TabWidget *tw = window->tabWidget();
        const bool wantCurrent = tabId < 0 && window == current;
        for (int i = 0; i < tw->count(); ++i) {
            WebView *view = tw->webView(i);
            if (!ExtensionEventHub::trackable(view))
                continue;
            if (tabId < 0 ? (wantCurrent && i == tw->currentIndex())
                          : idFor(view) == tabId) {
                LocatedTab out;
                out.window = window;
                out.tabs = tw;
                out.view = view;
                out.index = i;
                out.windowId = idFor(window);
                return out;
            }
        }
    }
    return LocatedTab();
}

// chrome.tabs reports groupId as an int; the strip keys group
// membership on per-window strings.  Mint a session-stable int per
// (window, group) pair — Chrome's -1 stays "no group".
int chromeGroupId(int windowId, const QString &groupId)
{
    if (groupId.isEmpty())
        return -1;
    static QHash<QString, int> ids;
    static int next = 1;
    const QString key = QString::number(windowId)
        + QLatin1Char('/') + groupId;
    int &slot = ids[key];
    if (!slot)
        slot = next++;
    return slot;
}

// chrome.tabGroups' Color enum onto the Arktos palette — the same
// ContainerManager::defaultColors() swatches the group's own color
// submenu offers, so a written color renders exactly what the UI
// could pick.  'grey' has no palette member and writes Chrome's
// canonical grey instead.  Reads classify the live color to the
// nearest name — a custom color lands on its closest Chrome name.
const QList<QPair<QString, QColor>> &chromeGroupPalette()
{
    static const QList<QPair<QString, QColor>> colors = {
        {QStringLiteral("blue"),   QColor(0x37, 0xad, 0xff)},
        {QStringLiteral("cyan"),   QColor(0x00, 0xc7, 0x9a)},
        {QStringLiteral("green"),  QColor(0x51, 0xcd, 0x00)},
        {QStringLiteral("yellow"), QColor(0xff, 0xcb, 0x00)},
        {QStringLiteral("orange"), QColor(0xff, 0x9f, 0x00)},
        {QStringLiteral("red"),    QColor(0xff, 0x61, 0x3d)},
        {QStringLiteral("pink"),   QColor(0xff, 0x4b, 0xda)},
        {QStringLiteral("purple"), QColor(0xaf, 0x51, 0xf5)},
        {QStringLiteral("grey"),   QColor(0x80, 0x86, 0x8b)},
    };
    return colors;
}

QString chromeGroupColorName(const QColor &color)
{
    int best = -1;
    qint64 bestDist = std::numeric_limits<qint64>::max();
    for (int i = 0; i < chromeGroupPalette().size(); ++i) {
        const QColor c = chromeGroupPalette().at(i).second;
        const qint64 dr = color.red() - c.red();
        const qint64 dg = color.green() - c.green();
        const qint64 db = color.blue() - c.blue();
        const qint64 dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best >= 0 ? chromeGroupPalette().at(best).first
                     : QStringLiteral("grey");
}

QColor chromeGroupColor(const QString &name)
{
    for (const auto &entry : chromeGroupPalette()) {
        if (entry.first == name)
            return entry.second;
    }
    return QColor();    // invalid -> the caller's invalid-argument
}

// chrome group id -> the live (window, string-id) pair: enumerate
// the strip-visible groups of every window and compare minted ids.
// Minting is stable per pair, so a live group resolves whether or
// not a query() ran first; a stale id (group gone) never matches —
// the same dead-id verdict bmResolve answers for bookmarks.
bool resolveTabGroup(int chromeId, BrowserMainWindow **windowOut,
                     QString *groupIdOut)
{
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        TabWidget *tw = window ? window->tabWidget() : nullptr;
        if (!tw)
            continue;
        const int wid = idFor(window);
        for (const QString &gid : tw->tabGroupIds()) {
            if (chromeGroupId(wid, gid) == chromeId) {
                if (windowOut)
                    *windowOut = window;
                if (groupIdOut)
                    *groupIdOut = gid;
                return true;
            }
        }
    }
    return false;
}

// MD11: Chrome's zoomSettings shape for a tab — the recorded
// setZoomSettings request merged over the honest defaults: Arktos
// zoom is per-tab (never per-origin) and persists across
// navigation, which is Chrome's "manual" mode.
QJsonObject zoomSettingsFor(const WebView *view)
{
    QJsonObject out{
        {QStringLiteral("mode"), QStringLiteral("manual")},
        {QStringLiteral("scope"), QStringLiteral("per-tab")},
        {QStringLiteral("defaultZoomFactor"), 1.0}};
    const QJsonObject recorded = view
        ? view->property("arktosExtZoomSettings").toJsonObject()
        : QJsonObject();
    for (const QString &key :
             {QStringLiteral("mode"), QStringLiteral("scope")}) {
        if (recorded.contains(key))
            out.insert(key, recorded.value(key));
    }
    return out;
}

// -------------------------------------------------------------------
// MD21: chrome.bookmarks over the RC05 store.  The C++ BookmarkNode
// wrappers are already the facade/FFI lane (children materialize one
// rc_bm_child_at handle at a time; mutations ride the undoable
// manager ops so entryAdded/entryRemoved/entryChanged keep the
// model, toolbar and autosave consistent).
//
// Chrome ids: "0" names the root, "1" the bookmarks bar (our
// toolbar), "2" Other bookmarks (our menu) — the fixed ids real
// extensions reach for.  Every other node serializes its store
// handle (rust builds) or object identity (no-rust) offset by a
// base, so produced ids can never collide with the fixed ids.
// Resolution walks the live tree comparing serials — a dead handle
// (removed node, rebound profile tree) simply never matches, so no
// stale pointer is ever dereferenced.
constexpr quint64 kBmIdBase = 1024;

quint64 bmSerial(const BookmarkNode *node)
{
#ifdef ARKTOS_RUSTCORE
    if (node->handle())
        return node->handle();
#endif
    return quintptr(node);
}

bool bmIsSpecial(BookmarksManager *manager, const BookmarkNode *node)
{
    return node == manager->bookmarks() || node == manager->toolbar()
        || node == manager->menu();
}

QString bmId(BookmarksManager *manager, const BookmarkNode *node)
{
    if (node == manager->bookmarks())
        return QStringLiteral("0");
    if (node == manager->toolbar())
        return QStringLiteral("1");
    if (node == manager->menu())
        return QStringLiteral("2");
    return QString::number(bmSerial(node) + kBmIdBase);
}

BookmarkNode *bmWalk(BookmarksManager *manager, BookmarkNode *node,
                     quint64 serial)
{
    if (!bmIsSpecial(manager, node) && bmSerial(node) == serial)
        return node;
    const QList<BookmarkNode *> children = node->children();
    for (BookmarkNode *child : children) {
        if (BookmarkNode *hit = bmWalk(manager, child, serial))
            return hit;
    }
    return nullptr;
}

// id -> live node; nullptr covers both malformed ids and removed
// nodes (a detached node is unreachable — Chrome's not-found).
BookmarkNode *bmResolve(BookmarksManager *manager, const QString &id)
{
    if (id == QLatin1String("0"))
        return manager->bookmarks();
    if (id == QLatin1String("1"))
        return manager->toolbar();
    if (id == QLatin1String("2"))
        return manager->menu();
    bool ok = false;
    const quint64 n = id.toULongLong(&ok);
    if (!ok || n <= kBmIdBase)
        return nullptr;
    return bmWalk(manager, manager->bookmarks(), n - kBmIdBase);
}

// The Chrome BookmarkTreeNode shape, plus Firefox's WebExtension
// 'type' member so a separator serializes honestly (Chrome has no
// separator node).  The store carries no timestamps, so
// dateAdded/dateGroupModified are honestly omitted rather than
// fabricated.
QJsonObject bmNodeJson(BookmarksManager *manager, BookmarkNode *node,
                     bool deep)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), bmId(manager, node));
    if (BookmarkNode *p = node->parent()) {
        o.insert(QStringLiteral("parentId"), bmId(manager, p));
        o.insert(QStringLiteral("index"),
                 p->children().indexOf(node));
    }
    o.insert(QStringLiteral("title"), node->title);
    switch (node->type()) {
    case BookmarkNode::Bookmark:
        o.insert(QStringLiteral("type"), QStringLiteral("bookmark"));
        o.insert(QStringLiteral("url"), node->url);
        break;
    case BookmarkNode::Separator:
        o.insert(QStringLiteral("type"), QStringLiteral("separator"));
        break;
    default:
        o.insert(QStringLiteral("type"), QStringLiteral("folder"));
        if (deep) {
            QJsonArray children;
            const QList<BookmarkNode *> kids = node->children();
            for (BookmarkNode *child : kids)
                children.append(bmNodeJson(manager, child, true));
            o.insert(QStringLiteral("children"), children);
        }
        break;
    }
    return o;
}

// Depth-first collection of every attached node under root.
void bmCollect(BookmarkNode *node, QList<BookmarkNode *> *out)
{
    out->append(node);
    const QList<BookmarkNode *> children = node->children();
    for (BookmarkNode *child : children)
        bmCollect(child, out);
}

// The store's insert-time url policy (rc_bm_create applies
// check_bookmark_url with allow_javascript=false) mirrored so a
// refused scheme answers typed BEFORE the node reaches the C++
// tree: http/https/ftp/mailto and schemeless urls pass, everything
// else (javascript:, data:, file:, arktos-*) refuses — an extension
// is code, not a human's deliberate edit.
bool bmUrlAllowed(const QString &url)
{
    QString probe = url;
    probe.remove(
        QRegularExpression(QStringLiteral("[\\x00-\\x20\\x7f]+")));
    const int colon = probe.indexOf(QLatin1Char(':'));
    if (colon < 0)
        return true;    // schemeless — the store calls it harmless
    const QString scheme = probe.left(colon).toLower();
    if (scheme.isEmpty() || !scheme.at(0).isLetter())
        return true;
    for (const QChar c : scheme) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('+')
                && c != QLatin1Char('-') && c != QLatin1Char('.'))
            return true;    // not a scheme token — treated relative
    }
    return scheme == QLatin1String("http")
        || scheme == QLatin1String("https")
        || scheme == QLatin1String("ftp")
        || scheme == QLatin1String("mailto");
}

// bookmarks.search query tokenizing — space-delimited terms with
// double-quoted phrase grouping, the string form Chrome documents.
QStringList bmQueryTerms(const QString &query)
{
    QStringList terms;
    QString current;
    bool inQuote = false;
    for (const QChar c : query) {
        if (c == QLatin1Char('"')) {
            inQuote = !inQuote;
        } else if (c.isSpace() && !inQuote) {
            if (!current.isEmpty()) {
                terms.append(current);
                current.clear();
            }
        } else {
            current.append(c);
        }
    }
    if (!current.isEmpty())
        terms.append(current);
    return terms;
}

// Bookmark-manager signal -> chrome.bookmarks.on* broadcast, for
// changes that arrive through the UI (drag, dialog, undo).  The API
// ops suppress this tap while they run and emit their own
// Chrome-accurate events (a move must report onMoved, not the
// onRemoved+onCreated pair its remove+insert decomposition fires).
int s_bmApiDepth = 0;

struct BmApiSuppress {
    BmApiSuppress() { ++s_bmApiDepth; }
    ~BmApiSuppress() { --s_bmApiDepth; }
};

void bmBroadcast(const QString &name, const QJsonArray &eventArgs)
{
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        name, QJsonObject{{QStringLiteral("__args"), eventArgs}});
}

QPointer<BookmarksManager> s_bmTapTarget;
QList<QMetaObject::Connection> s_bmTapConns;

// Lazily binds the ambient profile's manager signals to the event
// fan-out; re-points when the ambient persona (and therefore the
// manager) changes — bookmark events must describe the profile the
// extension actually reads through this namespace.
void tapBmEvents(BookmarksManager *manager)
{
    if (s_bmTapTarget == manager)
        return;
    for (const QMetaObject::Connection &conn : s_bmTapConns)
        QObject::disconnect(conn);
    s_bmTapConns.clear();
    s_bmTapTarget = manager;
    if (!manager)
        return;
    const QPointer<BookmarksManager> weak = manager;
    s_bmTapConns << QObject::connect(
        manager, &BookmarksManager::entryAdded, manager,
        [weak](BookmarkNode *node) {
            if (s_bmApiDepth || !weak || !node)
                return;
            bmBroadcast(QStringLiteral("bookmarks.onCreated"),
                        {bmId(weak.data(), node),
                         bmNodeJson(weak.data(), node, false)});
        });
    s_bmTapConns << QObject::connect(
        manager, &BookmarksManager::entryRemoved, manager,
        [weak](BookmarkNode *parent, int row, BookmarkNode *item) {
            if (s_bmApiDepth || !weak || !parent || !item)
                return;
            bmBroadcast(
                QStringLiteral("bookmarks.onRemoved"),
                {bmId(weak.data(), item),
                 QJsonObject{
                     {QStringLiteral("parentId"),
                      bmId(weak.data(), parent)},
                     {QStringLiteral("index"), row},
                     {QStringLiteral("node"),
                      bmNodeJson(weak.data(), item, true)}}});
        });
    s_bmTapConns << QObject::connect(
        manager, &BookmarksManager::entryChanged, manager,
        [weak](BookmarkNode *node) {
            if (s_bmApiDepth || !weak || !node)
                return;
            QJsonObject changeInfo{
                {QStringLiteral("title"), node->title}};
            if (node->type() == BookmarkNode::Bookmark)
                changeInfo.insert(QStringLiteral("url"), node->url);
            bmBroadcast(QStringLiteral("bookmarks.onChanged"),
                        {bmId(weak.data(), node), changeInfo});
        });
}

// -------------------------------------------------------------------
// MD22: chrome.history over the RC04 store.  HistoryManager is
// already the facade/FFI lane: under rustcore m_history mirrors
// history.db and every mutation below rides the same store ops the
// UI uses, so deletes walk the same remove/clear path
// checkForExpired/clearSince take (icons and models included).
//
// Chrome shapes: a HistoryItem is the per-url aggregate (visitCount
// across the visit rows, lastVisitTime the newest).  The store has
// no separate url table, so the group-by runs over the manager's
// newest-first listing — first occurrence wins the group's order
// slot and its newest visit's title/timestamp.  ids are FNV-1a
// serials of the url masked to JS's exact-integer range — stable
// across calls, deterministic, never a pointer.  The store carries
// no transition or typed-count column: every VisitItem answers
// 'link' and HistoryItem.typedCount 0, noted once per context
// rather than fabricated.
quint64 histHash(const QByteArray &key)
{
    quint64 h = 14695981039346656037ULL;   // FNV-1a 64 offset basis
    for (const char c : key) {
        h ^= quint64(quint8(c));
        h *= 1099511628211ULL;             // FNV prime
    }
    return h & 0x1fffffffffffffULL;        // 2^53-1: exact JSON int
}

// The addUrl-time scheme policy, same posture the bookmarks url gate
// takes: extension code is not a user's deliberate navigation, so
// script/data-bearing spellings never enter the visit log; every
// well-formed url a page could actually load (file: included —
// Chrome records it too) passes.
bool histUrlAllowed(const QString &url)
{
    QString probe = url;
    probe.remove(
        QRegularExpression(QStringLiteral("[\\x00-\\x20\\x7f]+")));
    const int colon = probe.indexOf(QLatin1Char(':'));
    if (colon < 0)
        return true;    // schemeless — treated relative/harmless
    const QString scheme = probe.left(colon).toLower();
    if (scheme.isEmpty() || !scheme.at(0).isLetter())
        return true;
    for (const QChar c : scheme) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('+')
                && c != QLatin1Char('-') && c != QLatin1Char('.'))
            return true;    // not a scheme token — treated relative
    }
    return scheme != QLatin1String("javascript")
        && scheme != QLatin1String("data")
        && scheme != QLatin1String("vbscript");
}

// The Chrome HistoryItem object for one url's visit rows.
QJsonObject histItemJson(const QString &url, const QString &title,
                         qint64 lastVisitMs, int visitCount)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"),
             QString::number(histHash(url.toUtf8())));
    o.insert(QStringLiteral("url"), url);
    if (!title.isEmpty())
        o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("lastVisitTime"), double(lastVisitMs));
    o.insert(QStringLiteral("visitCount"), visitCount);
    // Not tracked by the store — the honest zero, not a guess.
    o.insert(QStringLiteral("typedCount"), 0);
    return o;
}

// Aggregate a url's rows into its HistoryItem; empty object when the
// url has no visit at all.
QJsonObject histItemForUrl(const QList<HistoryEntry> &entries,
                           const QString &url)
{
    QString title;
    qint64 lastTs = 0;
    int visits = 0;
    for (const HistoryEntry &e : entries) {
        if (e.url != url)
            continue;
        if (!visits) {
            title = e.title;
            lastTs = e.dateTime.toMSecsSinceEpoch();
        }
        ++visits;
    }
    if (!visits)
        return QJsonObject();
    return histItemJson(url, title, lastTs, visits);
}

// One url's visit rows -> Chrome's reverse-chronological VisitItem[]
// (the listing is already newest-first).  visitId serials mix the
// same-stamp ordinal so two visits sharing url AND millisecond still
// mint distinct ids.
QJsonArray histVisitItems(const QList<HistoryEntry> &entries,
                          const QString &url)
{
    QJsonArray out;
    QHash<qint64, int> sameStamp;
    for (const HistoryEntry &e : entries) {
        if (e.url != url)
            continue;
        const qint64 ts = e.dateTime.toMSecsSinceEpoch();
        const int k = sameStamp.value(ts);
        sameStamp.insert(ts, k + 1);
        QJsonObject v;
        v.insert(QStringLiteral("id"),
                 QString::number(histHash(url.toUtf8())));
        v.insert(QStringLiteral("visitId"), double(histHash(
            (url + QLatin1Char('\x1f') + QString::number(ts)
             + QLatin1Char('\x1f') + QString::number(k)).toUtf8())));
        v.insert(QStringLiteral("visitTime"), double(ts));
        // Referring visits are not tracked — Chrome's "no referrer".
        v.insert(QStringLiteral("referringVisitId"),
                 QStringLiteral("0"));
        v.insert(QStringLiteral("transition"),
                 QStringLiteral("link"));
        out.append(v);
    }
    return out;
}

// History-manager signal -> chrome.history.on* broadcast, for
// changes that arrive through the UI (page visits feed entryAdded,
// the dialog's delete entryRemoved, Clear History historyCleared).
// API ops suppress the tap while they run and emit their own
// Chrome-accurate aggregate event (a deleteRange reports one
// onVisitRemoved carrying every affected url, not one per row).
int s_histApiDepth = 0;

struct HistApiSuppress {
    HistApiSuppress() { ++s_histApiDepth; }
    ~HistApiSuppress() { --s_histApiDepth; }
};

void histBroadcast(const QString &name, const QJsonArray &eventArgs)
{
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        name, QJsonObject{{QStringLiteral("__args"), eventArgs}});
}

QPointer<HistoryManager> s_histTapTarget;
QList<QMetaObject::Connection> s_histTapConns;

// Lazily binds the ambient profile's manager signals to the event
// fan-out; re-points when the ambient persona (and therefore the
// manager) changes — history events must describe the profile the
// extension actually reads through this namespace.
void tapHistEvents(HistoryManager *manager)
{
    if (s_histTapTarget == manager)
        return;
    for (const QMetaObject::Connection &conn : s_histTapConns)
        QObject::disconnect(conn);
    s_histTapConns.clear();
    s_histTapTarget = manager;
    if (!manager)
        return;
    const QPointer<HistoryManager> weak = manager;
    s_histTapConns << QObject::connect(
        manager, &HistoryManager::entryAdded, manager,
        [weak](const HistoryEntry &item) {
            if (s_histApiDepth || !weak)
                return;
            const QJsonObject histItem =
                histItemForUrl(weak->history(), item.url);
            if (!histItem.isEmpty())
                histBroadcast(QStringLiteral("history.onVisited"),
                              {histItem});
        });
    s_histTapConns << QObject::connect(
        manager, &HistoryManager::entryRemoved, manager,
        [weak](const HistoryEntry &item) {
            if (s_histApiDepth || !weak)
                return;
            histBroadcast(QStringLiteral("history.onVisitRemoved"),
                          {QJsonObject{
                              {QStringLiteral("allHistory"), false},
                              {QStringLiteral("urls"),
                               QJsonArray{item.url}}}});
        });
    s_histTapConns << QObject::connect(
        manager, &HistoryManager::historyCleared, manager, [weak]() {
            if (s_histApiDepth || !weak)
                return;
            histBroadcast(QStringLiteral("history.onVisitRemoved"),
                          {QJsonObject{
                              {QStringLiteral("allHistory"), true},
                              {QStringLiteral("urls"),
                               QJsonArray()}}});
        });
}

// -------------------------------------------------------------------
// MD23: chrome.cookies — the persistent-mirror read side + the
// engine-store signal tap behind onChanged.  Records travel in the
// RC15 wire shape ({"container","name","value","domain","path",
// "expires" ms|null,"secure","httpOnly","sameSite" 0..3,"hostOnly",
// "created"}); the no-rust read path derives the same shape from the
// C++ jar's live QNetworkCookie mirror so every filter runs on one
// pipeline.
// -------------------------------------------------------------------

// QNetworkCookie -> the RC15 record shape (container "": the
// extension store is always the default partition — the ambient
// profile's jar).
QJsonObject cookieRecordJson(const QNetworkCookie &cookie)
{
    QString domain = cookie.domain();
    const bool hostOnly = !domain.startsWith(QLatin1Char('.'));
    if (!hostOnly)
        domain = domain.mid(1);
    QJsonObject o;
    o.insert(QLatin1String("container"), QString());
    o.insert(QLatin1String("name"),
             QString::fromUtf8(cookie.name()));
    o.insert(QLatin1String("value"),
             QString::fromUtf8(cookie.value()));
    o.insert(QLatin1String("domain"), domain);
    o.insert(QLatin1String("path"), cookie.path().isEmpty()
             ? QStringLiteral("/") : cookie.path());
    if (cookie.isSessionCookie())
        o.insert(QLatin1String("expires"), QJsonValue());
    else
        o.insert(QLatin1String("expires"),
                 double(cookie.expirationDate().toMSecsSinceEpoch()));
    o.insert(QLatin1String("secure"), cookie.isSecure());
    o.insert(QLatin1String("httpOnly"), cookie.isHttpOnly());
    o.insert(QLatin1String("sameSite"), int(cookie.sameSitePolicy()));
    o.insert(QLatin1String("hostOnly"), hostOnly);
    o.insert(QLatin1String("created"),
             double(QDateTime::currentMSecsSinceEpoch()));
    return o;
}

// The Chrome Cookie object for a record: domain re-derives the
// leading dot from hostOnly, expires-ms becomes seconds
// expirationDate (omitted on session cookies), sameSite answers in
// Chrome's enum vocabulary.  Arktos serves one store — "0".
QJsonObject chromeCookieJson(const QJsonObject &rec,
                             const QString &storeId)
{
    const QString domain = rec.value(QLatin1String("domain")).toString();
    const bool hostOnly = rec.value(QLatin1String("hostOnly")).toBool();
    QJsonObject o;
    o.insert(QLatin1String("name"), rec.value(QLatin1String("name")));
    o.insert(QLatin1String("value"), rec.value(QLatin1String("value")));
    o.insert(QLatin1String("domain"), hostOnly
             ? domain : QStringLiteral(".") + domain);
    o.insert(QLatin1String("hostOnly"), hostOnly);
    o.insert(QLatin1String("path"), rec.value(QLatin1String("path")));
    o.insert(QLatin1String("secure"), rec.value(QLatin1String("secure")));
    o.insert(QLatin1String("httpOnly"),
             rec.value(QLatin1String("httpOnly")));
    const QJsonValue exp = rec.value(QLatin1String("expires"));
    const bool session = exp.isNull() || exp.isUndefined();
    o.insert(QLatin1String("session"), session);
    if (!session)
        o.insert(QLatin1String("expirationDate"),
                 exp.toDouble() / 1000.0);
    static const char *names[] = {
        "unspecified", "no_restriction", "lax", "strict"};
    const int ss = qBound(0, rec.value(QLatin1String("sameSite")).toInt(),
                          3);
    o.insert(QLatin1String("sameSite"), QLatin1String(names[ss]));
    o.insert(QLatin1String("storeId"), storeId);
    return o;
}

// The Chrome Cookie object straight from a QNetworkCookie — the
// onChanged tap's conversion (it needs no RC15 round trip).
QJsonObject chromeCookieJson(const QNetworkCookie &cookie,
                             const QString &storeId)
{
    QString domain = cookie.domain();
    const bool hostOnly = !domain.startsWith(QLatin1Char('.'));
    QJsonObject o;
    o.insert(QLatin1String("name"),
             QString::fromUtf8(cookie.name()));
    o.insert(QLatin1String("value"),
             QString::fromUtf8(cookie.value()));
    o.insert(QLatin1String("domain"), domain);
    o.insert(QLatin1String("hostOnly"), hostOnly);
    o.insert(QLatin1String("path"), cookie.path().isEmpty()
             ? QStringLiteral("/") : cookie.path());
    o.insert(QLatin1String("secure"), cookie.isSecure());
    o.insert(QLatin1String("httpOnly"), cookie.isHttpOnly());
    const bool session = cookie.isSessionCookie();
    o.insert(QLatin1String("session"), session);
    if (!session)
        o.insert(QLatin1String("expirationDate"),
                 cookie.expirationDate().toMSecsSinceEpoch() / 1000.0);
    static const char *names[] = {
        "unspecified", "no_restriction", "lax", "strict"};
    o.insert(QLatin1String("sameSite"),
             QLatin1String(names[qBound(0, int(cookie.sameSitePolicy()),
                                      3)]));
    o.insert(QLatin1String("storeId"), storeId);
    return o;
}

#ifdef ARKTOS_RUSTCORE
// Legacy-lane anchor, identical to CookieJar::ensureRustCookies:
// the process-global store must be (re)bound to this profile's
// cookies.db — rc_set_data_dir alone never opens it, so every
// rc_cookies_* call without it errors as "not been called".
void legacyCookiesLane(const QString &profileId)
{
    rustCoreEnsureDataDirFor(profileId);
    rc_cookies_open(nullptr);
}
#endif

// Every default-partition record the mirror holds — facade handle
// first, the legacy rc_* spelling over the per-call anchor second,
// and the C++ jar's live mirror last (the no-rust read path and the
// same FFI-failure fallback the facade consumers keep).
QJsonArray cookieMirrorRows(CookieJar *jar)
{
#ifdef ARKTOS_RUSTCORE
    AcCookies *h = nullptr;
    const QString profileId = jar->profileId();
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->cookies(profileId);
    if (!h)
        legacyCookiesLane(profileId);
    char *raw = h
        ? ac_cookies_list(h, "")
        : rc_cookies_list("");
    ac_cookies_close(h);
    if (raw) {
        const QByteArray out(raw);
        if (h)
            ac_string_free(raw);
        else
            rc_string_free(raw);
        const QJsonArray rows = QJsonDocument::fromJson(out).array();
        if (!rows.isEmpty() || out.trimmed() == "[]")
            return rows;
        // A malformed reply falls through to the live mirror — the
        // C++ jar still carries the engine's truth.
    }
#endif
    QJsonArray rows;
    const QList<QNetworkCookie> cookies = jar->cookies();
    for (const QNetworkCookie &c : cookies)
        rows.append(cookieRecordJson(c));
    return rows;
}

// The send-match records for one url — the RC15 for_url lane (or
// the C++ jar's canonical cookiesForUrl without rustcore), each
// normalized to the record shape the filters run on.
QJsonArray cookieRowsForUrl(CookieJar *jar, const QUrl &url)
{
#ifdef ARKTOS_RUSTCORE
    AcCookies *h = nullptr;
    const QString profileId = jar->profileId();
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->cookies(profileId);
    if (!h)
        legacyCookiesLane(profileId);
    const QByteArray request = QJsonDocument(QJsonObject{
        {QStringLiteral("url"), url.toString()},
        {QStringLiteral("container"), QString()},
        {QStringLiteral("first_party"), QJsonValue()},
        {QStringLiteral("now_ms"), 0}}).toJson(QJsonDocument::Compact);
    char *raw = h
        ? ac_cookies_for_url(
              h, reinterpret_cast<const uint8_t *>(request.constData()),
              size_t(request.size()))
        : rc_cookies_for_url(
              reinterpret_cast<const uint8_t *>(request.constData()),
              size_t(request.size()));
    ac_cookies_close(h);
    if (raw) {
        const QByteArray out(raw);
        if (h)
            ac_string_free(raw);
        else
            rc_string_free(raw);
        return QJsonDocument::fromJson(out).array();
    }
#endif
    QJsonArray rows;
    const QList<QNetworkCookie> cookies = jar->cookiesForUrl(url);
    for (const QNetworkCookie &c : cookies)
        rows.append(cookieRecordJson(c));
    return rows;
}

// Engine-store signal -> chrome.cookies.onChanged broadcast.  Qt's
// cookieAdded/cookieRemoved distinguish neither evictions nor
// expirations nor overwrites — the cause honestly answers
// "explicit" for every change (each backend records the caveat in
// its notes once).
void cookieBroadcast(const QJsonObject &changeInfo)
{
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        QStringLiteral("cookies.onChanged"),
        QJsonObject{{QStringLiteral("__args"),
                     QJsonArray{changeInfo}}});
}

QPointer<CookieJar> s_cookieTapTarget;
QList<QMetaObject::Connection> s_cookieTapConns;

// Lazily binds the ambient jar's engine store signals to the event
// fan-out; re-points when the ambient persona (and therefore the
// jar/store) changes — cookie events must describe the profile the
// extension reads through this namespace.
void tapCookieEvents(CookieJar *jar)
{
    if (s_cookieTapTarget == jar)
        return;
    for (const QMetaObject::Connection &conn : s_cookieTapConns)
        QObject::disconnect(conn);
    s_cookieTapConns.clear();
    s_cookieTapTarget = jar;
    if (!jar || !jar->profile())
        return;
    QWebEngineCookieStore *store = jar->profile()->cookieStore();
    if (!store)
        return;
    const QPointer<QWebEngineCookieStore> weak = store;
    s_cookieTapConns << QObject::connect(
        store, &QWebEngineCookieStore::cookieAdded, store,
        [weak](const QNetworkCookie &cookie) {
            if (!weak)
                return;
            cookieBroadcast(QJsonObject{
                {QStringLiteral("removed"), false},
                {QStringLiteral("cause"), QStringLiteral("explicit")},
                {QStringLiteral("cookie"),
                 chromeCookieJson(cookie, QStringLiteral("0"))}});
        });
    s_cookieTapConns << QObject::connect(
        store, &QWebEngineCookieStore::cookieRemoved, store,
        [weak](const QNetworkCookie &cookie) {
            if (!weak)
                return;
            cookieBroadcast(QJsonObject{
                {QStringLiteral("removed"), true},
                {QStringLiteral("cause"), QStringLiteral("explicit")},
                {QStringLiteral("cookie"),
                 chromeCookieJson(cookie, QStringLiteral("0"))}});
        });
}

// MD13: chrome.windows create/update share this prop application —
// bounds land first (frame-inclusive move, matching Chrome's
// outer-bounds vocabulary), then window state so a maximized or
// minimized request wins over a same-call geometry change, then
// the attention flash drawAttention asks for.
void applyWindowProps(BrowserMainWindow *window,
                      const QJsonObject &props)
{
    if (!window)
        return;
    const bool hasW = props.contains(QStringLiteral("width"));
    const bool hasH = props.contains(QStringLiteral("height"));
    if (hasW || hasH)
        window->resize(
            hasW ? props.value(QStringLiteral("width")).toInt()
                 : window->width(),
            hasH ? props.value(QStringLiteral("height")).toInt()
                 : window->height());
    const bool hasL = props.contains(QStringLiteral("left"));
    const bool hasT = props.contains(QStringLiteral("top"));
    if (hasL || hasT)
        window->move(
            hasL ? props.value(QStringLiteral("left")).toInt()
                 : window->x(),
            hasT ? props.value(QStringLiteral("top")).toInt()
                 : window->y());
    const QString state =
        props.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("minimized"))
        window->showMinimized();
    else if (state == QLatin1String("maximized"))
        window->showMaximized();
    else if (state == QLatin1String("fullscreen"))
        window->showFullScreen();
    else if (state == QLatin1String("normal"))
        window->showNormal();
    if (props.value(QStringLiteral("drawAttention")).toBool())
        QApplication::alert(window);
}

// MD19 — Chrome's published storage quota bounds, measured the way
// Chrome measures them: the key's length plus the JSON
// stringification of the value, per item.
constexpr qint64 kSyncQuotaBytesPerItem = 8192;
constexpr int kSyncMaxItems = 512;

qint64 storageQuotaBytes(const QString &area, bool unlimitedStorage)
{
    if (area == QLatin1String("sync"))
        return 102400;          // sync.QUOTA_BYTES — never lifted
    // BW04: 'unlimitedStorage' lifts the local-area ceiling only —
    // session keeps its MV3 10 MB bound either way.
    if (unlimitedStorage && area == QLatin1String("local"))
        return -1;
    return 10485760;            // local + session (MV3 10 MB)
}

qint64 jsonValueBytes(const QJsonValue &value)
{
    return QJsonDocument(QJsonArray{value})
            .toJson(QJsonDocument::Compact).size() - 2;
}

qint64 storeBytesUsed(const QJsonObject &store)
{
    qint64 total = 0;
    for (auto it = store.constBegin(); it != store.constEnd(); ++it)
        total += it.key().toUtf8().size()
            + jsonValueBytes(it.value());
    return total;
}

// getInfo.windowTypes — every Arktos browser window is "normal",
// so a filter naming only other types matches nothing.
bool windowTypeMatch(const QJsonObject &getInfo)
{
    const QJsonArray types =
        getInfo.value(QStringLiteral("windowTypes")).toArray();
    if (types.isEmpty())
        return true;
    for (const QJsonValue &t : types) {
        if (t.toString() == QLatin1String("normal"))
            return true;
    }
    return false;
}

// -------------------------------------------------------------------
// MD24: chrome.downloads — the machinery over DownloadManager's
// record list.  The engine delivers a download() request
// asynchronously (the profile's downloadRequested signal), so the op
// answers a ticket at invoke time and the real Chrome download id
// arrives via a '__arktos.downloadCreated' push the itemAdded tap
// mints.  Pending tickets queue here, claimed by (url, issuing page)
// — a UI-driven download of the same url can never steal one.
// -------------------------------------------------------------------

int s_dlTicketSeq = 0;

struct DlPending {
    int ticket = 0;
    QString extId;
    QString extName;
    QUrl url;
    QPointer<Engine::Page> issuer;
    QPointer<ExtensionShimBackend> backend;
};

QList<DlPending> &dlPendingList()
{
    static QList<DlPending> list;
    return list;
}

void dlBroadcast(const QString &name, const QJsonArray &eventArgs)
{
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        name, QJsonObject{{QStringLiteral("__args"), eventArgs}});
}

// The deferred downloads.download answer — the ticket reply reaches
// only the calling extension (every 'downloads'-declaring context
// still hears the public onCreated broadcast).  A bare backend has
// no relay context: pushEvent is its only surface — the same
// fallback managementFanOut/bookmarksFanOut apply.
void dlAnswerTicket(const DlPending &pending, const QJsonObject &reply)
{
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->deliverEvent(pending.extId,
        QStringLiteral("__arktos.downloadCreated"), reply);
    if (pending.backend && !runtime->hasRelayContext(pending.extId))
        emit pending.backend->pushEvent(
            QStringLiteral("__arktos.downloadCreated"), reply);
}

// Per-item signal -> downloads.onChanged broadcast.  statusChanged
// and downloadFinished both emit the full current-state delta (the
// card fires them for pause flips, terminal transitions and retries
// alike); progress ticks report byte counters only.  Chrome's
// DownloadDelta values carry {current} — a previous value is only
// included where the card actually recorded one (it never does, so
// the field is honestly omitted rather than fabricated).
void dlAttachItemTap(DownloadItem *item)
{
    if (!item || item->property("arktosDlTapped").toBool())
        return;
    item->setProperty("arktosDlTapped", true);
    // Off-the-record rows stay invisible to the whole namespace —
    // Chrome never surfaces incognito downloads to regular-profile
    // contexts (downloadsItemJson answers them as empty).
    item->setProperty("arktosDlVisible",
                      !ExtensionShimBackend::downloadsDeltaJson(
                          item, false).isEmpty());
    const QPointer<DownloadItem> weak(item);
    const auto fireDelta = [weak](bool finished) {
        if (!weak || !weak->property("arktosDlVisible").toBool())
            return;
        QJsonObject delta =
            ExtensionShimBackend::downloadsDeltaJson(weak, finished);
        delta.insert(QStringLiteral("id"), idFor(weak.data()));
        dlBroadcast(QStringLiteral("downloads.onChanged"),
                    QJsonArray{delta});
    };
    QObject::connect(item, &DownloadItem::statusChanged, item,
                     [fireDelta]() { fireDelta(false); });
    QObject::connect(item, &DownloadItem::downloadFinished, item,
                     [fireDelta]() { fireDelta(true); });
    // pause/resume flip the request's isPaused asynchronously — the
    // statusChanged delta above reads the value mid-apply, so the
    // real pause notification rides the engine's isPausedChanged.
    ExtensionShimBackend::downloadsPauseTap(item);
    QObject::connect(item, &DownloadItem::progress, item, [weak]() {
        if (!weak || !weak->property("arktosDlVisible").toBool())
            return;
        dlBroadcast(QStringLiteral("downloads.onChanged"), QJsonArray{
            QJsonObject{
                {QStringLiteral("id"), idFor(weak.data())},
                {QStringLiteral("bytesReceived"), QJsonObject{
                    {QStringLiteral("current"),
                     double(weak->bytesReceived())}}},
                {QStringLiteral("totalBytes"), QJsonObject{
                    {QStringLiteral("current"),
                     double(weak->bytesTotal())}}}}});
    });
}

QPointer<DownloadManager> s_dlTapTarget;
QList<QMetaObject::Connection> s_dlTapConns;

// Lazily binds the ambient persona's download manager to the event
// fan-out; re-points when the persona (and therefore the record
// list) changes — download events must describe the profile the
// extension reads through this namespace.
void tapDlEvents(DownloadManager *manager)
{
    if (s_dlTapTarget == manager)
        return;
    for (const QMetaObject::Connection &conn : s_dlTapConns)
        QObject::disconnect(conn);
    s_dlTapConns.clear();
    s_dlTapTarget = manager;
    if (!manager)
        return;
    // Rows that predate the tap (restored records, downloads that
    // started before the first downloads.* call) get the delta tap
    // too — state changes on them still deserve onChanged.
    const int rows =
        manager->model() ? manager->model()->rowCount() : 0;
    for (int r = 0; r < rows; ++r)
        dlAttachItemTap(manager->itemAt(r));
    const QPointer<DownloadManager> weak = manager;
    s_dlTapConns << QObject::connect(
        manager, &DownloadManager::itemAdded, manager,
        [weak](DownloadItem *item) {
            if (!weak || !item)
                return;
            dlAttachItemTap(item);
            ExtensionShimBackend::downloadsClaimPending(item);
            const QJsonObject json =
                ExtensionShimBackend::downloadsItemJson(item);
            if (!json.isEmpty())
                dlBroadcast(QStringLiteral("downloads.onCreated"),
                            QJsonArray{json});
        });
    s_dlTapConns << QObject::connect(
        manager->model(), &QAbstractItemModel::rowsAboutToBeRemoved,
        manager, [weak](const QModelIndex &, int first, int last) {
            if (!weak)
                return;
            for (int r = first; r <= last; ++r) {
                DownloadItem *item = weak->itemAt(r);
                if (!item)
                    continue;
                // OTR rows are invisible to the namespace — an empty
                // item json means no onErased either.
                const QJsonObject json =
                    ExtensionShimBackend::downloadsItemJson(item);
                if (!json.isEmpty())
                    dlBroadcast(QStringLiteral("downloads.onErased"),
                                QJsonArray{json.value(QLatin1String("id"))});
            }
        });
}

} // namespace

ExtensionShimBackend::ExtensionShimBackend(const QString &extensionId,
                                           const QString &extensionName,
                                           const QString &extensionPath,
                                           QObject *parent)
    : QObject(parent)
    , m_extensionId(extensionId)
    , m_extensionName(extensionName)
    , m_extensionPath(extensionPath)
{
    // EXT16 registry ownership — this backend is now the authority
    // for the extension's context-menu items and display name.
    menuRegistry().owners.insert(m_extensionId, this);
    menuRegistry().names.insert(m_extensionId, m_extensionName);
}

ExtensionShimBackend::~ExtensionShimBackend()
{
    for (Alarm &alarm : m_alarms) {
        if (alarm.timer)
            alarm.timer->stop();
    }
    if (m_localDirty)
        flushLocalStore();
    if (m_syncDirty)
        flushSyncStore();
    // The items die with their owner — Chrome clears an extension's
    // context menus when its background context ends.  A respawn's
    // replacement backend already claimed the slot: leave it.
    if (menuRegistry().owners.value(m_extensionId) == this) {
        menuRegistry().owners.remove(m_extensionId);
        menuRegistry().items.remove(m_extensionId);
        menuRegistry().names.remove(m_extensionId);
    }
}

ExtensionShimBackend::ActionState
ExtensionShimBackend::actionState(const QString &extensionId)
{
    return actionStates().value(extensionId);
}

int ExtensionShimBackend::tabIdFor(const QObject *view)
{
    return idFor(view);
}

int ExtensionShimBackend::windowIdFor(const QObject *window)
{
    return idFor(window);
}

WebView *ExtensionShimBackend::viewForTabId(int tabId)
{
    if (tabId < 0)
        return nullptr;
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window || !window->tabWidget())
            continue;
        TabWidget *tw = window->tabWidget();
        for (int i = 0; i < tw->count(); ++i) {
            WebView *view = tw->webView(i);
            if (ExtensionEventHub::trackable(view)
                    && idFor(view) == tabId)
                return view;
        }
    }
    return nullptr;
}

QJsonObject ExtensionShimBackend::invoke(const QString &ns,
                                         const QString &method,
                                         const QJsonObject &args)
{
    if (ns == QLatin1String("storage"))
        return invokeStorage(method, args);
    if (ns == QLatin1String("alarms"))
        return invokeAlarms(method, args);
    if (ns == QLatin1String("tabs"))
        return invokeTabs(method, args);
    if (ns == QLatin1String("webNavigation"))
        return invokeWebNavigation(method, args);
    if (ns == QLatin1String("windows"))
        return invokeWindows(method, args);
    if (ns == QLatin1String("action")
            || ns == QLatin1String("browserAction"))
        return invokeAction(method, args);
    if (ns == QLatin1String("i18n"))
        return invokeI18n(method, args);
    if (ns == QLatin1String("permissions"))
        return invokePermissions(method, args);
    if (ns == QLatin1String("contextMenus")
            || ns == QLatin1String("menus"))
        return invokeContextMenus(method, args);
    if (ns == QLatin1String("sidePanel"))
        return invokeSidePanel(method, args);
    if (ns == QLatin1String("notifications"))
        return invokeNotifications(method, args);
    if (ns == QLatin1String("commands"))
        return invokeCommands(method, args);
    if (ns == QLatin1String("idle"))
        return invokeIdle(method, args);
    if (ns == QLatin1String("management"))
        return invokeManagement(method, args);
    if (ns == QLatin1String("bookmarks"))
        return invokeBookmarks(method, args);
    if (ns == QLatin1String("history"))
        return invokeHistory(method, args);
    if (ns == QLatin1String("topSites"))
        return invokeTopSites(method, args);
    if (ns == QLatin1String("browsingData"))
        return invokeBrowsingData(method, args);
    if (ns == QLatin1String("dns"))
        return invokeDns(method, args);
    if (ns == QLatin1String("system"))
        return invokeSystem(method, args);
    if (ns == QLatin1String("privacy"))
        return invokePrivacy(method, args);
    if (ns == QLatin1String("fontSettings"))
        return invokeFontSettings(method, args);
    if (ns == QLatin1String("cookies"))
        return invokeCookies(method, args);
    if (ns == QLatin1String("downloads"))
        return invokeDownloads(method, args);
    if (ns == QLatin1String("sessions"))
        return invokeSessions(method, args);
    if (ns == QLatin1String("tabGroups"))
        return invokeTabGroups(method, args);
    if (ns == QLatin1String("declarativeNetRequest"))
        return invokeDeclarativeNetRequest(method, args);
    if (ns == QLatin1String("identity"))
        return invokeIdentity(method, args);
    if (ns == QLatin1String("extension"))
        return invokeExtension(method, args);
    if (ns == QLatin1String("sidebarAction"))
        return invokeSidebarAction(method, args);
    if (ns == QLatin1String("runtime"))
        return invokeRuntime(method, args);
    if (ns == QLatin1String("shim"))
        return invokeShim(method, args);
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.storage — per-extension host-side store.  'local' and
// 'sync' persist as JSON under <data>/extstorage/<id>/ (QSaveFile
// atomic write); 'sync' roams through the user's account on Chrome —
// Arktos has no sync service, so the honest answer is profile-local
// persistence plus a recorded note (MD19).  'session' is in-memory
// like the real area; 'managed' is a never-written read-only store.
// Mutations answer the WebExtensions change record so the shim can
// fire onChanged.
// -------------------------------------------------------------------

QString ExtensionShimBackend::storageDir() const
{
    // Extension ids are Chromium-derived base16-ish — keep only the
    // safe alphabet anyway, nothing outside gets to name the dir.
    QString safe;
    for (const QChar c : m_extensionId) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_')
                || c == QLatin1Char('-'))
            safe.append(c);
    }
    if (safe.isEmpty())
        safe = QStringLiteral("_");
    return BrowserPaths::dataFilePath(
        QStringLiteral("extstorage")) + QLatin1Char('/') + safe;
}

QJsonObject ExtensionShimBackend::loadStoreFile(const QString &area) const
{
    // The read cap sits just above the 10 MB local-area quota — a
    // legitimately filled store must never refuse to load back.
    // BW04: 'unlimitedStorage' removes the local write cap entirely,
    // so for those extensions the bound is a corruption guard, not
    // a quota — a JSON store past it is torn or forged, not data an
    // honest write path produced.
    const qint64 cap = manifestPermissions().contains(
        QLatin1String("unlimitedStorage"))
        ? (qint64(1) << 30)
        : qint64(12) * 1024 * 1024;
    QFile file(storageDir() + QLatin1Char('/') + area
               + QLatin1String(".json"));
    if (file.size() <= cap && file.open(QIODevice::ReadOnly))
        return QJsonDocument::fromJson(file.readAll()).object();
    if (file.size() > cap)
        const_cast<ExtensionShimBackend *>(this)->recordNote(
            QStringLiteral(
                "chrome.storage.%1: on-disk store exceeds the %2-byte "
                "load bound — treated as corrupt, starting empty")
                .arg(area).arg(cap));
    return QJsonObject();
}

void ExtensionShimBackend::flushStoreFile(const QString &area,
                                          const QJsonObject &store)
{
    QDir dir;
    if (!dir.mkpath(storageDir()))
        return;
    QSaveFile file(storageDir() + QLatin1Char('/') + area
                   + QLatin1String(".json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(store).toJson(QJsonDocument::Compact));
    file.commit();
}

QJsonObject &ExtensionShimBackend::storeFor(const QString &area)
{
    if (area == QLatin1String("local")) {
        if (!m_localLoaded) {
            m_localLoaded = true;
            m_storeLocal = loadStoreFile(QStringLiteral("local"));
        }
        return m_storeLocal;
    }
    if (area == QLatin1String("sync")) {
        if (!m_syncLoaded) {
            m_syncLoaded = true;
            m_storeSync = loadStoreFile(QStringLiteral("sync"));
            recordNote(QStringLiteral(
                "chrome.storage.sync: persisted under extstorage/ "
                "profile-local — Arktos has no cloud sync, the area "
                "does not roam"));
        }
        return m_storeSync;
    }
    if (area == QLatin1String("managed"))
        return m_storeManaged;
    return m_storeSession;
}

void ExtensionShimBackend::persistArea(const QString &area)
{
    if (area == QLatin1String("local")) {
        m_localDirty = true;
        flushLocalStore();
    } else if (area == QLatin1String("sync")) {
        m_syncDirty = true;
        flushSyncStore();
    }
}

void ExtensionShimBackend::flushLocalStore()
{
    if (!m_localLoaded)
        return;
    flushStoreFile(QStringLiteral("local"), m_storeLocal);
    m_localDirty = false;
}

void ExtensionShimBackend::flushSyncStore()
{
    if (!m_syncLoaded)
        return;
    flushStoreFile(QStringLiteral("sync"), m_storeSync);
    m_syncDirty = false;
}

// WebExtensions get() key shapes: null → whole store, string → one
// key, array → subset, object → default-merged subset.
static QStringList getKeysList(const QJsonValue &keys,
                               const QJsonObject &store,
                               QJsonObject *defaults)
{
    QStringList list;
    if (keys.isNull() || keys.isUndefined()) {
        for (auto it = store.constBegin(); it != store.constEnd(); ++it)
            list.append(it.key());
        return list;
    }
    if (keys.isString()) {
        list.append(keys.toString());
        return list;
    }
    if (keys.isArray()) {
        const QJsonArray arr = keys.toArray();
        for (const QJsonValue &v : arr)
            list.append(v.toString());
        return list;
    }
    if (keys.isObject()) {
        const QJsonObject obj = keys.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            list.append(it.key());
            if (defaults)
                defaults->insert(it.key(), it.value());
        }
    }
    return list;
}

QJsonObject ExtensionShimBackend::changesForSet(const QJsonObject &store,
                                              const QJsonObject &items) const
{
    QJsonObject changes;
    for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
        QJsonObject rec;
        if (store.contains(it.key()))
            rec.insert(QStringLiteral("oldValue"), store.value(it.key()));
        rec.insert(QStringLiteral("newValue"), it.value());
        changes.insert(it.key(), rec);
    }
    return changes;
}

QJsonObject ExtensionShimBackend::changesForRemove(
        const QJsonObject &store, const QStringList &keys) const
{
    QJsonObject changes;
    for (const QString &key : keys) {
        if (!store.contains(key))
            continue;
        QJsonObject rec;
        rec.insert(QStringLiteral("oldValue"), store.value(key));
        changes.insert(key, rec);
    }
    return changes;
}

QJsonObject ExtensionShimBackend::invokeStorage(const QString &method,
                                                const QJsonObject &args)
{
    // ER16: Chrome exposes chrome.storage only to extensions
    // declaring 'storage' — the bridge object answers a direct call
    // from any code in the context, so the namespace's absence is
    // enforced here, not in the shim's install list.
    if (!manifestPermissions().contains(QLatin1String("storage")))
        return errReply(QStringLiteral("no-permission"));
    // ER17: 'changed' is the notify op — a shim context that wrote
    // through the REAL engine area (the hosted page keeps it so the
    // extension's store stays shared with real extension pages)
    // reports the change record it computed, and it fans out to the
    // extension's sibling contexts as storage.onChanged +
    // storage.<area>.onChanged.  The writer fires its own copy
    // synchronously in the shim; ChromeBridge::invoke intercepts
    // this op to exclude the posting bridge, leaving only direct
    // backend callers here — page agents are never relay contexts,
    // so an un-excluded fan-out cannot double-deliver.  The engine's
    // own storage delegate never emits either — the fan-out is the
    // only onChanged these pages will ever see.  Same-domain trust:
    // a covered context can already message its own extension's
    // other contexts, so the report needs no re-proof.
    if (method == QLatin1String("changed")) {
        const QString area =
            args.value(QStringLiteral("area")).toString();
        if (area != QLatin1String("local")
                && area != QLatin1String("session")
                && area != QLatin1String("sync"))
            return errReply(QStringLiteral("invalid-area"));
        ExtensionBackgroundRuntime::instance()->fanOutStorageChanged(
            m_extensionId, area,
            args.value(QStringLiteral("changes")).toObject());
        return okReply();
    }
    const int dot = method.indexOf(QLatin1Char('.'));
    const QString area = method.left(dot);
    const QString op = method.mid(dot + 1);
    if (area != QLatin1String("local") && area != QLatin1String("session")
            && area != QLatin1String("sync")
            && area != QLatin1String("managed"))
        return errReply(QStringLiteral("invalid-area"));
    QJsonObject &store = storeFor(area);

    if (op == QLatin1String("get")) {
        QJsonObject defaults;
        const QStringList keys = getKeysList(
            args.value(QStringLiteral("keys")), store, &defaults);
        QJsonObject out = defaults;
        for (const QString &key : keys) {
            if (store.contains(key))
                out.insert(key, store.value(key));
        }
        return okReply(out);
    }
    if (op == QLatin1String("getBytesInUse")) {
        // Approximation: serialized size of the matched entries.
        QJsonObject picked;
        const QStringList keys = getKeysList(
            args.value(QStringLiteral("keys")), store, nullptr);
        for (const QString &key : keys) {
            if (store.contains(key))
                picked.insert(key, store.value(key));
        }
        return okReply(QJsonDocument(picked)
                           .toJson(QJsonDocument::Compact).size());
    }
    if (op == QLatin1String("getKeys")) {
        QJsonArray keys;
        for (auto it = store.constBegin(); it != store.constEnd();
                ++it)
            keys.append(it.key());
        return okReply(keys);
    }
    if (op == QLatin1String("setAccessLevel")) {
        // Chrome takes {accessLevel: AccessLevel}; accept the bare
        // string too — both spellings reach us over the bridge.
        const QJsonValue raw =
            args.value(QStringLiteral("accessLevel"));
        const QString level = raw.isObject()
            ? raw.toObject().value(QStringLiteral("accessLevel"))
                .toString()
            : raw.toString();
        if (level != QLatin1String("TRUSTED_CONTEXTS")
                && level != QLatin1String(
                    "TRUSTED_AND_UNTRUSTED_CONTEXTS"))
            return errReply(QStringLiteral("invalid-argument"));
        if (m_storageAccessLevels.value(area) != level) {
            m_storageAccessLevels.insert(area, level);
            recordNote(QStringLiteral(
                "chrome.storage.%1.setAccessLevel(%2) recorded — "
                "Chrome scopes TRUSTED/UNTRUSTED_CONTEXTS reads on "
                "the session area; the bridge store serves every "
                "context of this extension regardless")
                    .arg(area, level));
        }
        return okReply();
    }
    if (op == QLatin1String("migrated")) {
        // BW04 — the shim's native->bridge merge marker.  A flag
        // file beside the area store records that the one-time
        // merge already ran, so a key the extension deleted is never
        // resurrected off a stale native copy on the next boot.
        // Bare call answers {migrated: bool}; {mark:true} records
        // the merge as done.  Not a Chrome member — an internal
        // protocol op like 'changed' above.
        const QString flag = storageDir() + QLatin1Char('/') + area
            + QLatin1String(".migrated");
        if (args.value(QStringLiteral("mark")).toBool()) {
            if (!QDir().mkpath(storageDir()))
                return errReply(QStringLiteral("io-error"));
            QFile file(flag);
            if (!file.open(QIODevice::WriteOnly))
                return errReply(QStringLiteral("io-error"));
            file.write("1");
            return okReply();
        }
        return okReply(QFile::exists(flag));
    }
    if (op == QLatin1String("set")) {
        if (area == QLatin1String("managed"))
            return errReply(QStringLiteral("read-only"));
        const QJsonObject items =
            args.value(QStringLiteral("items")).toObject();
        // MD19 — Chrome's quota bounds, enforced before the write
        // lands (the call fails atomically like Chrome): sync caps
        // each item at QUOTA_BYTES_PER_ITEM (8192) and the area at
        // MAX_ITEMS (512); every writable area enforces its
        // QUOTA_BYTES ceiling (sync 102400, local/session 10485760).
        if (area == QLatin1String("sync")) {
            for (auto it = items.constBegin(); it != items.constEnd();
                    ++it) {
                if (it.key().toUtf8().size()
                        + jsonValueBytes(it.value())
                    > kSyncQuotaBytesPerItem)
                    return errReply(QStringLiteral("quota-exceeded"));
            }
            int count = store.size();
            for (auto it = items.constBegin(); it != items.constEnd();
                    ++it) {
                if (!store.contains(it.key()))
                    ++count;
            }
            if (count > kSyncMaxItems)
                return errReply(QStringLiteral("quota-exceeded"));
        }
        QJsonObject merged = store;
        for (auto it = items.constBegin(); it != items.constEnd(); ++it)
            merged.insert(it.key(), it.value());
        const qint64 quota = storageQuotaBytes(
            area, manifestPermissions().contains(
                QLatin1String("unlimitedStorage")));
        if (quota >= 0 && storeBytesUsed(merged) > quota)
            return errReply(QStringLiteral("quota-exceeded"));
        QJsonObject result;
        result.insert(QStringLiteral("changes"),
                      changesForSet(store, items));
        store = merged;
        persistArea(area);
        return okReply(result);
    }
    if (op == QLatin1String("remove")) {
        if (area == QLatin1String("managed"))
            return errReply(QStringLiteral("read-only"));
        QStringList keys;
        const QJsonValue k = args.value(QStringLiteral("keys"));
        if (k.isString())
            keys.append(k.toString());
        else {
            const QJsonArray arr = k.toArray();
            for (const QJsonValue &v : arr)
                keys.append(v.toString());
        }
        QJsonObject result;
        result.insert(QStringLiteral("changes"),
                      changesForRemove(store, keys));
        for (const QString &key : keys)
            store.remove(key);
        persistArea(area);
        return okReply(result);
    }
    if (op == QLatin1String("clear")) {
        if (area == QLatin1String("managed"))
            return errReply(QStringLiteral("read-only"));
        QStringList keys;
        for (auto it = store.constBegin(); it != store.constEnd(); ++it)
            keys.append(it.key());
        QJsonObject result;
        result.insert(QStringLiteral("changes"),
                      changesForRemove(store, keys));
        store = QJsonObject();
        persistArea(area);
        return okReply(result);
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.alarms — host QTimers so onAlarm genuinely fires.  Alarm
// objects are {name, scheduledTime, periodInMinutes} per the spec.
// -------------------------------------------------------------------

void ExtensionShimBackend::fireAlarm(const QString &name)
{
    // MD20: membership, not the name — Chrome's default "" alarm
    // fires onAlarm like any named one.
    if (!m_alarms.contains(name))
        return;
    const Alarm alarm = m_alarms.value(name);
    QJsonObject payload;
    payload.insert(QStringLiteral("name"), alarm.name);
    payload.insert(QStringLiteral("scheduledTime"),
                   double(alarm.scheduledTime));
    if (alarm.periodInMinutes > 0)
        payload.insert(QStringLiteral("periodInMinutes"),
                       alarm.periodInMinutes);
    emit pushEvent(QStringLiteral("alarms.onAlarm"), payload);

    if (alarm.periodInMinutes > 0) {
        Alarm &a = m_alarms[name];
        a.scheduledTime = QDateTime::currentMSecsSinceEpoch()
            + qint64(a.periodInMinutes * 60000.0);
        a.timer->setInterval(int(a.periodInMinutes * 60000.0));
        a.timer->start();
    } else {
        if (alarm.timer)
            alarm.timer->deleteLater();
        m_alarms.remove(name);
    }
}

QJsonObject ExtensionShimBackend::alarmJson(
        const ExtensionShimBackend::Alarm &a)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), a.name);
    o.insert(QStringLiteral("scheduledTime"),
             double(a.scheduledTime));
    if (a.periodInMinutes > 0)
        o.insert(QStringLiteral("periodInMinutes"),
                 a.periodInMinutes);
    return o;
}

QJsonObject ExtensionShimBackend::invokeAlarms(const QString &method,
                                               const QJsonObject &args)
{
    // ER16: 'alarms' permission gates the namespace like Chrome.
    if (!manifestPermissions().contains(QLatin1String("alarms")))
        return errReply(QStringLiteral("no-permission"));
    if (method == QLatin1String("create")) {
        const QJsonObject info =
            args.value(QStringLiteral("info")).toObject();
        // MD20 edges, matching Chrome's AlarmsCreateFunction:
        // alarmInfo.name is the object-form name spelling — giving
        // it both ways is a Chrome argument error — and an unnamed
        // alarm lives under "" (Chrome's default name), so a second
        // unnamed create replaces the first.
        const QString argName =
            args.value(QStringLiteral("name")).toString();
        const QJsonValue infoName =
            info.value(QStringLiteral("name"));
        if (!argName.isEmpty() && infoName.isString())
            return errReply(QStringLiteral("invalid-argument"));
        const QString name = argName.isEmpty()
            ? infoName.toString()
            : argName;
        // Chrome refuses a create that both fixes and offsets the
        // first fire — or that specifies no schedule at all.
        const bool hasWhen = info.contains(QStringLiteral("when"));
        const bool hasDelay =
            info.contains(QStringLiteral("delayInMinutes"));
        const bool hasPeriod =
            info.contains(QStringLiteral("periodInMinutes"));
        if ((hasWhen && hasDelay)
                || (!hasWhen && !hasDelay && !hasPeriod))
            return errReply(QStringLiteral("invalid-argument"));
        Alarm alarm;
        alarm.name = name;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (hasWhen) {
            alarm.scheduledTime =
                qint64(info.value(QStringLiteral("when")).toDouble());
        } else {
            // delayInMinutes — or, with neither first-fire spelling
            // given, periodInMinutes doubles as the delay (Chrome's
            // documented fallback).  The reported schedule stays the
            // requested one; the timer interval keeps its own floor.
            const double delayMin = info.value(
                hasDelay ? QStringLiteral("delayInMinutes")
                         : QStringLiteral("periodInMinutes"))
                    .toDouble(0.0);
            alarm.scheduledTime =
                now + qint64(qMax(0.0, delayMin) * 60000.0);
        }
        // MD20: Chrome's MV3 floor — a repeating period under 30
        // seconds would spin the timer, so clamp to the floor and
        // record the note Chrome writes to the console.  One-shot
        // delays keep their requested schedule: hosted contexts run
        // like Chrome's unpacked extensions, where the dev minimum
        // lifts the first-fire clamp.
        double period =
            info.value(QStringLiteral("periodInMinutes"))
                .toDouble(0.0);
        if (period > 0.0 && period < 0.5) {
            recordNote(QStringLiteral(
                "alarms.create: periodInMinutes %1 for '%2' below "
                "Chrome's 30-second MV3 floor — clamped to 0.5")
                .arg(period).arg(name));
            period = 0.5;
        }
        alarm.periodInMinutes = period;
        alarm.order = ++m_alarmSeq;
        alarm.timer = new QTimer(this);
        alarm.timer->setSingleShot(alarm.periodInMinutes <= 0);
        alarm.timer->setInterval(
            int(qMax(qint64(100), alarm.scheduledTime - now)));
        connect(alarm.timer, &QTimer::timeout, this,
                [this, name]() { fireAlarm(name); });
        alarm.timer->start();
        if (Alarm old = m_alarms.value(name); old.timer)
            old.timer->deleteLater();
        m_alarms.insert(name, alarm);
        return okReply();
    }
    if (method == QLatin1String("get")) {
        const QString name =
            args.value(QStringLiteral("name")).toString();
        // MD20: the "" default name is a real key — membership, not
        // an empty name, decides a miss.
        return okReply(m_alarms.contains(name)
                ? alarmJson(m_alarms.value(name))
                : QJsonValue(QJsonValue::Undefined));
    }
    if (method == QLatin1String("getAll")) {
        // MD20: Chrome iterates the per-extension alarm list in
        // creation order (a replaced alarm re-appends at the end);
        // sort by the sequence minted at create — QHash order is
        // nondeterministic.
        QList<Alarm> sorted = m_alarms.values();
        std::sort(sorted.begin(), sorted.end(),
                  [](const Alarm &a, const Alarm &b) {
                      return a.order < b.order;
                  });
        QJsonArray all;
        for (const Alarm &a : sorted)
            all.append(alarmJson(a));
        return okReply(all);
    }
    if (method == QLatin1String("clear")) {
        const QString name =
            args.value(QStringLiteral("name")).toString();
        // MD20: clearing the "" unnamed alarm answers true like
        // Chrome — the empty name is a valid key.
        const bool had = m_alarms.contains(name);
        const Alarm a = m_alarms.take(name);
        if (a.timer)
            a.timer->deleteLater();
        return okReply(had);
    }
    if (method == QLatin1String("clearAll")) {
        for (const Alarm &a : m_alarms) {
            if (a.timer)
                a.timer->deleteLater();
        }
        const bool had = !m_alarms.isEmpty();
        m_alarms.clear();
        return okReply(had);
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.tabs — live snapshot over the app's windows/tab widgets.
// Event DELIVERY (onUpdated etc.) is EXT16's job; this answers the
// query surface the 27-entry delta measured.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::tabObject(WebView *view, int index,
                                            int windowId)
{
    QJsonObject tab;
    tab.insert(QStringLiteral("id"), idFor(view));
    tab.insert(QStringLiteral("index"), index);
    tab.insert(QStringLiteral("windowId"), windowId);
    QWebEnginePage *page = view ? view->page() : nullptr;
    TabWidget *tw = view ? view->tabWidget() : nullptr;
    Engine::Page *engine = view ? view->enginePage() : nullptr;
    const bool active = view && tw
        && tw->currentWebView() == view;
    tab.insert(QStringLiteral("active"), active);
    // The strip has no multi-select, so Chrome's highlighted set
    // collapses to the one active tab.
    tab.insert(QStringLiteral("highlighted"), active);
    tab.insert(QStringLiteral("selected"), active);
    // MD10 honesty — real strip/page state instead of constants:
    // pinned and group membership come off the TabWidget records,
    // discarded maps the SLEEP01 sleeping state (Chrome's name for
    // the same renderer release), audible/muted come off the live
    // page, and autoDiscardable reads the flag tabs.update's setter
    // plants on the view (Chrome's default is true).
    tab.insert(QStringLiteral("pinned"),
               tw && tw->isTabPinned(index));
    tab.insert(QStringLiteral("audible"),
               engine && engine->recentlyAudible());
    tab.insert(QStringLiteral("discarded"),
               (tw && tw->isTabSleeping(index))
                   || (engine && engine->lifecycleState()
                           == Engine::Page::LifecycleState::Discarded));
    const QVariant autoDiscard =
        view ? view->property("arktosExtAutoDiscardable") : QVariant();
    tab.insert(QStringLiteral("autoDiscardable"),
               !autoDiscard.isValid() || autoDiscard.toBool());
    tab.insert(QStringLiteral("groupId"),
               tw ? chromeGroupId(windowId, tw->tabGroupId(index))
                  : -1);
    tab.insert(QStringLiteral("incognito"),
               page && page->profile()
                   && page->profile()->isOffTheRecord());
    QJsonObject muted;
    muted.insert(QStringLiteral("muted"),
                 page && page->isAudioMuted());
    tab.insert(QStringLiteral("mutedInfo"), muted);
    tab.insert(QStringLiteral("url"),
               view ? view->url().toString() : QString());
    tab.insert(QStringLiteral("title"),
               page ? page->title() : QString());
    tab.insert(QStringLiteral("status"),
               view && view->isLoading()
                   ? QStringLiteral("loading")
                   : QStringLiteral("complete"));
    return tab;
}

QJsonObject ExtensionShimBackend::shapedTab(const QJsonObject &tab) const
{
    if (ExtensionBackgroundRuntime::instance()->seesUrls(
            m_extensionId,
            tab.value(QStringLiteral("url")).toString()))
        return tab;
    QJsonObject out = tab;
    out.remove(QStringLiteral("url"));
    out.remove(QStringLiteral("title"));
    out.remove(QStringLiteral("favIconUrl"));
    out.remove(QStringLiteral("pendingUrl"));
    return out;
}

QJsonObject ExtensionShimBackend::invokeTabs(const QString &method,
                                             const QJsonObject &args)
{
    // EXT16 parity: the query surface describes the same tab set
    // the event hub tracks — every top-level window, minus tabs on
    // profiles extensions may not see (OTR, tor).
    const QList<BrowserMainWindow *> windows =
        ExtensionEventHub::browsingWindows();

    if (method == QLatin1String("query")) {
        const QJsonObject q =
            args.value(QStringLiteral("query")).toObject();
        BrowserMainWindow *current = currentWindow();
        const int currentWid = idFor(current);
        QJsonArray out;
        for (BrowserMainWindow *window : windows) {
            if (!window)
                continue;
            const int wid = idFor(window);
            // WINDOW_ID_CURRENT (-2) resolves to the live window;
            // WINDOW_ID_NONE (-1) matches only window-less tabs —
            // the engine has none, so it filters everything.
            if (q.contains(QStringLiteral("windowId"))) {
                int wanted = int(q.value(QStringLiteral("windowId"))
                                     .toDouble(-2));
                if (wanted == -2)
                    wanted = currentWid;
                if (wanted != wid)
                    continue;
            }
            if (q.value(QStringLiteral("currentWindow")).toBool()
                    && wid != currentWid)
                continue;
            if (q.value(QStringLiteral("lastFocusedWindow")).toBool()
                    && wid != currentWid)
                continue;
            // Window-scoped filter — every Arktos browser window is
            // "normal"; a query for popup/panel/app/devtools matches
            // nothing rather than mislabeling the surface.
            if (q.contains(QStringLiteral("windowType"))
                    && q.value(QStringLiteral("windowType")).toString()
                           != QLatin1String("normal"))
                continue;
            TabWidget *tw = window->tabWidget();
            if (!tw)
                continue;
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                // Engine/widget tabs have no WebView; OTR and tor
                // pages are invisible to extensions (EXT16 parity).
                if (!ExtensionEventHub::trackable(view))
                    continue;
                QJsonObject tab = tabObject(view, i, wid);
                // Boolean state filters — Chrome's query keys map
                // one-to-one onto the tab object fields now that they
                // carry real state ('muted' reads mutedInfo.muted).
                const auto boolFilter = [&q, &tab](
                        const QString &key, const QJsonValue &actual) {
                    return !q.contains(key)
                        || q.value(key).toBool() == actual.toBool();
                };
                if (!boolFilter(QStringLiteral("active"),
                                tab.value(QStringLiteral("active")))
                        || !boolFilter(QStringLiteral("highlighted"),
                                tab.value(QStringLiteral("highlighted")))
                        || !boolFilter(QStringLiteral("pinned"),
                                tab.value(QStringLiteral("pinned")))
                        || !boolFilter(QStringLiteral("audible"),
                                tab.value(QStringLiteral("audible")))
                        || !boolFilter(QStringLiteral("discarded"),
                                tab.value(QStringLiteral("discarded")))
                        || !boolFilter(QStringLiteral("autoDiscardable"),
                                tab.value(QStringLiteral("autoDiscardable")))
                        || !boolFilter(QStringLiteral("muted"),
                                tab.value(QStringLiteral("mutedInfo"))
                                    .toObject()
                                    .value(QStringLiteral("muted"))))
                    continue;
                if (q.contains(QStringLiteral("groupId"))
                        && int(q.value(QStringLiteral("groupId"))
                                   .toDouble(-1))
                               != tab.value(QStringLiteral("groupId"))
                                      .toInt(-2))
                    continue;
                if (q.contains(QStringLiteral("index"))
                        && int(q.value(QStringLiteral("index"))
                                   .toDouble(-1)) != i)
                    continue;
                if (q.contains(QStringLiteral("status"))
                        && q.value(QStringLiteral("status")).toString()
                               != tab.value(QStringLiteral("status"))
                                      .toString())
                    continue;
                if (q.contains(QStringLiteral("title"))) {
                    const QString needle =
                        q.value(QStringLiteral("title")).toString();
                    if (!tab.value(QStringLiteral("title")).toString()
                            .contains(needle, Qt::CaseInsensitive))
                        continue;
                }
                if (q.contains(QStringLiteral("url"))) {
                    // Url match patterns are a Chrome syntax — do the
                    // honest subset: substring match (array = OR).
                    const QString url =
                        tab.value(QStringLiteral("url")).toString();
                    const QJsonValue patterns =
                        q.value(QStringLiteral("url"));
                    QStringList needles;
                    if (patterns.isString())
                        needles.append(patterns.toString());
                    else {
                        for (const QJsonValue &v : patterns.toArray())
                            needles.append(v.toString());
                    }
                    bool urlOk = needles.isEmpty();
                    for (const QString &needle : needles) {
                        // '*://*.host/*' style patterns collapse to
                        // their significant literal for the contains
                        // check; exact urls keep full fidelity.
                        QString lit = needle;
                        lit.remove(QLatin1Char('*'));
                        lit.remove(QStringLiteral("://"));
                        if (lit.isEmpty()
                                || url.contains(lit,
                                                Qt::CaseInsensitive))
                            urlOk = true;
                    }
                    if (!urlOk)
                        continue;
                }
                out.append(shapedTab(tab));
            }
        }
        return okReply(out);
    }
    if (method == QLatin1String("get")) {
        const int wanted =
            int(args.value(QStringLiteral("id")).toDouble(-1));
        for (BrowserMainWindow *window : windows) {
            if (!window)
                continue;
            TabWidget *tw = window->tabWidget();
            if (!tw)
                continue;
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (ExtensionEventHub::trackable(view)
                        && idFor(view) == wanted)
                    return okReply(shapedTab(
                        tabObject(view, i, idFor(window))));
            }
        }
        return errReply(QStringLiteral("not-found"));
    }
    if (method == QLatin1String("create")) {
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        // ER16: scheme-scoped navigation gate — an extension may put
        // a real tab only on url classes Chrome accepts from it
        // (web schemes, data:, about:blank, or its OWN extension
        // documents; relative spellings resolve there too).  Checked
        // before any window/tab is touched.
        const QString urlText =
            props.value(QStringLiteral("url")).toString();
        const QUrl url = extensionNavUrl(urlText);
        if (!urlText.isEmpty() && !url.isValid())
            return errReply(QStringLiteral("url-refused"));
        BrowserMainWindow *window = currentWindow();
        if (props.contains(QStringLiteral("windowId"))) {
            int wanted = int(
                props.value(QStringLiteral("windowId")).toDouble(-2));
            // WINDOW_ID_CURRENT names the live window, like query.
            if (wanted == -2)
                wanted = idFor(window);
            window = nullptr;
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    window = w;
                    break;
                }
            }
            // An explicit windowId that resolves nowhere is a typed
            // failure — Chrome answers "No window with id", we answer
            // no-window; it must NOT silently create one.
            if (!window)
                return errReply(QStringLiteral("no-window"));
        }
        if (!window) {
            // Chrome parity: with no window to host the tab,
            // tabs.create opens one — the same fallback
            // invokeWindows create() already uses.  A refused
            // creation (vault-locked persona) keeps no-window.
            BrowserApplication *app = BrowserApplication::instance();
            window = app ? app->newMainWindow() : nullptr;
        }
        if (!window || !window->tabWidget())
            return errReply(QStringLiteral("no-window"));
        const bool active = !props.contains(QStringLiteral("active"))
            || props.value(QStringLiteral("active")).toBool();
        WebView *view = window->tabWidget()->makeNewTab(active);
        if (!view)
            return errReply(QStringLiteral("no-tab"));
        if (!url.isEmpty())
            view->loadUrl(url);
        const int index =
            window->tabWidget()->webViewIndex(view);
        return okReply(shapedTab(
            tabObject(view, index, idFor(window))));
    }
    if (method == QLatin1String("update")) {
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        int wanted = int(args.value(QStringLiteral("id"))
                             .toDouble(-1));
        WebView *target = nullptr;
        BrowserMainWindow *targetWindow = nullptr;
        int targetIndex = -1;
        for (BrowserMainWindow *window : windows) {
            if (!window || !window->tabWidget())
                continue;
            TabWidget *tw = window->tabWidget();
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (!ExtensionEventHub::trackable(view))
                    continue;
                if (wanted < 0) {
                    // No id -> the active tab of the current window.
                    if (window == currentWindow()
                            && i == tw->currentIndex()) {
                        target = view;
                        targetWindow = window;
                        targetIndex = i;
                    }
                } else if (idFor(view) == wanted) {
                    target = view;
                    targetWindow = window;
                    targetIndex = i;
                }
            }
        }
        if (!target)
            return errReply(QStringLiteral("not-found"));
        if (props.contains(QStringLiteral("url"))) {
            // ER16: same navigation gate create() applies.
            const QString urlText =
                props.value(QStringLiteral("url")).toString();
            const QUrl url = extensionNavUrl(urlText);
            if (!urlText.isEmpty() && !url.isValid())
                return errReply(QStringLiteral("url-refused"));
            if (!url.isEmpty())
                target->loadUrl(url);
        }
        if (props.contains(QStringLiteral("pinned")))
            targetWindow->tabWidget()->setTabPinned(
                targetIndex,
                props.value(QStringLiteral("pinned")).toBool());
        if (props.contains(QStringLiteral("muted"))
                && target->page())
            target->page()->setAudioMuted(
                props.value(QStringLiteral("muted")).toBool());
        if (props.contains(QStringLiteral("autoDiscardable"))) {
            const bool on =
                props.value(QStringLiteral("autoDiscardable")).toBool();
            target->setProperty("arktosExtAutoDiscardable", on);
            // Chrome: autoDiscardable:false un-discards the tab.
            if (!on)
                targetWindow->tabWidget()->wakeTab(targetIndex);
        }
        if (props.value(QStringLiteral("active")).toBool())
            targetWindow->tabWidget()->setCurrentIndex(targetIndex);
        // setTabPinned slides the tab to the pinned-block edge — the
        // captured index is stale once a pin/unpin landed.
        const int replyIndex =
            targetWindow->tabWidget()->webViewIndex(target);
        return okReply(shapedTab(
            tabObject(target, replyIndex, idFor(targetWindow))));
    }
    if (method == QLatin1String("remove")) {
        // Chrome accepts one id or an array — resolve the whole set
        // first so a bogus id refuses the batch before any tab dies.
        // A missing/null argument is a call error, never "the
        // current tab" (unlike the optional-id ops below).
        const QJsonValue idv = args.value(QStringLiteral("ids"));
        if (!idv.isDouble() && !idv.isArray())
            return errReply(QStringLiteral("invalid-argument"));
        QList<int> ids;
        if (idv.isArray()) {
            const QJsonArray arr = idv.toArray();
            for (const QJsonValue &v : arr)
                ids.append(int(v.toDouble(-1)));
        } else {
            ids.append(int(idv.toDouble(-1)));
        }
        QList<LocatedTab> targets;
        for (const int id : ids) {
            const LocatedTab t = id > 0 ? locateTab(id) : LocatedTab();
            if (!t.view)
                return errReply(QStringLiteral("not-found"));
            targets.append(t);
        }
        for (const LocatedTab &t : targets) {
            // Strip indexes shift as earlier closes land — re-resolve
            // each view's live position right before its close.
            const int at = t.tabs->webViewIndex(t.view);
            if (at >= 0)
                t.tabs->closeTab(at);
        }
        return okReply();
    }
    if (method == QLatin1String("reload")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        if (t.tabs->isTabSleeping(t.index)) {
            // A discarded page has no live document to reload —
            // waking rebuilds the WebContents on the current entry,
            // which IS the reload Chrome performs there.
            t.tabs->wakeTab(t.index);
        } else if (args.value(QStringLiteral("bypassCache")).toBool()
                   && t.view->enginePage()) {
            QAction *bypass = t.view->enginePage()->action(
                Engine::StandardAction::ReloadAndBypassCache);
            if (bypass)
                bypass->trigger();
            else
                t.tabs->reloadTab(t.index);
        } else {
            t.tabs->reloadTab(t.index);
        }
        return okReply();
    }
    if (method == QLatin1String("duplicate")) {
        const QJsonValue idv = args.value(QStringLiteral("id"));
        if (!idv.isDouble())
            return errReply(QStringLiteral("invalid-argument"));
        const LocatedTab t = locateTab(int(idv.toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        // cloneTab() keeps the copy in the source's group and private
        // context but appends it — Chrome lands the duplicate right
        // after the source, so slide it there.
        QSet<WebView *> before;
        for (int i = 0; i < t.tabs->count(); ++i)
            before.insert(t.tabs->webView(i));
        t.tabs->cloneTab(t.index);
        WebView *copy = nullptr;
        for (int i = 0; i < t.tabs->count(); ++i) {
            WebView *v = t.tabs->webView(i);
            if (v && !before.contains(v)) {
                copy = v;
                break;
            }
        }
        if (!copy)
            return errReply(QStringLiteral("no-tab"));
        const int at = t.tabs->webViewIndex(copy);
        if (at > t.index + 1)
            t.tabs->tabBar()->moveTab(at, t.index + 1);
        return okReply(shapedTab(tabObject(
            copy, t.tabs->webViewIndex(copy), t.windowId)));
    }
    if (method == QLatin1String("highlight")) {
        // Chrome takes tab INDEXES inside a window, not ids — and
        // multi-selects the rest.  The strip has no multi-select, so
        // the first listed tab activates and the reply reports the
        // one highlight that actually landed.
        int wanted = int(
            args.value(QStringLiteral("windowId")).toDouble(-2));
        BrowserMainWindow *target = nullptr;
        if (wanted == -2) {
            target = currentWindow();
        } else {
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    target = w;
                    break;
                }
            }
        }
        if (!target || !target->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *tw = target->tabWidget();
        const QJsonValue tv = args.value(QStringLiteral("indexes"));
        if (!tv.isDouble() && !tv.isArray())
            return errReply(QStringLiteral("invalid-argument"));
        QList<int> indexes;
        if (tv.isArray()) {
            const QJsonArray arr = tv.toArray();
            for (const QJsonValue &v : arr)
                indexes.append(int(v.toDouble(-1)));
        } else {
            indexes.append(int(tv.toDouble(-1)));
        }
        int activated = -1;
        for (const int i : indexes) {
            if (i >= 0 && i < tw->count()
                    && ExtensionEventHub::trackable(tw->webView(i))) {
                tw->setCurrentIndex(i);
                activated = i;
                break;
            }
        }
        if (activated < 0)
            return errReply(QStringLiteral("not-found"));
        QJsonObject win = windowObject(target);
        win.insert(QStringLiteral("tabs"),
                   QJsonArray{shapedTab(tabObject(
                       tw->webView(activated), activated,
                       idFor(target)))});
        return okReply(win);
    }
    if (method == QLatin1String("move")) {
        const QJsonValue idv = args.value(QStringLiteral("ids"));
        if (!idv.isDouble() && !idv.isArray())
            return errReply(QStringLiteral("invalid-argument"));
        QList<int> ids;
        if (idv.isArray()) {
            const QJsonArray arr = idv.toArray();
            for (const QJsonValue &v : arr)
                ids.append(int(v.toDouble(-1)));
        } else {
            ids.append(int(idv.toDouble(-1)));
        }
        if (ids.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        const int to =
            int(args.value(QStringLiteral("index")).toDouble(-1));
        int wanted = int(
            args.value(QStringLiteral("windowId")).toDouble(-2));
        BrowserMainWindow *target = nullptr;
        if (wanted == -2) {
            // No windowId: same-strip reorder — the destination is
            // the moved tab's own window.
            const LocatedTab first = locateTab(ids.first());
            if (!first.view)
                return errReply(QStringLiteral("not-found"));
            target = first.window;
        } else {
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    target = w;
                    break;
                }
            }
            if (!target)
                return errReply(QStringLiteral("no-window"));
        }
        if (!target || !target->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *dst = target->tabWidget();
        QJsonArray moved;
        for (const int id : ids) {
            // Re-resolve per tab — earlier moves shift indexes.
            const LocatedTab t =
                id > 0 ? locateTab(id) : LocatedTab();
            if (!t.view)
                return errReply(QStringLiteral("not-found"));
            const int at = t.tabs->webViewIndex(t.view);
            if (at < 0)
                return errReply(QStringLiteral("not-found"));
            const int pos = moved.size();
            if (t.tabs == dst) {
                // Same-strip reorder — the bar's moveTab rides the
                // tabMoved bookkeeping (location bars, group
                // normalize, pin-boundary conversion).
                const int dstIndex = to < 0
                    ? dst->count() - 1
                    : qBound(0, to + pos, dst->count() - 1);
                if (dstIndex != at)
                    t.tabs->tabBar()->moveTab(at, dstIndex);
            } else {
                const int dstIndex =
                    to < 0 ? -1 : to + pos;
                if (!t.tabs->moveTabTo(at, dst, dstIndex))
                    return errReply(QStringLiteral("move-refused"));
            }
            // The reply reports the landing position — for a
            // cross-window move the view's strip changed entirely.
            TabWidget *landed = t.view->tabWidget();
            const int landedAt =
                landed ? landed->webViewIndex(t.view) : -1;
            moved.append(shapedTab(tabObject(
                t.view, landedAt, idFor(target))));
        }
        return okReply(ids.size() == 1
            ? moved.at(0) : QJsonValue(moved));
    }
    if (method == QLatin1String("group")
            || method == QLatin1String("ungroup")) {
        // MD26 — Chrome puts group membership ops under chrome.tabs:
        // group() is the API's only group-CREATE path (its
        // chrome.tabGroups surface has none), ungroup() drops the
        // listed tabs out of their respective groups — an emptied
        // group dies like Chrome's does.
        const QJsonObject opts =
            args.value(QStringLiteral("options")).toObject();
        QList<int> ids;
        const QJsonValue multi = opts.value(QStringLiteral("tabIds"));
        const QJsonValue single = opts.value(QStringLiteral("tabId"));
        bool badId = false;
        if (multi.isArray()) {
            for (const QJsonValue &v : multi.toArray()) {
                if (!v.isDouble()) {
                    badId = true;
                    break;
                }
                ids.append(int(v.toDouble(-1)));
            }
        } else if (multi.isDouble()) {
            ids.append(int(multi.toDouble(-1)));
        }
        if (single.isDouble())
            ids.append(int(single.toDouble(-1)));
        else if (single.isArray() || single.isObject())
            badId = true;
        if (badId || ids.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        // Resolve every tab first — Chrome's call is atomic, a dead
        // id must not leave a half-grouped strip.
        QList<LocatedTab> located;
        for (const int id : ids) {
            const LocatedTab t = locateTab(id);
            if (!t.view)
                return errReply(QStringLiteral("not-found"));
            located.append(t);
        }

        if (method == QLatin1String("ungroup")) {
            for (const LocatedTab &t : located) {
                const int at = t.tabs->webViewIndex(t.view);
                if (at >= 0)
                    t.tabs->removeTabFromGroup(at);
            }
            return okReply();
        }

        // group(): the destination window is the named group's, or
        // createProperties.windowId, else the first tab's own — and
        // every listed tab must live there (a cross-window member
        // would tear the run).
        BrowserMainWindow *target = nullptr;
        QString joinGid;
        if (opts.contains(QStringLiteral("groupId"))) {
            const QJsonValue gv = opts.value(QStringLiteral("groupId"));
            if (!gv.isDouble())
                return errReply(QStringLiteral("invalid-argument"));
            const int cid = int(gv.toDouble(-1));
            // TAB_GROUP_ID_NONE (-1) asks for a fresh group.
            if (cid != -1
                    && !resolveTabGroup(cid, &target, &joinGid))
                return errReply(QStringLiteral("not-found"));
        }
        if (!target) {
            const QJsonObject create =
                opts.value(QStringLiteral("createProperties"))
                    .toObject();
            if (create.contains(QStringLiteral("windowId"))) {
                int wanted = int(
                    create.value(QStringLiteral("windowId"))
                        .toDouble(-2));
                if (wanted == -2)
                    wanted = idFor(currentWindow());
                for (BrowserMainWindow *w : windows) {
                    if (w && idFor(w) == wanted) {
                        target = w;
                        break;
                    }
                }
                if (!target)
                    return errReply(QStringLiteral("no-window"));
            }
            if (!target)
                target = located.first().window;
        }
        TabWidget *tw = target->tabWidget();
        if (!tw)
            return errReply(QStringLiteral("no-window"));
        for (const LocatedTab &t : located) {
            if (t.window != target || t.tabs != tw)
                return errReply(QStringLiteral("invalid-argument"));
        }
        if (joinGid.isEmpty()) {
            // createTabGroup() binds the first tab and mints the id;
            // addTabToGroup() joins the rest, sliding each into the
            // run like drop-stacking does.
            joinGid = tw->createTabGroup(
                tw->webViewIndex(located.first().view));
            if (joinGid.isEmpty())
                return errReply(QStringLiteral("group-refused"));
            for (int k = 1; k < located.size(); ++k) {
                const int at = tw->webViewIndex(located.at(k).view);
                if (at >= 0)
                    tw->addTabToGroup(at, joinGid);
            }
        } else {
            for (const LocatedTab &t : located) {
                if (tw->tabGroupOf(t.view) == joinGid)
                    continue;   // already a member — idempotent
                const int at = tw->webViewIndex(t.view);
                if (at >= 0)
                    tw->addTabToGroup(at, joinGid);
            }
        }
        return okReply(QJsonObject{
            {QStringLiteral("groupId"),
             chromeGroupId(idFor(target), joinGid)}});
    }
    if (method == QLatin1String("goBack")
            || method == QLatin1String("goForward")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        Engine::Page *page = t.view ? t.view->enginePage() : nullptr;
        if (!page)
            return errReply(QStringLiteral("not-found"));
        const bool back = method == QLatin1String("goBack");
        if (back ? !page->canGoBack() : !page->canGoForward())
            return errReply(QStringLiteral("no-history"));
        if (back)
            page->back();
        else
            page->forward();
        return okReply();
    }
    if (method == QLatin1String("discard")) {
        const QJsonValue idv = args.value(QStringLiteral("id"));
        if (idv.isUndefined() || idv.isNull()) {
            // No id — Chrome picks the least-important discardable
            // tab.  The honest subset: the first background tab the
            // sleep path would actually take (the current tab, empty
            // pages and sleep-in-flight entries are never taken).
            BrowserMainWindow *current = currentWindow();
            TabWidget *tw = current ? current->tabWidget() : nullptr;
            if (!tw)
                return okReply(QJsonValue(QJsonValue::Null));
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (i == tw->currentIndex()
                        || !ExtensionEventHub::trackable(view)
                        || tw->isTabSleeping(i)
                        || view->url().isEmpty())
                    continue;
                tw->sleepTab(i);
                return okReply(shapedTab(
                    tabObject(view, i, idFor(current))));
            }
            return okReply(QJsonValue(QJsonValue::Null));
        }
        const LocatedTab t = locateTab(int(idv.toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        // An already-discarded tab answers its state again (Chrome
        // returns the tab); one the sleep path cannot take — the
        // active tab, an empty page — answers Chrome's undefined.
        if (t.tabs->isTabSleeping(t.index))
            return okReply(shapedTab(
                tabObject(t.view, t.index, t.windowId)));
        if (t.index == t.tabs->currentIndex()
                || t.view->url().isEmpty())
            return okReply(QJsonValue(QJsonValue::Null));
        t.tabs->sleepTab(t.index);
        // The discard commits after the async scroll/form capture —
        // the reply's tab reflects the request landing, and a dirty
        // form can still veto the drop itself.
        return okReply(shapedTab(
            tabObject(t.view, t.index, t.windowId)));
    }
    // MD11 — zoom surface.  get/setZoom ride the view's own zoom
    // machinery (setTabZoomFactor keeps m_currentZoom, the zoom
    // readout and reload reapplication coherent); zoomSettings are
    // a recorded model like sidePanel's — mode/scope store on the
    // tab, 'disabled' is enforced on this surface, and requests the
    // engine cannot honor (per-origin scoping, navigation-reset
    // 'automatic') land fidelity notes.
    if (method == QLatin1String("getZoom")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        return okReply(qreal(t.view->currentZoom()) / 100.0);
    }
    if (method == QLatin1String("setZoom")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        const QJsonObject settings = zoomSettingsFor(t.view);
        if (settings.value(QStringLiteral("mode")).toString()
                == QLatin1String("disabled"))
            return errReply(QStringLiteral("zoom-disabled"));
        const double zoom =
            args.value(QStringLiteral("zoom")).toDouble(-1.0);
        if (zoom <= 0.0)
            return errReply(QStringLiteral("invalid-argument"));
        const double before = qreal(t.view->currentZoom()) / 100.0;
        const double applied = t.view->setTabZoomFactor(zoom);
        // Chrome fires tabs.onZoomChange on every zoom commit —
        // push it with the factors and the tab's settings shape.
        emit pushEvent(QStringLiteral("tabs.onZoomChange"), QJsonObject{
            {QStringLiteral("tabId"), idFor(t.view)},
            {QStringLiteral("oldZoomFactor"), before},
            {QStringLiteral("newZoomFactor"), applied},
            {QStringLiteral("zoomSettings"), settings}});
        return okReply();
    }
    if (method == QLatin1String("getZoomSettings")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        return okReply(zoomSettingsFor(t.view));
    }
    if (method == QLatin1String("setZoomSettings")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        const QJsonObject req =
            args.value(QStringLiteral("settings")).toObject();
        QJsonObject recorded =
            t.view->property("arktosExtZoomSettings").toJsonObject();
        const QString mode =
            req.value(QStringLiteral("mode")).toString();
        if (!mode.isEmpty()) {
            if (mode != QLatin1String("automatic")
                    && mode != QLatin1String("manual")
                    && mode != QLatin1String("disabled"))
                return errReply(QStringLiteral("invalid-argument"));
            recorded.insert(QStringLiteral("mode"), mode);
            if (mode == QLatin1String("automatic"))
                recordNote(QStringLiteral(
                    "tabs.setZoomSettings: 'automatic' recorded — "
                    "Arktos tab zoom persists across navigation "
                    "(manual semantics)"));
        }
        const QString scope =
            req.value(QStringLiteral("scope")).toString();
        if (!scope.isEmpty()) {
            if (scope != QLatin1String("per-origin")
                    && scope != QLatin1String("per-tab"))
                return errReply(QStringLiteral("invalid-argument"));
            recorded.insert(QStringLiteral("scope"), scope);
            if (scope == QLatin1String("per-origin"))
                recordNote(QStringLiteral(
                    "tabs.setZoomSettings: 'per-origin' recorded — "
                    "Arktos zoom applies per-tab"));
        }
        t.view->setProperty("arktosExtZoomSettings", recorded);
        return okReply();
    }
    if (method == QLatin1String("captureVisibleTab")) {
        int wanted = int(
            args.value(QStringLiteral("windowId")).toDouble(-2));
        BrowserMainWindow *target = nullptr;
        if (wanted == -2) {
            target = currentWindow();
        } else {
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    target = w;
                    break;
                }
            }
        }
        if (!target || !target->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *tw = target->tabWidget();
        WebView *view = tw->currentWebView();
        if (!ExtensionEventHub::trackable(view))
            return errReply(QStringLiteral("not-found"));
        // The capture exposes page pixels — the same url-disclosure
        // class as the tab object's url fields, so it rides the
        // seesUrls gate shapedTab applies ('tabs' or host coverage;
        // Chrome's activeTab invocation grant is not modeled
        // anywhere in this runtime).
        if (!ExtensionBackgroundRuntime::instance()->seesUrls(
                m_extensionId, view->url().toString()))
            return errReply(QStringLiteral("no-permission"));
        const QJsonObject opts =
            args.value(QStringLiteral("options")).toObject();
        const QString format =
            opts.value(QStringLiteral("format"))
                .toString(QStringLiteral("png"));
        if (format != QLatin1String("png")
                && format != QLatin1String("jpeg"))
            return errReply(QStringLiteral("invalid-argument"));
        QPixmap pixmap = view->grab();
        if (pixmap.isNull() && !view->size().isEmpty()) {
            // grab() asks the compositor — a never-realized view
            // still paints through render() into a scratch image.
            QImage fallback(view->size(), QImage::Format_ARGB32);
            fallback.fill(Qt::transparent);
            view->render(&fallback);
            pixmap = QPixmap::fromImage(fallback);
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        const int quality =
            int(opts.value(QStringLiteral("quality")).toDouble(-1));
        const bool saved = !pixmap.isNull()
            && (format == QLatin1String("jpeg")
                ? pixmap.toImage().save(&buffer, "JPEG",
                                        quality < 0 ? -1
                                                    : qBound(0, quality,
                                                             100))
                : pixmap.save(&buffer, "PNG"));
        if (!saved || bytes.isEmpty())
            return errReply(QStringLiteral("capture-failed"));
        return okReply(QStringLiteral("data:image/%1;base64,%2")
            .arg(format, QString::fromLatin1(bytes.toBase64())));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// EXT17 — context-local resolution: the tab whose page IS the asking
// context.  An extension page hosted in a real browser tab (options
// pages via tabs.create) answers itself; a popup/detached page gets
// Chrome's undefined (null result), never a guess.
QJsonObject ExtensionShimBackend::tabForPageReply(QWebEnginePage *page)
{
    if (page) {
        for (BrowserMainWindow *window
             : ExtensionEventHub::browsingWindows()) {
            if (!window || !window->tabWidget())
                continue;
            TabWidget *tw = window->tabWidget();
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (ExtensionEventHub::trackable(view)
                        && view->page() == page)
                    return okReply(tabObject(view, i, idFor(window)));
            }
        }
    }
    return okReply(QJsonValue(QJsonValue::Null));
}

// -------------------------------------------------------------------
// chrome.webNavigation — frame lookup.  The engine only registers
// the main frame per tab (no subframe map is kept), so getFrame /
// getAllFrames answer frameId 0 honestly and report Chrome's own
// 'no frame' shape for anything deeper rather than fabricating a
// frame.  documentId is deliberately absent — callers that verify a
// navigation race (Bitwarden's IPC send) treat a missing id as
// 'cannot check' and proceed, matching Chrome's legacy profile.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeWebNavigation(
        const QString &method, const QJsonObject &args)
{
    if (method == QLatin1String("getFrame")
            || method == QLatin1String("getAllFrames")) {
        const int tabId =
                args.value(QStringLiteral("tabId")).toInt(-1);
        WebView *view = viewForTabId(tabId);
        if (!view || !view->page())
            return errReply(QStringLiteral("no-tab"));
        const QJsonObject frame{
            {QStringLiteral("errorOccurred"), false},
            {QStringLiteral("frameId"), 0},
            {QStringLiteral("parentFrameId"), -1},
            {QStringLiteral("frameType"),
             QStringLiteral("outermost_frame")},
            {QStringLiteral("processId"), -1},
            {QStringLiteral("documentLifecycle"),
             QStringLiteral("active")},
            {QStringLiteral("url"), view->page()->url().toString()}};
        if (method == QLatin1String("getFrame")) {
            const int frameId =
                    args.value(QStringLiteral("frameId")).toInt(-1);
            if (frameId != 0)
                return errReply(QStringLiteral("no-frame"));
            return okReply(frame);
        }
        return okReply(QJsonArray{frame});
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.windows — the window-level mirror of the tabs snapshot.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::windowObject(BrowserMainWindow *window)
{
    QJsonObject out;
    out.insert(QStringLiteral("id"), idFor(window));
    out.insert(QStringLiteral("focused"),
               window && window->isActiveWindow());
    // MD13: the real window-manager state instead of a hardcoded
    // "normal" — Chrome's vocabulary maps straight onto Qt's.
    QString state = QStringLiteral("normal");
    if (window) {
        if (window->isMinimized())
            state = QStringLiteral("minimized");
        else if (window->isMaximized())
            state = QStringLiteral("maximized");
        else if (window->isFullScreen())
            state = QStringLiteral("fullscreen");
    }
    out.insert(QStringLiteral("state"), state);
    out.insert(QStringLiteral("type"), QStringLiteral("normal"));
    out.insert(QStringLiteral("alwaysOnTop"),
               window && window->windowFlags().testFlag(
                   Qt::WindowStaysOnTopHint));
    out.insert(QStringLiteral("incognito"), false);
    if (window) {
        // Chrome reports outer bounds; frameGeometry carries the
        // frame the same way x()/y() do.
        const QRect bounds = window->frameGeometry();
        out.insert(QStringLiteral("left"), bounds.x());
        out.insert(QStringLiteral("top"), bounds.y());
        out.insert(QStringLiteral("width"), bounds.width());
        out.insert(QStringLiteral("height"), bounds.height());
    }
    return out;
}

QJsonObject ExtensionShimBackend::invokeWindows(const QString &method,
                                              const QJsonObject &args)
{
    // EXT16 parity: a window with no trackable tab (all-OTR, tor)
    // is invisible to extensions — filter it out of every answer.
    QList<BrowserMainWindow *> windows;
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window || !window->tabWidget())
            continue;
        TabWidget *tw = window->tabWidget();
        bool visible = false;
        for (int i = 0; i < tw->count(); ++i) {
            if (ExtensionEventHub::trackable(tw->webView(i))) {
                visible = true;
                break;
            }
        }
        if (visible)
            windows.append(window);
    }
    // MD13: Chrome's populate option returns the window's tabs
    // array — scrubbed through the same seesUrls gate tabs.query
    // applies so url/title never ride a side channel.
    const auto tabsFor = [this](BrowserMainWindow *window) {
        QJsonArray tabs;
        TabWidget *tw = window ? window->tabWidget() : nullptr;
        if (tw) {
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (ExtensionEventHub::trackable(view))
                    tabs.append(shapedTab(
                        tabObject(view, i, idFor(window))));
            }
        }
        return tabs;
    };
    if (method == QLatin1String("get")
            || method == QLatin1String("getCurrent")
            || method == QLatin1String("getLastFocused")) {
        const QJsonObject getInfo =
            args.value(QStringLiteral("getInfo")).toObject();
        const bool populate =
            getInfo.value(QStringLiteral("populate")).toBool();
        if (!windowTypeMatch(getInfo))
            return errReply(method == QLatin1String("get")
                ? QStringLiteral("not-found")
                : QStringLiteral("no-window"));
        const auto answer = [populate, &tabsFor](
                BrowserMainWindow *window) {
            QJsonObject win = windowObject(window);
            if (populate)
                win.insert(QStringLiteral("tabs"), tabsFor(window));
            return okReply(win);
        };
        if (method == QLatin1String("get")) {
            const int wanted =
                int(args.value(QStringLiteral("id")).toDouble(-1));
            // WINDOW_ID_CURRENT names the asking window the same
            // way getCurrent does.
            if (wanted == -2) {
                BrowserMainWindow *current = currentWindow();
                if (current && windows.contains(current))
                    return answer(current);
                return errReply(QStringLiteral("not-found"));
            }
            for (BrowserMainWindow *window : windows) {
                if (window && idFor(window) == wanted)
                    return answer(window);
            }
            return errReply(QStringLiteral("not-found"));
        }
        BrowserMainWindow *current = currentWindow();
        if (!current || !windows.contains(current))
            return errReply(QStringLiteral("no-window"));
        return answer(current);
    }
    if (method == QLatin1String("getAll")) {
        const QJsonObject getInfo =
            args.value(QStringLiteral("getInfo")).toObject();
        const bool populate =
            getInfo.value(QStringLiteral("populate")).toBool();
        if (!windowTypeMatch(getInfo))
            return okReply(QJsonArray());
        QJsonArray out;
        for (BrowserMainWindow *window : windows) {
            QJsonObject win = windowObject(window);
            if (populate)
                win.insert(QStringLiteral("tabs"), tabsFor(window));
            out.append(win);
        }
        return okReply(out);
    }
    if (method == QLatin1String("create")) {
        // EXT17 — extensions open real windows (Bitwarden's pop-out
        // and SSO flows call this).  type:'popup'/'panel' has no
        // chrome-less shell to map to, so it lands as a normal
        // window; the url list fans out one tab each.
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        QStringList urls;
        const QJsonValue urlVal = props.value(QStringLiteral("url"));
        if (urlVal.isArray()) {
            for (const QJsonValue &u : urlVal.toArray())
                urls.append(u.toString());
        } else if (urlVal.isString()) {
            urls.append(urlVal.toString());
        }
        QList<QUrl> navUrls;
        for (const QString &u : urls) {
            // ER16: same navigation gate chrome.tabs applies.
            const QUrl url = extensionNavUrl(u);
            if (!u.isEmpty() && !url.isValid())
                return errReply(QStringLiteral("url-refused"));
            navUrls.append(url);
        }
        BrowserApplication *app = BrowserApplication::instance();
        if (!app)
            return errReply(QStringLiteral("no-window"));
        BrowserMainWindow *window = app->newMainWindow();
        if (!window || !window->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *tw = window->tabWidget();
        WebView *first = tw->count() ? tw->webView(0) : nullptr;
        bool usedFirst = false;
        for (const QUrl &url : navUrls) {
            WebView *view = (!usedFirst && first) ? first
                : tw->makeNewTab(false);
            if (!view)
                continue;
            usedFirst = true;
            if (!url.isEmpty())
                view->loadUrl(url);
        }
        // MD13: create honours the same bounds/state props update
        // accepts — Chrome applies them on the fresh window.
        applyWindowProps(window, props);
        if (props.value(QStringLiteral("focused")).toBool(true))
            window->activateWindow();
        QJsonObject reply = windowObject(window);
        // Chrome answers with the tabs array populated — callers
        // read win.tabs[0].id to drive follow-up chrome.tabs calls.
        reply.insert(QStringLiteral("tabs"), tabsFor(window));
        return okReply(reply);
    }
    if (method == QLatin1String("update")) {
        const int wanted =
            int(args.value(QStringLiteral("id")).toDouble(-1));
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        for (BrowserMainWindow *window : windows) {
            if (!window || idFor(window) != wanted)
                continue;
            applyWindowProps(window, props);
            if (props.value(QStringLiteral("focused")).toBool())
                window->activateWindow();
            return okReply(windowObject(window));
        }
        return errReply(QStringLiteral("not-found"));
    }
    if (method == QLatin1String("remove")) {
        const int wanted =
            int(args.value(QStringLiteral("id")).toDouble(-1));
        for (BrowserMainWindow *window : windows) {
            if (!window || idFor(window) != wanted)
                continue;
            window->close();
            return okReply();
        }
        return errReply(QStringLiteral("not-found"));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.action / chrome.browserAction — state feeds the EXT08
// button strip via ExtensionInfo; no native toolbar delegate exists.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeAction(const QString &method,
                                               const QJsonObject &args)
{
    ActionState &state = actionStates()[m_extensionId];
    const QString key = args.value(QStringLiteral("key")).toString();
    if (method == QLatin1String("setIcon")) {
        // details.path — a bare string or a size->path map of
        // bundle-relative files under m_extensionPath (the same
        // hygiene openOptionsPage applies).  details.imageData —
        // {width,height,rgba-base64} or a size->image map of raw
        // RGBA; decoded here so no reader ever sees the wire shape.
        // Neither present (setIcon({})) resets to the manifest
        // default like Chrome.  Validate into locals first so a bad
        // member can't half-commit an override.
        QMap<int, QString> paths;
        QMap<int, QImage> images;
        const auto cleanPath = [](const QString &rel) {
            return !rel.isEmpty()
                && !rel.startsWith(QLatin1Char('/'))
                && !rel.contains(QLatin1Char(':'))
                && !rel.split(QLatin1Char('/'))
                        .contains(QStringLiteral(".."));
        };
        const QJsonValue path = args.value(QStringLiteral("path"));
        if (path.isString()) {
            if (!cleanPath(path.toString()))
                return errReply(QStringLiteral("invalid-path"));
            paths.insert(0, path.toString());
        } else if (path.isObject()) {
            const QJsonObject map = path.toObject();
            for (auto it = map.constBegin(); it != map.constEnd();
                 ++it) {
                const QString rel = it.value().toString();
                if (!cleanPath(rel))
                    return errReply(QStringLiteral("invalid-path"));
                paths.insert(it.key().toInt(), rel);
            }
        } else if (!path.isUndefined() && !path.isNull()) {
            return errReply(QStringLiteral("invalid-path"));
        }
        const auto decodeImage = [](const QJsonValue &v, QImage *out) {
            const QJsonObject spec = v.toObject();
            const int w = spec.value(QStringLiteral("width")).toInt();
            const int h = spec.value(QStringLiteral("height")).toInt();
            const QByteArray rgba = QByteArray::fromBase64(
                spec.value(QStringLiteral("rgba")).toString()
                    .toLatin1());
            if (w <= 0 || h <= 0 || w > 1024 || h > 1024
                    || rgba.size() != qint64(w) * h * 4)
                return false;
            // ImageData pixels are RGBA byte order — Format_RGBA8888
            // is exactly that layout.
            *out = QImage(
                reinterpret_cast<const uchar *>(rgba.constData()),
                w, h, QImage::Format_RGBA8888).copy();
            return true;
        };
        const QJsonValue imageData =
            args.value(QStringLiteral("imageData"));
        const auto decodeInto = [&images, &decodeImage](
                int size, const QJsonValue &v) {
            QImage img;
            if (!decodeImage(v, &img))
                return false;
            images.insert(size, img);
            return true;
        };
        if (imageData.isObject()
                && imageData.toObject().contains(
                    QStringLiteral("rgba"))) {
            if (!decodeInto(0, imageData))
                return errReply(QStringLiteral("invalid-image-data"));
        } else if (imageData.isObject()) {
            const QJsonObject map = imageData.toObject();
            for (auto it = map.constBegin(); it != map.constEnd();
                 ++it) {
                if (!decodeInto(it.key().toInt(), it.value()))
                    return errReply(
                        QStringLiteral("invalid-image-data"));
            }
        } else if (!imageData.isUndefined() && !imageData.isNull()) {
            return errReply(QStringLiteral("invalid-image-data"));
        }
        state.iconPaths = paths;
        state.iconImages = images;
        emit actionStateChanged();
        // The EXT08 strip rebuilds off the manager's changed() —
        // pulse it so a runtime setIcon actually re-renders.
        emit ExtensionManager::instance()->changed();
        return okReply();
    }
    if (method == QLatin1String("set")) {
        if (key == QLatin1String("title"))
            state.title = args.value(QStringLiteral("value")).toString();
        else if (key == QLatin1String("badgeText"))
            state.badgeText =
                args.value(QStringLiteral("value")).toString();
        else if (key == QLatin1String("badgeColor")
                 || key == QLatin1String("badgeTextColor")) {
            const QJsonValue v = args.value(QStringLiteral("value"));
            QString css;
            if (v.isString())
                css = v.toString();
            else if (v.isArray()) {
                const QJsonArray c = v.toArray();
                if (c.size() >= 3)
                    css = QStringLiteral("rgba(%1,%2,%3,%4)")
                        .arg(c.at(0).toInt()).arg(c.at(1).toInt())
                        .arg(c.at(2).toInt())
                        .arg(c.size() > 3
                                 ? c.at(3).toDouble() / 255.0 : 1.0);
            }
            if (key == QLatin1String("badgeColor"))
                state.badgeColor = css;
            else
                state.badgeTextColor = css;
        } else if (key == QLatin1String("popup")) {
            state.popup = args.value(QStringLiteral("value")).toString();
            state.popupSet = true;
        }
        else if (key == QLatin1String("enabled"))
            state.enabled =
                args.value(QStringLiteral("value")).toBool();
        else
            return errReply(QStringLiteral("invalid-key"));
        emit actionStateChanged();
        return okReply();
    }
    if (method == QLatin1String("get")) {
        if (key == QLatin1String("title"))
            return okReply(state.title);
        if (key == QLatin1String("badgeText"))
            return okReply(state.badgeText);
        if (key == QLatin1String("badgeColor")
                || key == QLatin1String("badgeTextColor")) {
            const QString css = key == QLatin1String("badgeColor")
                ? state.badgeColor : state.badgeTextColor;
            // Chrome answers [r,g,b,a]; we stored css — parse the
            // rgba() we wrote, else answer transparent.
            QJsonArray arr{0, 0, 0, 0};
            static const QRegularExpression re(
                QStringLiteral(
                    "rgba?\\((\\d+),(\\d+),(\\d+)(?:,([\\d.]+))?\\)"));
            const QRegularExpressionMatch m = re.match(css);
            if (m.hasMatch()) {
                arr = QJsonArray{m.captured(1).toInt(),
                                 m.captured(2).toInt(),
                                 m.captured(3).toInt(),
                                 m.captured(4).isEmpty()
                                     ? 255
                                     : int(m.captured(4).toDouble()
                                           * 255)};
            }
            return okReply(arr);
        }
        if (key == QLatin1String("popup"))
            return okReply(state.popup);
        if (key == QLatin1String("enabled"))
            return okReply(state.enabled);
        if (key == QLatin1String("icon")) {
            // Internal read-back for delegates/tests — Chrome has
            // no getIcon; the strip consumes ActionState directly.
            QJsonObject out;
            QJsonObject paths;
            for (auto it = state.iconPaths.constBegin();
                 it != state.iconPaths.constEnd(); ++it)
                paths.insert(QString::number(it.key()), it.value());
            out.insert(QStringLiteral("paths"), paths);
            QJsonArray sizes;
            for (const int size : state.iconImages.keys())
                sizes.append(size);
            out.insert(QStringLiteral("imageDataSizes"), sizes);
            return okReply(out);
        }
        return errReply(QStringLiteral("invalid-key"));
    }
    if (method == QLatin1String("getUserSettings")) {
        // MD37: isOnToolbar is the EXT08 strip's membership truth —
        // the bar renders one button per enabled, loaded extension
        // that declares an action popup
        // (ExtensionActionBar::rebuild).  There is no pin toggle
        // today; when one lands it joins this same predicate, so
        // the answer surfaces it honestly instead of freezing true.
        bool onToolbar = false;
        for (const ExtensionManager::ExtensionInfo &info
             : ExtensionManager::instance()->extensions()) {
            if (info.id == m_extensionId) {
                onToolbar = info.loaded && info.enabled
                    && !info.actionPopupUrl.isEmpty();
                break;
            }
        }
        return okReply(QJsonObject{
            {QStringLiteral("isOnToolbar"), onToolbar}});
    }
    // openPopup has no programmatic surface — the EXT08 bubble only
    // opens from its own strip button.  Typed, not silent.
    if (method == QLatin1String("openPopup"))
        return errReply(QStringLiteral("unsupported"));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.i18n — fallback message lookup from _locales/<lang>/
// messages.json (the real binding reads the same files when present).
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::manifestJson() const
{
    if (!m_manifestLoaded) {
        ExtensionShimBackend *self =
            const_cast<ExtensionShimBackend *>(this);
        self->m_manifestLoaded = true;
        // BW02: hosted-only routing strips `background` from the
        // engine-visible manifest — getManifest() must still answer
        // what the extension actually declared.
        self->m_manifest =
            ExtensionManager::declaredManifestJson(m_extensionPath);
    }
    return m_manifest;
}

QStringList ExtensionShimBackend::manifestPermissions() const
{
    // manifestJson()'s "permissions" key is the REQUIRED set;
    // BW09's consented optional grants merge in from the runtime's
    // record so a granted permission unlocks the gated namespaces
    // exactly like a required declaration would.  Unconsented
    // optional_permissions stay inert and never unlock a call.
    QStringList perms;
    for (const QJsonValue &v : manifestJson()
             .value(QStringLiteral("permissions")).toArray())
        perms.append(v.toString());
    if (ExtensionBackgroundRuntime *runtime =
            ExtensionBackgroundRuntime::instance()) {
        for (const QString &granted :
                 runtime->grantedOptionalPermissions(m_extensionId)) {
            if (!perms.contains(granted))
                perms.append(granted);
        }
    }
    return perms;
}

QUrl ExtensionShimBackend::extensionNavUrl(const QString &text) const
{
    const QUrl url(text);
    if (url.isEmpty())
        return url;
    const QString scheme = url.scheme();
    if (scheme.isEmpty()) {
        // Relative spelling — Chrome resolves it against the
        // extension's own origin; apply the same bundle-path hygiene
        // openOptionsPage does before the rewrite ('//host/x' dies
        // here rather than escaping as a network-path reference).
        const QString rel = url.toString();
        if (rel.startsWith(QLatin1Char('/'))
                || rel.contains(QLatin1Char(':'))
                || rel.split(QLatin1Char('/')).contains(
                    QStringLiteral("..")))
            return QUrl();
        return QUrl(QStringLiteral("chrome-extension://")
                    + m_extensionId + QLatin1Char('/') + rel);
    }
    if (!WebView::isUrlAllowedOnUntrustedInput(url))
        return QUrl();
    if (scheme == QLatin1String("chrome-extension"))
        // An extension may open its own documents — never another
        // extension's.
        return url.host() == m_extensionId ? url : QUrl();
    if (scheme == QLatin1String("about"))
        return url.path().isEmpty()
                || url.path() == QLatin1String("blank")
            ? url : QUrl();
    if (scheme == QLatin1String("http")
            || scheme == QLatin1String("https")
            || scheme == QLatin1String("ftp")
            || scheme == QLatin1String("data"))
        return url;
    return QUrl();
}

QJsonObject ExtensionShimBackend::i18nMessages(
        const QString &extensionPath)
{
    if (extensionPath.isEmpty())
        return QJsonObject();

    // Locale pick order: UI languages, then the manifest's declared
    // default, then 'en'.
    QStringList langs = QLocale().uiLanguages();
    {
        QFile mfile(extensionPath + QLatin1String("/manifest.json"));
        if (mfile.size() <= 1024 * 1024
                && mfile.open(QIODevice::ReadOnly)) {
            const QString deflt = QJsonDocument::fromJson(
                mfile.readAll()).object()
                .value(QStringLiteral("default_locale")).toString();
            if (!deflt.isEmpty())
                langs.append(deflt);
        }
    }
    langs.append(QStringLiteral("en"));

    for (const QString &lang : langs) {
        QString dir = QLocale(lang).name();
        QFile file(extensionPath + QLatin1String("/_locales/")
                   + dir + QLatin1String("/messages.json"));
        if (!file.exists()) {
            // Chrome also accepts the short tag (_locales/en).
            const QString shortTag = dir.section(QLatin1Char('_'), 0, 0);
            file.setFileName(extensionPath
                             + QLatin1String("/_locales/") + shortTag
                             + QLatin1String("/messages.json"));
        }
        if (file.size() > 1024 * 1024
                || !file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject root =
            QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject flat;
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            const QString msg = it.value().toObject()
                .value(QStringLiteral("message")).toString();
            if (!msg.isEmpty())
                flat.insert(it.key(), msg);
        }
        return flat;
    }
    return QJsonObject();
}

QJsonObject ExtensionShimBackend::invokeI18n(const QString &method,
                                             const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method != QLatin1String("messages"))
        return errReply(QStringLiteral("unimplemented"));
    if (m_extensionPath.isEmpty())
        return errReply(QStringLiteral("no-path"));
    return okReply(i18nMessages(m_extensionPath));
}

// -------------------------------------------------------------------
// chrome.permissions — BW09's real optional-permission surface.
// contains/getAll report REQUIRED ∪ consented-optional grants;
// request() drives the real consent surface (a runtime grant never
// appears without it) and remove() revokes held optional grants.
// onAdded/onRemoved fan out on pushEvent — the shared backend
// reaches every shim context of the extension, matching Chrome's
// per-context dispatch.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokePermissions(
        const QString &method, const QJsonObject &args)
{
    const QJsonObject manifest = manifestJson();
    // The GRANTED set Chrome reports — required permissions plus
    // whatever optional_permissions the consent surface already
    // granted.  An unconsented optional declaration never lists:
    // claiming it would over-claim the extension's own grants and
    // send guarded optional-permission call paths (Bitwarden's
    // privacy.* probe) into typed errors the guard exists to
    // prevent.
    const QStringList perms = manifestPermissions();
    QStringList origins;
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("host_permissions")).toArray())
        origins.append(v.toString());
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    if (runtime) {
        for (const QString &granted :
                 runtime->grantedOptionalOrigins(m_extensionId)) {
            if (!origins.contains(granted))
                origins.append(granted);
        }
    }

    if (method == QLatin1String("contains")) {
        bool ok = true;
        for (const QJsonValue &v : args.value(
                 QStringLiteral("permissions")).toArray()) {
            if (!perms.contains(v.toString()))
                ok = false;
        }
        for (const QJsonValue &v : args.value(
                 QStringLiteral("origins")).toArray()) {
            if (!origins.contains(v.toString()))
                ok = false;
        }
        return okReply(ok);
    }
    if (method == QLatin1String("getAll")) {
        QJsonObject all;
        QJsonArray p, o;
        for (const QString &s : perms) p.append(s);
        for (const QString &s : origins) o.append(s);
        all.insert(QStringLiteral("permissions"), p);
        all.insert(QStringLiteral("origins"), o);
        return okReply(all);
    }

    if (method != QLatin1String("request")
            && method != QLatin1String("remove"))
        return errReply(QStringLiteral("unimplemented"));
    if (!runtime)
        return errReply(QStringLiteral("no-runtime"));

    const auto askList = [&args](const QString &key) {
        QStringList out;
        for (const QJsonValue &v : args.value(key).toArray()) {
            const QString s = v.toString();
            if (!s.isEmpty() && !out.contains(s))
                out.append(s);
        }
        return out;
    };
    const QStringList wantPerms =
        askList(QStringLiteral("permissions"));
    const QStringList wantOrigins =
        askList(QStringLiteral("origins"));
    const auto declared = [&manifest](const QString &key) {
        QStringList out;
        for (const QJsonValue &v : manifest.value(key).toArray())
            out.append(v.toString());
        return out;
    };
    const QStringList requiredPerms =
        declared(QStringLiteral("permissions"));
    const QStringList requiredOrigins =
        declared(QStringLiteral("host_permissions"));
    const QStringList optionalPerms =
        declared(QStringLiteral("optional_permissions"));
    const QStringList optionalOrigins =
        declared(QStringLiteral("optional_host_permissions"));
    const QStringList heldPerms =
        runtime->grantedOptionalPermissions(m_extensionId);
    const QStringList heldOrigins =
        runtime->grantedOptionalOrigins(m_extensionId);

    if (method == QLatin1String("request")) {
        // Chrome's contract: entries already granted (required
        // declarations or earlier consent) need no prompt; anything
        // the manifest never declared fails outright — an optional
        // grant can only ever come from optional_permissions /
        // optional_host_permissions.
        QStringList askPerms, askOrigins;
        for (const QString &want : wantPerms) {
            if (requiredPerms.contains(want) || heldPerms.contains(want))
                continue;
            if (!optionalPerms.contains(want))
                return errReply(QStringLiteral("not-optional"));
            askPerms.append(want);
        }
        for (const QString &want : wantOrigins) {
            if (requiredOrigins.contains(want)
                    || heldOrigins.contains(want))
                continue;
            if (!optionalOrigins.contains(want))
                return errReply(QStringLiteral("not-optional"));
            askOrigins.append(want);
        }
        if (askPerms.isEmpty() && askOrigins.isEmpty())
            return okReply(true);   // everything already held —
                                    // Chrome resolves sans prompt

        // The consent surface.  ARKTOS_EXT_PERMISSION_ANSWER=
        // grant|deny stands in for the user's click in headless
        // legs — grantConsent's precedent for callers with no UI.
        // With no surface to show the prompt on, the honest verdict
        // is a refusal, never a silent grant.
        bool consented;
        const QString forced = qEnvironmentVariable(
            "ARKTOS_EXT_PERMISSION_ANSWER").toLower();
        if (forced == QLatin1String("grant")) {
            consented = true;
        } else if (forced == QLatin1String("deny")) {
            consented = false;
        } else {
            static const QStringList headless{
                QStringLiteral("offscreen"),
                QStringLiteral("minimal"),
                QStringLiteral("minimalegl")};
            if (headless.contains(QGuiApplication::platformName())) {
                recordNote(QStringLiteral(
                    "chrome.permissions.request: no consent surface "
                    "on a headless platform — answering Chrome's "
                    "honest 'false', the grant stays withheld"));
                consented = false;
            } else {
                consented = ExtensionReviewDialog::requestPermissions(
                    m_extensionName, askPerms, askOrigins);
            }
        }
        if (!consented)
            return okReply(false);   // the honest denial

        runtime->grantOptionalPermissions(
            m_extensionId, askPerms, askOrigins);
        emit pushEvent(QStringLiteral("permissions.onAdded"),
            QJsonObject{{QStringLiteral("__args"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("permissions"),
                     QJsonArray::fromStringList(askPerms)},
                    {QStringLiteral("origins"),
                     QJsonArray::fromStringList(askOrigins)}}}}});
        return okReply(true);
    }

    // remove(): only held optional grants can leave — required
    // declarations and anything never granted are untouched, like
    // Chrome.  onRemoved reports the set that actually went.
    QStringList remPerms, remOrigins;
    for (const QString &want : wantPerms) {
        if (heldPerms.contains(want))
            remPerms.append(want);
    }
    for (const QString &want : wantOrigins) {
        if (heldOrigins.contains(want))
            remOrigins.append(want);
    }
    if (!remPerms.isEmpty() || !remOrigins.isEmpty()) {
        runtime->revokeOptionalPermissions(
            m_extensionId, remPerms, remOrigins);
        emit pushEvent(QStringLiteral("permissions.onRemoved"),
            QJsonObject{{QStringLiteral("__args"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("permissions"),
                     QJsonArray::fromStringList(remPerms)},
                    {QStringLiteral("origins"),
                     QJsonArray::fromStringList(remOrigins)}}}}});
    }
    return okReply(true);
}

// -------------------------------------------------------------------
// chrome.contextMenus — the shared registry EXT16's page-menu
// contribution point renders.  create() answers the id like Chrome
// and refuses duplicates (a silent overwrite would orphan a
// listener's menuItemId).
// -------------------------------------------------------------------

QList<ExtensionShimBackend::ContextMenuEntry>
ExtensionShimBackend::contextMenuEntries()
{
    QList<ContextMenuEntry> out;
    const MenuRegistry &registry = menuRegistry();
    for (auto it = registry.items.constBegin();
         it != registry.items.constEnd(); ++it) {
        for (const auto &item : it.value()) {
            ContextMenuEntry entry;
            entry.extensionId = it.key();
            entry.extensionName = registry.names.value(it.key());
            entry.itemId = item.first;
            entry.props = item.second;
            out.append(entry);
        }
    }
    return out;
}

QJsonObject ExtensionShimBackend::contextMenuItem(
        const QString &extensionId, const QString &itemId)
{
    const auto items = menuRegistry().items.value(extensionId);
    for (const auto &item : items) {
        if (item.first == itemId)
            return item.second;
    }
    return QJsonObject();
}

void ExtensionShimBackend::setContextMenuChecked(
        const QString &extensionId, const QString &itemId,
        bool checked)
{
    auto &items = menuRegistry().items[extensionId];
    for (auto &item : items) {
        if (item.first == itemId) {
            item.second.insert(QStringLiteral("checked"), checked);
            return;
        }
    }
}

QJsonObject ExtensionShimBackend::invokeContextMenus(
        const QString &method, const QJsonObject &args)
{
    // ER16: 'contextMenus' (or Firefox's 'menus') gates the
    // namespace — without it an ungated create() would render
    // extension-controlled entries into the user's context menu.
    const QStringList perms = manifestPermissions();
    if (!perms.contains(QLatin1String("contextMenus"))
            && !perms.contains(QLatin1String("menus")))
        return errReply(QStringLiteral("no-permission"));
    // Re-claim the slot — a stale owner's destructor may already
    // have cleared it (respawn ordering or test backends sharing an
    // id); whoever writes owns the entries.
    if (menuRegistry().owners.value(m_extensionId) != this) {
        menuRegistry().owners.insert(m_extensionId, this);
        menuRegistry().names.insert(m_extensionId, m_extensionName);
    }
    auto &items = menuRegistry().items[m_extensionId];
    const auto findItem = [&items](const QString &id) {
        for (int i = 0; i < items.size(); ++i) {
            if (items.at(i).first == id)
                return i;
        }
        return -1;
    };
    if (method == QLatin1String("create")) {
        QJsonObject props = args.value(QStringLiteral("props")).toObject();
        QString id = props.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            id = QStringLiteral("item-%1").arg(items.size() + 1);
        if (findItem(id) >= 0)
            return errReply(QStringLiteral("duplicate-id"));
        props.insert(QStringLiteral("id"), id);
        items.append({id, props});
        return okReply(QJsonObject{{QStringLiteral("id"), id}});
    }
    if (method == QLatin1String("update")) {
        const QString id = args.value(QStringLiteral("id")).toString();
        const int at = findItem(id);
        if (at < 0)
            return errReply(QStringLiteral("not-found"));
        QJsonObject props = items.at(at).second;
        const QJsonObject patch =
            args.value(QStringLiteral("props")).toObject();
        for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
            props.insert(it.key(), it.value());
        items[at].second = props;
        return okReply();
    }
    if (method == QLatin1String("remove")) {
        const QString id = args.value(QStringLiteral("id")).toString();
        const int at = findItem(id);
        if (at < 0)
            return errReply(QStringLiteral("not-found"));
        items.removeAt(at);
        return okReply();
    }
    if (method == QLatin1String("removeAll")) {
        items.clear();
        return okReply();
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.sidePanel — Arktos has no side-panel surface; options are
// recorded honestly so EXT17/later work can render them.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeSidePanel(
        const QString &method, const QJsonObject &args)
{
    // ER16: 'sidePanel' gates the namespace like Chrome.
    if (!manifestPermissions().contains(QLatin1String("sidePanel")))
        return errReply(QStringLiteral("no-permission"));
    if (method == QLatin1String("setOptions")) {
        m_sidePanelOptions =
            args.value(QStringLiteral("options")).toObject();
        return okReply();
    }
    if (method == QLatin1String("getOptions"))
        return okReply(m_sidePanelOptions);
    if (method == QLatin1String("setPanelBehavior")) {
        m_sidePanelBehavior =
            args.value(QStringLiteral("behavior")).toObject();
        return okReply();
    }
    if (method == QLatin1String("getPanelBehavior"))
        return okReply(m_sidePanelBehavior);
    return errReply(QStringLiteral("unimplemented"));
}

void ExtensionShimBackend::recordNote(const QString &message)
{
    if (m_notes.size() < 500)
        m_notes.append(message.left(2048));
    emit noteRecorded(message.left(2048));
}

// -------------------------------------------------------------------
// chrome.notifications — ER12.  Chrome exposes the namespace only to
// extensions declaring the 'notifications' permission — the gate
// reads the same installed manifest the permissions.* surface does.
// The record registry and platform presentation live in
// ExtensionNotificationCenter (freedesktop Notify on the session
// bus); events route back through the runtime's per-extension
// fan-out.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeNotifications(
        const QString &method, const QJsonObject &args)
{
    if (!manifestPermissions().contains(
            QLatin1String("notifications")))
        return errReply(QStringLiteral("no-permission"));

    ExtensionNotificationCenter *center =
        ExtensionNotificationCenter::instance();
    if (method == QLatin1String("create"))
        return center->create(
            this, args.value(QStringLiteral("id")).toString(),
            args.value(QStringLiteral("options")).toObject());
    if (method == QLatin1String("update"))
        return center->update(
            this, args.value(QStringLiteral("id")).toString(),
            args.value(QStringLiteral("options")).toObject());
    if (method == QLatin1String("clear"))
        return center->clear(
            this, args.value(QStringLiteral("id")).toString());
    if (method == QLatin1String("getAll"))
        return center->getAll(this);
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.commands — ER14.  No permission gate: Chrome serves the
// namespace to every extension.  getAll answers the registry's
// bound-state rows — every declared command with the CURRENTLY
// bound shortcut in Chrome key format, '' for absent/unbindable
// suggestions (Chrome's unassigned rows).  The event side is the
// C++-driven commands.onCommand dispatch from
// ExtensionCommandRegistry.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeCommands(
        const QString &method, const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method == QLatin1String("getAll"))
        return okReply(ExtensionCommandRegistry::instance()
                           ->commandList(m_extensionId));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.idle — ER13.  Chrome exposes the namespace only to
// extensions declaring 'idle'; the gate reads the same installed
// manifest permissions.* answers from.  State lives in
// ExtensionIdleWatcher (browser-global, like Chrome's): an
// application-wide input filter feeding a QTimer at the configured
// detection interval.  Activity is measured inside Arktos windows
// only and 'locked' is never reported — the shim notes the
// approximation once per context.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeIdle(
        const QString &method, const QJsonObject &args)
{
    if (!manifestPermissions().contains(QLatin1String("idle")))
        return errReply(QStringLiteral("no-permission"));

    ExtensionIdleWatcher *watcher = ExtensionIdleWatcher::instance();
    if (method == QLatin1String("queryState")) {
        const QJsonValue v = args.value(QStringLiteral("interval"));
        if (!v.isDouble() || v.toDouble() < 0)
            return errReply(QStringLiteral("invalid-argument"));
        return okReply(watcher->queryState(int(v.toDouble())));
    }
    if (method == QLatin1String("setDetectionInterval")) {
        const QJsonValue v = args.value(QStringLiteral("interval"));
        if (!v.isDouble() || v.toDouble() < 0)
            return errReply(QStringLiteral("invalid-argument"));
        watcher->setDetectionInterval(int(v.toDouble()));
        return okReply();
    }
    // The shim's onStateChanged onAdd — a listener registration is
    // what arms the watcher for an extension that only listens.
    if (method == QLatin1String("subscribe")) {
        watcher->subscribe();
        return okReply();
    }
    // getAutoLockDelay exists on ChromeOS only — typed, not silent.
    if (method == QLatin1String("getAutoLockDelay"))
        return errReply(QStringLiteral("unsupported"));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.management — ER15.  getSelf is ungated like Chrome (the
// only member that needs no 'management' permission) and answers the
// real ExtensionInfo shape from the manifest + the manager's own
// records.  MD16 serves the read ops the grant unlocks — getAll/get
// enumerate the manager's live ExtensionInfo rows through the same
// shape and the permissionWarnings pair derive their strings from
// the manifest's grant list (the same wording the install review
// shows).  MD17 serves the mutations — setEnabled/uninstall/
// uninstallSelf ride ExtensionManager::setExtensionEnabled/
// removeExtension and fan management.onEnabled/onDisabled/
// onUninstalled out to every 'management'-declaring context.
// Every other member rides Chrome's failure vocabulary:
// 'management' undeclared -> no-permission, declared-but-unserved ->
// unsupported.
// -------------------------------------------------------------------

// manifest.json for an extension other than the caller's own — the
// read ops shape every enumerated record, and each needs its own
// bundle's manifest (the same 1MB guard manifestJson() applies).
static QJsonObject manifestForExtensionPath(const QString &extensionPath)
{
    if (extensionPath.isEmpty())
        return QJsonObject();
    // BW02: same declared-manifest read as manifestJson() — a
    // hosted-only tree's engine manifest lacks `background`.
    return ExtensionManager::declaredManifestJson(extensionPath);
}

// MD17: a record the engine keeps only as an unload tombstone — a
// dev-loaded extension can linger in the enumeration after
// unloadExtension; Chrome's management surface has no such row
// (an installed extension that is merely disabled still answers
// loaded==false but keeps installed==true and stays listed).
static bool managementRecordGone(
        const ExtensionManager::ExtensionInfo &info)
{
    return !info.loaded && !info.installed;
}

// MD17: ids the manager has reported unloaded/uninstalled this
// session.  A dev-loaded extension usually VANISHES from the
// enumeration entirely on unload — the get()/getAll()/warnings
// self-fallback ("hosted backend the manager never tracked") must
// not resurrect a removed extension, so the lifecycle signals mark
// ids on the way out and clear them on a fresh load/install.
// Mutations also mark at dispatch so the window between the
// removeExtension call and unloadFinished never reports the id
// as alive-but-untracked.
static QSet<QString> &removedExtensionIds()
{
    static QSet<QString> *removed = []() {
        QSet<QString> *set = new QSet<QString>();
        if (ExtensionManager *manager = ExtensionManager::instance()) {
            const auto mark = [set](
                    const ExtensionManager::ExtensionInfo &info) {
                if (!info.id.isEmpty())
                    set->insert(info.id);
            };
            const auto unmark = [set](
                    const ExtensionManager::ExtensionInfo &info) {
                set->remove(info.id);
            };
            QObject::connect(manager,
                             &ExtensionManager::extensionUnloaded,
                             manager, mark);
            QObject::connect(manager,
                             &ExtensionManager::extensionUninstalled,
                             manager, mark);
            QObject::connect(manager,
                             &ExtensionManager::extensionLoaded,
                             manager, unmark);
            QObject::connect(manager,
                             &ExtensionManager::extensionInstalled,
                             manager, unmark);
        }
        return set;
    }();
    return *removed;
}

static bool extensionMarkedRemoved(const QString &id)
{
    return removedExtensionIds().contains(id);
}

QJsonObject ExtensionShimBackend::managementInfoJson(
        const QString &id, const QString &fallbackName,
        const QString &extensionPath, const QJsonObject &manifest,
        const ExtensionManager::ExtensionInfo *record)
{
    bool messagesLoaded = false;
    QJsonObject messages;
    const auto localized = [&messages, &messagesLoaded,
                            &extensionPath](const QString &value) {
        // Manifests localize user-facing strings as __MSG_key__ —
        // the management surface answers the resolved text like
        // Chrome does.
        static const QRegularExpression msgRe(
            QStringLiteral("^__MSG_(.+)__$"));
        const QRegularExpressionMatch m = msgRe.match(value);
        if (!m.hasMatch())
            return value;
        if (!messagesLoaded) {
            messagesLoaded = true;
            messages = i18nMessages(extensionPath);
        }
        const QString resolved =
            messages.value(m.captured(1)).toString();
        return resolved.isEmpty() ? value : resolved;
    };

    // The manager's record is the source of truth for enabled and
    // install provenance; a hosted backend that the manager lost
    // track of still answers enabled (it is running).
    const bool enabled = record ? record->enabled : true;
    const QString installType = !record
        ? QStringLiteral("development")
        : record->builtin
            ? QStringLiteral("internal")
        : record->installed
            ? QStringLiteral("normal")
            : QStringLiteral("development");

    QJsonObject info;
    info.insert(QStringLiteral("id"), id);
    info.insert(QStringLiteral("name"),
                localized(manifest.value(QStringLiteral("name"))
                              .toString(fallbackName)));
    info.insert(QStringLiteral("shortName"),
                localized(manifest.value(QStringLiteral("short_name"))
                              .toString(info.value(
                                  QStringLiteral("name")).toString())));
    info.insert(QStringLiteral("description"),
                localized(manifest.value(
                    QStringLiteral("description")).toString()));
    info.insert(QStringLiteral("version"),
                manifest.value(QStringLiteral("version")).toString());
    info.insert(QStringLiteral("versionName"),
                manifest.value(QStringLiteral("version_name"))
                    .toString());
    info.insert(QStringLiteral("manifestVersion"),
                manifest.value(QStringLiteral("manifest_version"))
                    .toInt());
    info.insert(QStringLiteral("enabled"), enabled);
    info.insert(QStringLiteral("mayEnable"), !enabled);
    info.insert(QStringLiteral("mayDisable"), enabled);
    info.insert(QStringLiteral("type"), QStringLiteral("extension"));
    info.insert(QStringLiteral("installType"), installType);
    info.insert(QStringLiteral("offlineEnabled"),
                manifest.value(QStringLiteral("offline_enabled"))
                    .toBool(true));
    const QString homepage =
        manifest.value(QStringLiteral("homepage_url")).toString();
    if (!homepage.isEmpty())
        info.insert(QStringLiteral("homepageUrl"), homepage);
    const QString updateUrl =
        manifest.value(QStringLiteral("update_url")).toString();
    if (!updateUrl.isEmpty())
        info.insert(QStringLiteral("updateUrl"), updateUrl);
    // options page — same validation openOptionsPage applies before
    // turning the manifest path into a chrome-extension url.
    QString options = manifest.value(QStringLiteral("options_ui"))
        .toObject().value(QStringLiteral("page")).toString();
    if (options.isEmpty())
        options = manifest.value(QStringLiteral("options_page"))
            .toString();
    if (!options.isEmpty() && !options.startsWith(QLatin1Char('/'))
            && !options.contains(QLatin1Char(':'))
            && !options.split(QLatin1Char('/')).contains(
                QStringLiteral("..")))
        info.insert(QStringLiteral("optionsUrl"),
                    QStringLiteral("chrome-extension://")
                        + id + QLatin1Char('/') + options);
    QJsonArray icons;
    const QJsonObject iconMap =
        manifest.value(QStringLiteral("icons")).toObject();
    for (auto it = iconMap.constBegin(); it != iconMap.constEnd();
         ++it) {
        const QString path = it.value().toString();
        if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
                || path.contains(QLatin1Char(':'))
                || path.split(QLatin1Char('/')).contains(
                    QStringLiteral("..")))
            continue;
        QJsonObject icon;
        icon.insert(QStringLiteral("size"), it.key().toInt());
        icon.insert(QStringLiteral("url"),
                    QStringLiteral("chrome-extension://")
                        + id + QLatin1Char('/') + path);
        icons.append(icon);
    }
    if (!icons.isEmpty())
        info.insert(QStringLiteral("icons"), icons);
    QJsonArray permissions;
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("permissions")).toArray())
        permissions.append(v.toString());
    info.insert(QStringLiteral("permissions"), permissions);
    // hostPermissions = host_permissions ∪ declared content-script
    // matches ∪ MV2-style match patterns inside permissions —
    // Chrome's management surface reports the union.
    QJsonArray hostPermissions;
    const auto addHost = [&hostPermissions](const QString &pattern) {
        for (const QJsonValue &v : hostPermissions) {
            if (v.toString() == pattern)
                return;
        }
        hostPermissions.append(pattern);
    };
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("host_permissions")).toArray())
        addHost(v.toString());
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("content_scripts")).toArray()) {
        for (const QJsonValue &m : v.toObject().value(
                 QStringLiteral("matches")).toArray())
            addHost(m.toString());
    }
    for (const QJsonValue &v : permissions) {
        const QString p = v.toString();
        if (p.contains(QLatin1String("://"))
                || p == QLatin1String("<all_urls>"))
            addHost(p);
    }
    info.insert(QStringLiteral("hostPermissions"), hostPermissions);
    return info;
}

QJsonArray ExtensionShimBackend::managementWarnings(
        const QJsonObject &manifest)
{
    // Honest warning strings for the grant list — the same wording
    // ExtensionReviewDialog's consent rows show.  API names and host
    // patterns run through separate tables like Chrome's permission
    // messages do; declarations keep manifest order, deduplicated.
    QJsonArray warnings;
    const auto add = [&warnings](const QString &text) {
        if (text.isEmpty())
            return;
        for (const QJsonValue &v : warnings) {
            if (v.toString() == text)
                return;
        }
        warnings.append(text);
    };
    QStringList apiPerms;
    QStringList hostPerms;
    const auto addHost = [&hostPerms](const QString &pattern) {
        if (!hostPerms.contains(pattern))
            hostPerms.append(pattern);
    };
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("permissions")).toArray()) {
        const QString p = v.toString();
        if (p.contains(QLatin1String("://"))
                || p == QLatin1String("<all_urls>"))
            addHost(p);
        else if (!apiPerms.contains(p))
            apiPerms.append(p);
    }
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("host_permissions")).toArray())
        addHost(v.toString());
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("content_scripts")).toArray()) {
        for (const QJsonValue &m : v.toObject().value(
                 QStringLiteral("matches")).toArray())
            addHost(m.toString());
    }
    for (const QString &p : apiPerms)
        add(ExtensionReviewDialog::describePermission(p));
    for (const QString &h : hostPerms)
        add(ExtensionReviewDialog::describeHostPermission(h));
    return warnings;
}

QJsonObject ExtensionShimBackend::invokeManagement(
        const QString &method, const QJsonObject &args)
{
    if (method == QLatin1String("getSelf")) {
        const ExtensionManager::ExtensionInfo *record = nullptr;
        if (ExtensionManager *manager = ExtensionManager::instance()) {
            const QList<ExtensionManager::ExtensionInfo> records =
                manager->extensions();
            for (const ExtensionManager::ExtensionInfo &info : records) {
                if (info.id != m_extensionId)
                    continue;
                record = &info;
                break;
            }
        }
        return okReply(managementInfoJson(
            m_extensionId, m_extensionName, m_extensionPath,
            manifestJson(), record));
    }

    // Everything below rides the 'management' grant — undeclared
    // keeps Chrome's no-permission answer.
    if (!manifestPermissions().contains(QLatin1String("management")))
        return errReply(QStringLiteral("no-permission"));

    if (method == QLatin1String("getAll")) {
        QJsonArray out;
        bool selfSeen = false;
        if (ExtensionManager *manager = ExtensionManager::instance()) {
            for (const ExtensionManager::ExtensionInfo &info
                 : manager->extensions()) {
                if (info.id == m_extensionId)
                    selfSeen = true;   // tombstones count — a removed
                                     // self must not resurrect below
                if (managementRecordGone(info))
                    continue;
                out.append(managementInfoJson(
                    info.id, info.name, info.path,
                    manifestForExtensionPath(info.path), &info));
            }
        }
        // Chrome's getAll lists the caller too — a hosted backend
        // the manager lost track of still appends its own row, but
        // never one the manager reported removed this session.
        if (!selfSeen && !m_extensionPath.isEmpty()
                && !extensionMarkedRemoved(m_extensionId))
            out.append(managementInfoJson(
                m_extensionId, m_extensionName, m_extensionPath,
                manifestJson(), nullptr));
        return okReply(out);
    }
    if (method == QLatin1String("get")) {
        const QString wanted = args.value(QStringLiteral("id")).toString();
        if (wanted.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        bool sawRecord = false;
        if (ExtensionManager *manager = ExtensionManager::instance()) {
            for (const ExtensionManager::ExtensionInfo &info
                 : manager->extensions()) {
                if (info.id != wanted)
                    continue;
                sawRecord = true;
                if (managementRecordGone(info))
                    break;   // tombstone — it was removed
                return okReply(managementInfoJson(
                    info.id, info.name, info.path,
                    manifestForExtensionPath(info.path), &info));
            }
        }
        // The self-fallback covers a hosted backend the manager
        // never tracked — a tombstone record or a removed marker
        // means it was uninstalled, which is Chrome's not-found,
        // never a resurrection.
        if (!sawRecord && wanted == m_extensionId
                && !extensionMarkedRemoved(wanted))
            return okReply(managementInfoJson(
                m_extensionId, m_extensionName, m_extensionPath,
                manifestJson(), nullptr));
        return errReply(QStringLiteral("not-found"));
    }
    if (method == QLatin1String("getPermissionWarningsById")) {
        const QString wanted =
            args.value(QStringLiteral("id")).toString();
        if (wanted.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        QString path;
        bool found = false;
        bool sawRecord = false;
        if (ExtensionManager *manager = ExtensionManager::instance()) {
            for (const ExtensionManager::ExtensionInfo &info
                 : manager->extensions()) {
                if (info.id != wanted)
                    continue;
                sawRecord = true;
                if (managementRecordGone(info))
                    break;   // tombstone — it was removed
                path = info.path;
                found = true;
                break;
            }
        }
        // Same rule get() applies: the self-fallback only stands in
        // for a never-tracked backend, not a removed one.
        if (!sawRecord && wanted == m_extensionId
                && !extensionMarkedRemoved(wanted)) {
            path = m_extensionPath;
            found = true;
        }
        if (!found)
            return errReply(QStringLiteral("not-found"));
        return okReply(managementWarnings(
            wanted == m_extensionId ? manifestJson()
                                    : manifestForExtensionPath(path)));
    }
    if (method == QLatin1String("getPermissionWarningsByManifest")) {
        // Chrome takes the manifest as a JSON string; an already-
        // parsed object is accepted too — either way a malformed
        // payload is a typed refusal, never an empty list.
        const QJsonValue raw =
            args.value(QStringLiteral("manifest"));
        const QJsonObject manifest =
            raw.isObject()
                ? raw.toObject()
                : QJsonDocument::fromJson(raw.toString().toUtf8())
                      .object();
        if (manifest.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        return okReply(managementWarnings(manifest));
    }

    // --- mutations (MD17) -------------------------------------------
    // Each rides ExtensionManager's real engine calls; the engine
    // applies asynchronously, so the event carries the committed
    // end-state, not a poll of it.  Chrome's transition events only
    // describe real changes — a setEnabled that leaves the record
    // where it was fires nothing.
    if (method == QLatin1String("setEnabled")) {
        const QString wanted = args.value(QStringLiteral("id")).toString();
        if (wanted.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        ExtensionManager *manager = ExtensionManager::instance();
        const ExtensionManager::ExtensionInfo *record = nullptr;
        QList<ExtensionManager::ExtensionInfo> records;
        if (manager)
            records = manager->extensions();
        for (const ExtensionManager::ExtensionInfo &info : records) {
            if (info.id != wanted || managementRecordGone(info))
                continue;
            record = &info;
            break;
        }
        if (!record)
            return errReply(QStringLiteral("not-found"));
        // Component extensions (the bundled PDF viewer & friends)
        // are not user-toggleable — Chrome refuses the same way.
        if (record->builtin)
            return errReply(QStringLiteral("cannot-modify"));
        const bool enabled =
            args.value(QStringLiteral("enabled")).toBool();
        if (record->enabled != enabled) {
            QJsonObject info = managementInfoJson(
                record->id, record->name, record->path,
                manifestForExtensionPath(record->path), record);
            info.insert(QStringLiteral("enabled"), enabled);
            info.insert(QStringLiteral("mayEnable"), !enabled);
            info.insert(QStringLiteral("mayDisable"), enabled);
            manager->setExtensionEnabled(wanted, enabled);
            managementFanOut(
                enabled ? QStringLiteral("management.onEnabled")
                        : QStringLiteral("management.onDisabled"),
                QJsonArray{info});
        }
        return okReply();
    }
    if (method == QLatin1String("uninstall")) {
        const QString wanted = args.value(QStringLiteral("id")).toString();
        if (wanted.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        ExtensionManager *manager = ExtensionManager::instance();
        bool found = false;
        bool builtin = false;
        if (manager) {
            for (const ExtensionManager::ExtensionInfo &info
                 : manager->extensions()) {
                if (info.id != wanted || managementRecordGone(info))
                    continue;
                found = true;
                builtin = info.builtin;
                break;
            }
        }
        if (!found)
            return errReply(QStringLiteral("not-found"));
        if (builtin)
            return errReply(QStringLiteral("cannot-modify"));
        removedExtensionIds().insert(wanted);
        manager->removeExtension(wanted);
        managementFanOut(QStringLiteral("management.onUninstalled"),
                         QJsonArray{wanted});
        return okReply();
    }
    if (method == QLatin1String("uninstallSelf")) {
        // Chrome does not require 'management' for self-removal —
        // the permission gate above has already run, which is a
        // deliberate tightening (uninstall is a destructive call;
        // an extension that never declared the namespace should not
        // reach it silently).
        removedExtensionIds().insert(m_extensionId);
        if (ExtensionManager *manager = ExtensionManager::instance())
            manager->removeExtension(m_extensionId);
        managementFanOut(QStringLiteral("management.onUninstalled"),
                         QJsonArray{m_extensionId});
        return okReply();
    }
    return errReply(QStringLiteral("unsupported"));
}

void ExtensionShimBackend::managementFanOut(
        const QString &name, const QJsonArray &eventArgs)
{
    const QJsonObject payload{
        {QStringLiteral("__args"), eventArgs}};
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->broadcastEvent(name, payload);
    // The broadcast covers every registered context — the caller's
    // included.  A bare backend (tests, a host mid-respawn) has no
    // registered context, so pushEvent is its only surface — the
    // same fallback the notification center applies.
    if (!runtime->hasRelayContext(m_extensionId))
        emit pushEvent(name, payload);
}

// -------------------------------------------------------------------
// chrome.bookmarks — MD21.  The real bookmark tree served over the
// RC05 store through the C++ wrappers (BookmarkNode/BookmarksManager
// are already the facade/FFI lane); mutations ride the manager's
// undoable ops so entryAdded/entryRemoved/entryChanged keep the
// model, toolbar and autosave in lockstep with the page's view.
// The 'bookmarks' manifest permission gates the whole namespace —
// undeclared keeps Chrome's no-permission answer — and the same
// grant scopes the on* event fan-out (eventGate).  Api-driven
// mutations suppress the signal tap while they emit their own
// Chrome-accurate events; UI-driven changes (drag, dialog, undo)
// still reach 'bookmarks'-declaring contexts through it.
// -------------------------------------------------------------------

void ExtensionShimBackend::bookmarksFanOut(
        const QString &name, const QJsonArray &eventArgs)
{
    const QJsonObject payload{
        {QStringLiteral("__args"), eventArgs}};
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->broadcastEvent(name, payload);
    // Same bare-backend fallback managementFanOut applies — tests
    // and mid-respawn hosts have no registered context.
    if (!runtime->hasRelayContext(m_extensionId))
        emit pushEvent(name, payload);
}

QJsonObject ExtensionShimBackend::invokeBookmarks(
        const QString &method, const QJsonObject &args)
{
    // ER16 shape: Chrome exposes the namespace only under the
    // declared 'bookmarks' grant — the bridge object answers a
    // direct call from any code in the context, so the gate runs
    // here too.
    if (!manifestPermissions().contains(QLatin1String("bookmarks")))
        return errReply(QStringLiteral("no-permission"));

    BookmarksManager *manager = BookmarksManager::instance();
    if (!manager)
        return errReply(QStringLiteral("no-store"));
    tapBmEvents(manager);

    if (method == QLatin1String("get")) {
        QStringList ids;
        const QJsonValue idArg = args.value(QLatin1String("id"));
        if (idArg.isString())
            ids.append(idArg.toString());
        const QJsonArray idList =
            args.value(QLatin1String("ids")).toArray();
        for (const QJsonValue &v : idList)
            ids.append(v.toString());
        if (ids.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        QJsonArray out;
        for (const QString &id : ids) {
            BookmarkNode *node = bmResolve(manager, id);
            if (!node)
                return errReply(QStringLiteral("not-found"));
            out.append(bmNodeJson(manager, node, false));
        }
        return okReply(out);
    }

    if (method == QLatin1String("getChildren")) {
        const QString id = args.value(QLatin1String("id")).toString();
        if (id.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        BookmarkNode *node = bmResolve(manager, id);
        if (!node)
            return errReply(QStringLiteral("not-found"));
        QJsonArray out;
        const QList<BookmarkNode *> children = node->children();
        for (BookmarkNode *child : children)
            out.append(bmNodeJson(manager, child, false));
        return okReply(out);
    }

    if (method == QLatin1String("getRecent")) {
        int count = args.value(QLatin1String("count")).toInt(-1);
        if (count < 0)
            count = 1;                      // Chrome's default
        QList<BookmarkNode *> all;
        bmCollect(manager->bookmarks(), &all);
        QList<BookmarkNode *> leaves;
        for (BookmarkNode *node : all) {
            if (node->type() == BookmarkNode::Bookmark)
                leaves.append(node);
        }
        // The store keeps no timestamps; handle serials mint in
        // creation order, so serial-desc is the honest "recent" it
        // can answer — file-loaded nodes keep document order and
        // anything the API created this session outranks them.
        std::sort(leaves.begin(), leaves.end(),
                  [](BookmarkNode *a, BookmarkNode *b) {
                      return bmSerial(a) > bmSerial(b);
                  });
        if (!m_bmRecentNoted) {
            m_bmRecentNoted = true;
            recordNote(QStringLiteral(
                "bookmarks.getRecent: the store carries no creation "
                "timestamps — ordering is node-serial desc (insert "
                "order), not Chrome's dateAdded sort"));
        }
        QJsonArray out;
        for (int i = 0; i < leaves.count() && i < count; ++i)
            out.append(bmNodeJson(manager, leaves.at(i), false));
        return okReply(out);
    }

    if (method == QLatin1String("getTree"))
        return okReply(QJsonArray{
            bmNodeJson(manager, manager->bookmarks(), true)});

    if (method == QLatin1String("getSubTree")) {
        const QString id = args.value(QLatin1String("id")).toString();
        if (id.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        BookmarkNode *node = bmResolve(manager, id);
        if (!node)
            return errReply(QStringLiteral("not-found"));
        return okReply(QJsonArray{bmNodeJson(manager, node, true)});
    }

    if (method == QLatin1String("search")) {
        // Chrome/MDN shape: a string is space-delimited terms (double
        // quotes group phrases) each matched as a case-insensitive
        // substring of title OR url; an object ANDs `query` (same
        // term form) with `title` (verbatim, case-sensitive), `url`
        // (verbatim, case-insensitive, trailing slashes ignored) and
        // `folder` (the containing folder's title, verbatim).
        const QJsonValue q = args.value(QLatin1String("query"));
        QStringList terms;
        bool hasTitle = false, hasUrl = false, hasFolder = false;
        QString exactTitle, exactUrl, exactFolder;
        if (q.isString()) {
            terms = bmQueryTerms(q.toString());
        } else if (q.isObject()) {
            const QJsonObject o = q.toObject();
            terms = bmQueryTerms(
                o.value(QLatin1String("query")).toString());
            hasTitle = o.contains(QLatin1String("title"));
            hasUrl = o.contains(QLatin1String("url"));
            hasFolder = o.contains(QLatin1String("folder"));
            exactTitle = o.value(QLatin1String("title")).toString();
            exactUrl = o.value(QLatin1String("url")).toString();
            exactFolder = o.value(QLatin1String("folder")).toString();
        } else {
            return errReply(QStringLiteral("invalid-argument"));
        }
        QList<BookmarkNode *> all;
        bmCollect(manager->bookmarks(), &all);
        QJsonArray out;
        for (BookmarkNode *node : all) {
            // Bookmarks and folders match (MDN: folders are in the
            // result set); separators have no text to match.
            if (node->type() != BookmarkNode::Bookmark
                    && node->type() != BookmarkNode::Folder)
                continue;
            bool match = true;
            for (const QString &term : terms) {
                if (!node->title.contains(term, Qt::CaseInsensitive)
                        && !node->url.contains(
                            term, Qt::CaseInsensitive)) {
                    match = false;
                    break;
                }
            }
            if (match && hasTitle && node->title != exactTitle)
                match = false;
            if (match && hasUrl) {
                QString a = node->url;
                QString b = exactUrl;
                while (a.endsWith(QLatin1Char('/')))
                    a.chop(1);
                while (b.endsWith(QLatin1Char('/')))
                    b.chop(1);
                if (a.compare(b, Qt::CaseInsensitive) != 0)
                    match = false;
            }
            if (match && hasFolder) {
                BookmarkNode *p = node->parent();
                if (!p || p->title != exactFolder)
                    match = false;
            }
            if (match)
                out.append(bmNodeJson(manager, node, false));
        }
        return okReply(out);
    }

    if (method == QLatin1String("create")) {
        if (!args.contains(QLatin1String("props")))
            return errReply(QStringLiteral("invalid-argument"));
        const QJsonObject props =
            args.value(QLatin1String("props")).toObject();
        BookmarkNode *parent = manager->menu();  // Chrome: Other
        if (props.contains(QLatin1String("parentId"))) {
            parent = bmResolve(
                manager,
                props.value(QLatin1String("parentId")).toString());
            if (!parent)
                return errReply(QStringLiteral("not-found"));
            if (parent->type() == BookmarkNode::Bookmark
                    || parent->type() == BookmarkNode::Separator)
                return errReply(QStringLiteral("invalid-parent"));
        }
        // Chrome: a missing url means a folder.
        const bool isBookmark = props.contains(QLatin1String("url"));
        const QString url =
            props.value(QLatin1String("url")).toString();
        if (isBookmark && !bmUrlAllowed(url))
            return errReply(QStringLiteral("scheme-refused"));
        const int count = parent->children().count();
        int index = props.value(QLatin1String("index")).toInt(-1);
        if (index < 0)
            index = count;
        else if (index > count)
            return errReply(QStringLiteral("invalid-argument"));
        BookmarkNode *node = new BookmarkNode(
            isBookmark ? BookmarkNode::Bookmark
                       : BookmarkNode::Folder);
        node->title = props.value(QLatin1String("title")).toString();
        node->url = url;
        {
            const BmApiSuppress suppress;
            manager->addBookmark(parent, node, index);
        }
#ifdef ARKTOS_RUSTCORE
        if (parent->handle() && !node->handle()) {
            // The store refused behind the wrapper (the url gate is
            // pre-checked, so this is the belt-and-suspenders leg):
            // back the insert out through the same undo machinery.
            const BmApiSuppress suppress;
            manager->undoRedoStack()->undo();
            return errReply(QStringLiteral("create-refused"));
        }
#endif
        bookmarksFanOut(
            QStringLiteral("bookmarks.onCreated"),
            QJsonArray{bmId(manager, node),
                       bmNodeJson(manager, node, false)});
        return okReply(bmNodeJson(manager, node, false));
    }

    if (method == QLatin1String("move")) {
        const QString id = args.value(QLatin1String("id")).toString();
        if (id.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        BookmarkNode *node = bmResolve(manager, id);
        if (!node)
            return errReply(QStringLiteral("not-found"));
        // Chrome's root folders are fixed — root, bar and menu all
        // refuse a move.
        if (bmIsSpecial(manager, node))
            return errReply(QStringLiteral("cannot-modify"));
        BookmarkNode *oldParent = node->parent();
        const int oldIndex = oldParent->children().indexOf(node);
        const QJsonObject dest =
            args.value(QLatin1String("destination")).toObject();
        BookmarkNode *newParent = oldParent;
        if (dest.contains(QLatin1String("parentId"))) {
            newParent = bmResolve(
                manager,
                dest.value(QLatin1String("parentId")).toString());
            if (!newParent)
                return errReply(QStringLiteral("not-found"));
            if (newParent->type() == BookmarkNode::Bookmark
                    || newParent->type() == BookmarkNode::Separator)
                return errReply(QStringLiteral("invalid-parent"));
        }
        // Re-parenting a folder into its own subtree is a cycle —
        // the store attach refuses it too, but only after the C++
        // list already mutated, so it is gated here.
        for (BookmarkNode *walk = newParent; walk;
                walk = walk->parent()) {
            if (walk == node)
                return errReply(QStringLiteral("invalid-argument"));
        }
        const bool hasIndex = dest.contains(QLatin1String("index"));
        int index = dest.value(QLatin1String("index")).toInt(-1);
        // Chrome's index is the FINAL position — a same-folder move
        // extracts the node first, so its valid ceiling there is one
        // below the current child count.
        const int indexCap =
            (newParent == oldParent)
                ? newParent->children().count() - 1
                : newParent->children().count();
        if (hasIndex && (index < 0 || index > indexCap))
            return errReply(QStringLiteral("invalid-argument"));
        if (newParent == oldParent && !hasIndex)
            return okReply(bmNodeJson(manager, node, false));
        {
            const BmApiSuppress suppress;
            // One undoable op — the remove+insert pair is Chrome's
            // single move, and the events it would double-report
            // stay suppressed while onMoved fires below.
            manager->undoRedoStack()->beginMacro(tr("Move Bookmark"));
            manager->removeBookmark(node);
            manager->addBookmark(newParent, node, index);
            manager->undoRedoStack()->endMacro();
        }
        bookmarksFanOut(
            QStringLiteral("bookmarks.onMoved"),
            QJsonArray{id,
                       QJsonObject{
                           {QStringLiteral("parentId"),
                            bmId(manager, newParent)},
                           {QStringLiteral("index"),
                            newParent->children().indexOf(node)},
                           {QStringLiteral("oldParentId"),
                            bmId(manager, oldParent)},
                           {QStringLiteral("oldIndex"), oldIndex}}});
        return okReply(bmNodeJson(manager, node, false));
    }

    if (method == QLatin1String("update")) {
        const QString id = args.value(QLatin1String("id")).toString();
        if (id.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        BookmarkNode *node = bmResolve(manager, id);
        if (!node)
            return errReply(QStringLiteral("not-found"));
        if (bmIsSpecial(manager, node))
            return errReply(QStringLiteral("cannot-modify"));
        const QJsonObject changes =
            args.value(QLatin1String("changes")).toObject();
        const bool hasTitle = changes.contains(QLatin1String("title"));
        const bool hasUrl = changes.contains(QLatin1String("url"));
        const QString title =
            changes.value(QLatin1String("title")).toString();
        const QString url =
            changes.value(QLatin1String("url")).toString();
        if (hasUrl && node->type() != BookmarkNode::Bookmark)
            return errReply(QStringLiteral("invalid-argument"));
        // Extension-side edits apply the insert-time gate (the
        // store's own set_url would let javascript: through as a
        // 'deliberate edit' — code is not a human consenting).
        if (hasUrl && !bmUrlAllowed(url))
            return errReply(QStringLiteral("scheme-refused"));
        bool changed = false;
        {
            const BmApiSuppress suppress;
            if (hasTitle && title != node->title) {
                manager->setTitle(node, title);
                changed = true;
            }
            if (hasUrl && url != node->url) {
                manager->setUrl(node, url);
                changed = true;
            }
        }
        if (changed) {
            QJsonObject changeInfo{
                {QStringLiteral("title"), node->title}};
            if (node->type() == BookmarkNode::Bookmark)
                changeInfo.insert(QStringLiteral("url"), node->url);
            bookmarksFanOut(QStringLiteral("bookmarks.onChanged"),
                            QJsonArray{id, changeInfo});
        }
        return okReply(bmNodeJson(manager, node, false));
    }

    if (method == QLatin1String("remove")) {
        const QString id = args.value(QLatin1String("id")).toString();
        if (id.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        BookmarkNode *node = bmResolve(manager, id);
        if (!node)
            return errReply(QStringLiteral("not-found"));
        if (bmIsSpecial(manager, node))
            return errReply(QStringLiteral("cannot-modify"));
        // Chrome's remove() deletes a bookmark or an EMPTY folder —
        // subtrees go through removeTree.
        if (node->type() != BookmarkNode::Bookmark
                && !node->children().isEmpty())
            return errReply(QStringLiteral("folder-not-empty"));
        BookmarkNode *parent = node->parent();
        const QJsonObject removeInfo{
            {QStringLiteral("parentId"), bmId(manager, parent)},
            {QStringLiteral("index"),
             parent->children().indexOf(node)},
            {QStringLiteral("node"),
             bmNodeJson(manager, node, true)}};
        {
            const BmApiSuppress suppress;
            manager->removeBookmark(node);
        }
        bookmarksFanOut(QStringLiteral("bookmarks.onRemoved"),
                        QJsonArray{id, removeInfo});
        return okReply();
    }

    if (method == QLatin1String("removeTree")) {
        const QString id = args.value(QLatin1String("id")).toString();
        if (id.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        BookmarkNode *node = bmResolve(manager, id);
        if (!node)
            return errReply(QStringLiteral("not-found"));
        if (bmIsSpecial(manager, node))
            return errReply(QStringLiteral("cannot-modify"));
        BookmarkNode *parent = node->parent();
        const QJsonObject removeInfo{
            {QStringLiteral("parentId"), bmId(manager, parent)},
            {QStringLiteral("index"),
             parent->children().indexOf(node)},
            {QStringLiteral("node"),
             bmNodeJson(manager, node, true)}};
        {
            const BmApiSuppress suppress;
            manager->removeBookmark(node);
        }
        bookmarksFanOut(QStringLiteral("bookmarks.onRemoved"),
                        QJsonArray{id, removeInfo});
        return okReply();
    }

    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.history — MD22.  The real visit log served over the RC04
// store through HistoryManager (the C++ mirror is the facade/FFI
// lane — under rustcore it persists to history.db, under the Qt
// fallback to the legacy file, both through the ops the UI rides).
// The 'history' manifest permission gates the whole namespace and
// the onVisited/onVisitRemoved fan-out (eventGate).  Api-driven
// deletions suppress the signal tap while they emit their own
// Chrome-accurate onVisitRemoved; UI-driven changes (page visits,
// dialog deletes, Clear History) still reach 'history'-declaring
// contexts through it.
// -------------------------------------------------------------------

void ExtensionShimBackend::historyFanOut(
        const QString &name, const QJsonArray &eventArgs)
{
    const QJsonObject payload{
        {QStringLiteral("__args"), eventArgs}};
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->broadcastEvent(name, payload);
    // Same bare-backend fallback bookmarksFanOut applies — tests
    // and mid-respawn hosts have no registered context.
    if (!runtime->hasRelayContext(m_extensionId))
        emit pushEvent(name, payload);
}

QJsonObject ExtensionShimBackend::invokeHistory(
        const QString &method, const QJsonObject &args)
{
    // ER16 shape: Chrome exposes the namespace only under the
    // declared 'history' grant — the bridge object answers a direct
    // call from any code in the context, so the gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("history")))
        return errReply(QStringLiteral("no-permission"));

    HistoryManager *manager = HistoryManager::instance();
    if (!manager)
        return errReply(QStringLiteral("no-store"));
    tapHistEvents(manager);
    const QList<HistoryEntry> entries = manager->history();

    if (method == QLatin1String("search")) {
        // Chrome's query object: 'text' splits into space-delimited
        // terms each AND-matched case-insensitively against url or
        // title ('' matches all); the time range admits an item when
        // ANY of its visits lands inside it (startTime defaults to
        // the last 24h, endTime is open); maxResults defaults 100,
        // minimum 1.
        if (!args.value(QLatin1String("query")).isObject())
            return errReply(QStringLiteral("invalid-argument"));
        const QJsonObject query =
            args.value(QLatin1String("query")).toObject();
        const QStringList rawTerms = query.value(QLatin1String("text"))
            .toString().split(QRegularExpression(QStringLiteral("\\s+")),
                              Qt::SkipEmptyParts);
        QStringList terms;
        for (const QString &t : rawTerms)
            terms.append(t.toLower());
        const qint64 startMs = query.contains(QLatin1String("startTime"))
            ? qint64(query.value(QLatin1String("startTime")).toDouble())
            : QDateTime::currentMSecsSinceEpoch() - 86400000;
        const qint64 endMs = query.contains(QLatin1String("endTime"))
            ? qint64(query.value(QLatin1String("endTime")).toDouble())
            : std::numeric_limits<qint64>::max();
        int maxResults = 100;
        if (query.contains(QLatin1String("maxResults"))) {
            maxResults = query.value(QLatin1String("maxResults")).toInt();
            if (maxResults < 1)
                return errReply(QStringLiteral("invalid-argument"));
        }
        // Group the newest-first listing per url; first occurrence
        // claims the order slot (descending lastVisitTime) and the
        // newest visit's title.
        struct HistGroup {
            QString title;
            qint64 lastTs;
            int visits = 0;
            bool inRange = false;
        };
        QStringList order;
        QHash<QString, HistGroup> groups;
        for (const HistoryEntry &e : entries) {
            HistGroup &g = groups[e.url];
            if (!g.visits) {
                order.append(e.url);
                g.title = e.title;
                g.lastTs = e.dateTime.toMSecsSinceEpoch();
            }
            ++g.visits;
            const qint64 ts = e.dateTime.toMSecsSinceEpoch();
            if (ts >= startMs && ts <= endMs)
                g.inRange = true;
            if (g.title.isEmpty() && !e.title.isEmpty())
                g.title = e.title;
        }
        QJsonArray out;
        for (const QString &url : order) {
            const HistGroup &g = groups.value(url);
            if (!g.inRange)
                continue;
            const QString urlL = url.toLower();
            const QString titleL = g.title.toLower();
            bool match = true;
            for (const QString &term : terms) {
                if (!urlL.contains(term) && !titleL.contains(term)) {
                    match = false;
                    break;
                }
            }
            if (!match)
                continue;
            out.append(histItemJson(url, g.title, g.lastTs, g.visits));
            if (out.size() >= maxResults)
                break;
        }
        return okReply(out);
    }

    if (method == QLatin1String("getVisits")) {
        const QString url = args.value(QLatin1String("url")).toString();
        if (url.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        if (!m_histTransitionNoted) {
            m_histTransitionNoted = true;
            recordNote(QStringLiteral(
                "history.getVisits: the store carries no transition "
                "column — 'link' answers every visit, referringVisitId "
                "is always \"0\", and typedCount is 0"));
        }
        return okReply(histVisitItems(entries, url));
    }

    if (method == QLatin1String("addUrl")) {
        // Chrome: a 'link'-transition visit recorded at the current
        // time — addHistoryEntry is exactly that (it cleans the url
        // the same way a page visit does).  The entryAdded tap is
        // suppressed so onVisited emits once, Chrome-accurate, with
        // the url's aggregated HistoryItem.
        const QString url = args.value(QLatin1String("url")).toString();
        const QUrl parsed(url);
        if (url.isEmpty() || !parsed.isValid())
            return errReply(QStringLiteral("invalid-argument"));
        if (!histUrlAllowed(url))
            return errReply(QStringLiteral("scheme-refused"));
        {
            const HistApiSuppress suppress;
            manager->addHistoryEntry(url);
        }
        const QList<HistoryEntry> after = manager->history();
        if (!after.isEmpty()) {
            const QJsonObject histItem =
                histItemForUrl(after, after.first().url);
            if (!histItem.isEmpty())
                historyFanOut(QStringLiteral("history.onVisited"),
                              QJsonArray{histItem});
        }
        return okReply();
    }

    if (method == QLatin1String("deleteUrl")) {
        const QString url = args.value(QLatin1String("url")).toString();
        if (url.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        {
            const HistApiSuppress suppress;
            for (const HistoryEntry &e : entries) {
                if (e.url == url)
                    manager->removeHistoryEntry(e);
            }
        }
        historyFanOut(QStringLiteral("history.onVisitRemoved"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("allHistory"), false},
                          {QStringLiteral("urls"),
                           QJsonArray{url}}}});
        return okReply();
    }

    if (method == QLatin1String("deleteRange")) {
        // Chrome requires both bounds; the range is inclusive —
        // every visit inside it goes, urls outside it stay.
        if (!args.contains(QLatin1String("startTime"))
                || !args.contains(QLatin1String("endTime")))
            return errReply(QStringLiteral("invalid-argument"));
        const qint64 startMs =
            qint64(args.value(QLatin1String("startTime")).toDouble());
        const qint64 endMs =
            qint64(args.value(QLatin1String("endTime")).toDouble());
        QList<HistoryEntry> doomed;
        QSet<QString> seen;
        QStringList removedUrls;
        for (const HistoryEntry &e : entries) {
            const qint64 ts = e.dateTime.toMSecsSinceEpoch();
            if (ts < startMs || ts > endMs)
                continue;
            doomed.append(e);
            if (!seen.contains(e.url)) {
                seen.insert(e.url);
                removedUrls.append(e.url);
            }
        }
        {
            const HistApiSuppress suppress;
            for (const HistoryEntry &e : doomed)
                manager->removeHistoryEntry(e);
        }
        QJsonArray urls;
        for (const QString &u : removedUrls)
            urls.append(u);
        historyFanOut(QStringLiteral("history.onVisitRemoved"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("allHistory"), false},
                          {QStringLiteral("urls"), urls}}});
        return okReply();
    }

    if (method == QLatin1String("deleteAll")) {
        {
            const HistApiSuppress suppress;
            manager->clear();
        }
        historyFanOut(QStringLiteral("history.onVisitRemoved"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("allHistory"), true},
                          {QStringLiteral("urls"), QJsonArray()}}});
        return okReply();
    }

    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.topSites — MD27.  Chrome's MostVisitedURL is the per-host
// "new tab tile" entry {url, title}; the ranking decision lives in
// the RC04 hosts rollup (the same table TRUST01 and the omnibox
// suggest lane build on) via rc_history_top_hosts — visit count
// descending, earliest first_seen breaking ties, the host's newest
// visit supplying the representative url/title.  The C++ lane
// aggregates the identical ranking over HistoryManager's
// newest-first listing so a no-rust build or an FFI failure still
// answers honestly.  Rows: [{host, url, title, visits, first_seen}].
// -------------------------------------------------------------------

QJsonArray topSiteRows()
{
#ifdef ARKTOS_RUSTCORE
    // Facade handle binds the ambient persona's history.db for the
    // query — same posture the completer's RustHistorySuggestModel
    // takes; the legacy spelling anchors the path by hand when the
    // bridge cannot serve.
    AcHistory *h = nullptr;
    const QString profileId = BrowserPaths::activeProfileId();
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->history(profileId);
    if (!h) {
        const QByteArray dbPath = BrowserPaths::profileDataFilePath(
            profileId, QLatin1String("history.db")).toUtf8();
        rc_hist_open(dbPath.constData());
    }
    // Chrome's tile list stays small — 20 bounds the marshal.
    char *raw = h
        ? ac_hist_top_hosts(h, 20)
        : rc_history_top_hosts(20);
    ac_hist_close(h);
    if (raw) {
        const QByteArray out(raw);
        if (h)
            ac_string_free(raw);
        else
            rc_string_free(raw);
        const QJsonArray rows = QJsonDocument::fromJson(out).array();
        if (!rows.isEmpty() || out.trimmed() == "[]")
            return rows;
        // A malformed reply falls through to the C++ aggregate —
        // the manager still carries the same store's truth.
    }
#endif
    HistoryManager *manager = HistoryManager::instance();
    if (!manager)
        return QJsonArray();
    struct HostAgg {
        QString url;
        QString title;
        qint64 firstTs = 0;
        int visits = 0;
    };
    // Newest-first listing: the first occurrence per host keeps the
    // representative url/title; every visit counts toward the rank.
    QStringList order;
    QHash<QString, HostAgg> agg;
    for (const HistoryEntry &e : manager->history()) {
        const QString host = QUrl(e.url).host().toLower();
        if (host.isEmpty())
            continue;
        HostAgg &a = agg[host];
        if (!a.visits) {
            order.append(host);
            a.url = e.url;
            a.title = e.title;
            a.firstTs = e.dateTime.toMSecsSinceEpoch();
        } else {
            a.firstTs = qMin(a.firstTs,
                             e.dateTime.toMSecsSinceEpoch());
        }
        ++a.visits;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](const QString &x, const QString &y) {
                         const HostAgg &a = agg.value(x);
                         const HostAgg &b = agg.value(y);
                         if (a.visits != b.visits)
                             return a.visits > b.visits;
                         return a.firstTs < b.firstTs;
                     });
    QJsonArray rows;
    for (const QString &host : order) {
        const HostAgg &a = agg.value(host);
        rows.append(QJsonObject{
            {QStringLiteral("host"), host},
            {QStringLiteral("url"), a.url},
            {QStringLiteral("title"), a.title},
            {QStringLiteral("visits"), a.visits},
            {QStringLiteral("first_seen"), a.firstTs}});
        if (rows.size() >= 20)
            break;
    }
    return rows;
}

QJsonObject ExtensionShimBackend::invokeTopSites(
        const QString &method, const QJsonObject &args)
{
    Q_UNUSED(args);
    // Chrome exposes get() only under the declared 'topSites' grant —
    // the bridge object answers a direct call from any code in the
    // context, so the gate runs here too (the task card's "no gate"
    // line predates the Chrome docs check: developer.chrome.com
    // lists topSites as a required permission).
    if (!manifestPermissions().contains(QLatin1String("topSites")))
        return errReply(QStringLiteral("no-permission"));

    if (method == QLatin1String("get")) {
        QJsonArray out;
        for (const QJsonValue &v : topSiteRows()) {
            const QJsonObject row = v.toObject();
            const QString url =
                row.value(QLatin1String("url")).toString();
            if (url.isEmpty())
                continue;   // no representative visit recorded
            out.append(QJsonObject{
                {QStringLiteral("url"), url},
                {QStringLiteral("title"),
                 row.value(QLatin1String("title")).toString()}});
        }
        return okReply(out);
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.browsingData — MD28.  Chrome's bulk-clear API over the CLR01
// plan machinery: remove() and the five served remove* convenience
// members map Chrome's DataTypeSet onto ClearPrivateData category
// bits, and the same rc_clearplan/planQt planner the dialog runs
// decides the ops and the persistent/OTR scope.  'browsingData'
// gates the namespace (Chrome lists it as a required permission).
//
// Served dataTypes: history, downloads, cookies, cache, formData —
// the stores the plan vocabulary owns (formData has no plan op; the
// autofill wipe runs direct, same as accept()).  The other nine
// Chrome dataTypes (appcache, cacheStorage, fileSystems, indexedDB,
// localStorage, passwords, pluginData, serviceWorkers, webSQL) name
// stores this build cannot remove selectively — a remove() that
// asks for them reports them in the result's 'unsupported' list and
// a dedicated removeX() answers the typed verdict; never silently
// skipped.  RemovalOptions scoping this build cannot honor —
// origins/excludeOrigins, non-web originTypes — is a typed
// 'unsupported' too: widening it to a whole-store wipe would delete
// data the caller scoped out.
//
// RemovalOptions.since is an absolute ms-epoch stamp; the plan's
// window vocabulary carries it exactly via "secs:<n>" — the ranged
// visit log honors the window, the all-or-nothing stores wipe whole
// and are reported under 'degraded'.
// -------------------------------------------------------------------

// Chrome DataTypeSet -> the CLR01 category bits.  formData returns 0
// here — the autofill store has no plan op and wipes direct, the
// same path ClearPrivateData::accept() runs.
static quint32 browsingDataCategory(const QString &type)
{
    if (type == QLatin1String("history"))
        return ClearPrivateData::ClearHistory;
    if (type == QLatin1String("downloads"))
        return ClearPrivateData::ClearDownloads;
    if (type == QLatin1String("cookies"))
        return ClearPrivateData::ClearCookies;
    if (type == QLatin1String("cache"))
        return ClearPrivateData::ClearCache;
    return 0;
}

// The Chrome vocabulary beyond the five served names — kept explicit
// so settings() can answer dataRemovalPermitted:false for each and
// remove() can tell a known-unserved type from an unrecognized key
// (an unrecognized key in DataTypeSet is ignored, like Chrome).
static const QStringList &browsingDataUnserved()
{
    static const QStringList unserved = {
        QStringLiteral("appcache"), QStringLiteral("cacheStorage"),
        QStringLiteral("fileSystems"), QStringLiteral("indexedDB"),
        QStringLiteral("localStorage"), QStringLiteral("passwords"),
        QStringLiteral("pluginData"), QStringLiteral("serviceWorkers"),
        QStringLiteral("webSQL"),
    };
    return unserved;
}

// Every removeXyz() convenience member Chrome publishes is remove()
// with a fixed one-member DataTypeSet — the member name camel-cases
// the dataType, so removeHistory -> {history:true}.  An empty string
// means the name is not a remove* member at all.
static QString browsingDataFixedType(const QString &method)
{
    if (!method.startsWith(QLatin1String("remove"))
            || method == QLatin1String("remove")
            || method.size() <= 6)
        return QString();
    QString type = method.mid(6);
    type[0] = type[0].toLower();
    return type;
}

// settings() — Chrome answers {options, dataToRemove,
// dataRemovalPermitted} from the browser's own clear dialog.  The
// honest local mapping is the persisted ClearPrivateData checkbox
// state for the five served types; dataRemovalPermitted is the
// capability truth — served types true, the rest false.
static QJsonObject browsingDataSettings()
{
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("clearprivatedata"));
    QJsonObject dataToRemove{
        {QStringLiteral("history"),
         settings.value(QLatin1String("browsingHistory"), true).toBool()},
        {QStringLiteral("downloads"),
         settings.value(QLatin1String("downloadHistory"), true).toBool()},
        {QStringLiteral("cookies"),
         settings.value(QLatin1String("cookies"), true).toBool()},
        {QStringLiteral("cache"),
         settings.value(QLatin1String("cache"), true).toBool()},
        {QStringLiteral("formData"),
         settings.value(QLatin1String("autofill"), true).toBool()},
    };
    settings.endGroup();

    QJsonObject permitted = dataToRemove;
    for (const QString &type : browsingDataUnserved()) {
        dataToRemove.insert(type, false);
        permitted.insert(type, false);
    }

    return QJsonObject{
        {QStringLiteral("options"), QJsonObject{
            {QStringLiteral("since"), 0},
            {QStringLiteral("originTypes"), QJsonObject{
                {QStringLiteral("unprotectedWeb"), true},
                {QStringLiteral("protectedWeb"), false},
                {QStringLiteral("extension"), false}}},
        }},
        {QStringLiteral("dataToRemove"), dataToRemove},
        {QStringLiteral("dataRemovalPermitted"), permitted},
    };
}

// remove(options, dataToRemove) — the shared body for remove() and
// every served remove* member.  Executes the served subset and
// reports the rest; a call whose WHOLE requested set is unservable
// answers the typed verdict instead.
static QJsonObject browsingDataRemove(const QJsonValue &optionsValue,
                                      const QJsonObject &dataToRemove)
{
    QJsonObject options;
    if (optionsValue.isObject())
        options = optionsValue.toObject();
    else if (!optionsValue.isUndefined())
        return errReply(QStringLiteral("invalid-argument"));

    // 'since' is an absolute ms-epoch stamp — the plan vocabulary
    // carries it verbatim as a "secs:<n>" window so the ranged visit
    // log honors exactly the window the extension asked for.
    QString period = QLatin1String("all");
    bool windowed = false;
    const QJsonValue sinceValue = options.value(QLatin1String("since"));
    if (!sinceValue.isUndefined()) {
        const double since = sinceValue.toDouble();
        // !(s >= 0) catches NaN and negatives alike.
        if (!sinceValue.isDouble() || !(since >= 0))
            return errReply(QStringLiteral("invalid-argument"));
        if (since > 0) {
            // Round up to whole seconds — over-clearing is the
            // honest direction for a privacy wipe; a future stamp
            // (or one beyond ms precision) clamps to a 1s window
            // rather than meaning "all".
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 sinceMs =
                since >= double(nowMs) ? nowMs : qint64(since);
            const qint64 secs =
                qMax<qint64>(1, (nowMs - sinceMs + 999) / 1000);
            period = QStringLiteral("secs:%1").arg(secs);
            windowed = true;
        }
    }

    // originTypes: every store the plan touches is regular-web data
    // (unprotectedWeb).  Scopes outside it are reported, never
    // silently dropped; per-origin limits (origins/excludeOrigins)
    // cannot be honored by whole-store ops, so they refuse — a
    // degraded all-store wipe would remove data the caller scoped
    // out.
    const QJsonValue originTypes =
        options.value(QLatin1String("originTypes"));
    bool unprotectedWeb = true;
    QStringList unsupportedScopes;
    if (originTypes.isObject()) {
        const QJsonObject ot = originTypes.toObject();
        if (ot.contains(QLatin1String("unprotectedWeb")))
            unprotectedWeb =
                ot.value(QLatin1String("unprotectedWeb")).toBool();
        if (ot.value(QLatin1String("protectedWeb")).toBool())
            unsupportedScopes << QStringLiteral("protectedWeb");
        if (ot.value(QLatin1String("extension")).toBool())
            unsupportedScopes << QStringLiteral("extension");
    } else if (!originTypes.isUndefined()) {
        return errReply(QStringLiteral("invalid-argument"));
    }
    for (const char *key : {"origins", "excludeOrigins"}) {
        const QJsonValue v = options.value(QLatin1String(key));
        if (v.isUndefined())
            continue;
        if (!v.isArray())
            return errReply(QStringLiteral("invalid-argument"));
        if (!v.toArray().isEmpty())
            return errReply(QStringLiteral("unsupported"));
    }

    // Split the requested set: explicit false members are not
    // requested (Chrome's DataTypeSet semantics); true members split
    // into served stores and typed-unserved ones.
    quint32 categories = 0;
    bool wantFormData = false;
    QStringList served, unserved;
    for (auto it = dataToRemove.constBegin();
         it != dataToRemove.constEnd(); ++it) {
        if (!it.value().toBool())
            continue;
        const QString type = it.key();
        const quint32 category = browsingDataCategory(type);
        if (category) {
            categories |= category;
            served << type;
        } else if (type == QLatin1String("formData")) {
            wantFormData = true;
            served << type;
        } else if (browsingDataUnserved().contains(type)) {
            unserved << type;
        }
        // Unrecognized keys are ignored like Chrome's dictionary.
    }
    if (!unprotectedWeb) {
        // The regular-web scope is explicitly off — every served
        // type was requested under scopes this build cannot reach.
        unserved << served;
        served.clear();
        categories = 0;
        wantFormData = false;
    }
    if (served.isEmpty() && !unserved.isEmpty())
        return errReply(QStringLiteral("unsupported"));

    // The decision — which ops, in which scope — is the planner's;
    // Qt only executes.  The scope mirrors ClearPrivateData::accept():
    // a private-context request gets the OTR plan whose ops can never
    // reach the persistent profile.
    const bool otr = BrowserApplication::isPrivate();
    const QJsonArray ops =
        ClearPrivateData::plan(period, categories, otr);
    ClearPrivateData::executePlan(ops);
    // formData has no plan op — the autofill store wipes direct, the
    // same path accept() runs.  An OTR context writes no autofill
    // data, so a private-requested formData is satisfied vacuously.
    if (wantFormData && !otr)
        AutoFillManager::instance()->clearData();

    QJsonObject result;
    result.insert(QStringLiteral("removed"),
                  QJsonArray::fromStringList(served));
    if (!unserved.isEmpty())
        result.insert(QStringLiteral("unsupported"),
                      QJsonArray::fromStringList(unserved));
    if (!unsupportedScopes.isEmpty())
        result.insert(QStringLiteral("unsupportedScopes"),
                      QJsonArray::fromStringList(unsupportedScopes));
    if (windowed && !served.isEmpty()) {
        // The visit log is the only ranged store — every requested
        // type ends up at least partially beyond the window (the
        // visited-link companion op is unranged too), so report the
        // widening on all of them.
        result.insert(QStringLiteral("degraded"),
                      QJsonArray::fromStringList(served));
    }
    return okReply(result);
}

QJsonObject ExtensionShimBackend::invokeBrowsingData(
        const QString &method, const QJsonObject &args)
{
    // Chrome exposes the namespace only under the declared
    // 'browsingData' grant — the bridge object answers a direct call
    // from any code in the context, so the gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("browsingData")))
        return errReply(QStringLiteral("no-permission"));

    if (!m_browsingDataNoted) {
        m_browsingDataNoted = true;
        recordNote(QStringLiteral(
            "chrome.browsingData: served subset is history, downloads, "
            "cookies, cache and formData over the CLR01 plan; the other "
            "nine Chrome dataTypes answer typed unsupported; a 'since' "
            "window widens to a full wipe on the all-or-nothing "
            "stores"));
    }

    if (method == QLatin1String("settings"))
        return okReply(browsingDataSettings());

    if (method == QLatin1String("remove")) {
        // Chrome requires the DataTypeSet argument.
        const QJsonValue dataToRemove =
            args.value(QLatin1String("dataToRemove"));
        if (!dataToRemove.isObject())
            return errReply(QStringLiteral("invalid-argument"));
        return browsingDataRemove(args.value(QLatin1String("options")),
                                  dataToRemove.toObject());
    }

    const QString fixed = browsingDataFixedType(method);
    if (!fixed.isEmpty()) {
        const quint32 category = browsingDataCategory(fixed);
        const bool served = category
            || fixed == QLatin1String("formData");
        if (!served && !browsingDataUnserved().contains(fixed))
            return errReply(QStringLiteral("unimplemented"));
        if (!served)
            return errReply(QStringLiteral("unsupported"));
        return browsingDataRemove(
            args.value(QLatin1String("options")),
            QJsonObject{{fixed, true}});
    }

    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.dns — MD29.  Chrome publishes resolve() alone (and the
// namespace only exists on ChromeOS upstream — here it answers for
// every hosted context under the declared 'dns' grant).  The system
// resolver runs off-thread behind QHostInfo::lookupHost, so the
// reply is deferred like downloads.download: the invoke answers a
// ticket and the verdict arrives on '__arktos.dnsResolved' —
// {ticket,addresses:[...]} on success, {ticket,error:'resolve-
// failed'} when the resolver reports an error or the bounded wait
// expires.  Only the calling extension sees the push (every other
// context of it shares the ticket space anyway — EXT17 backends are
// per-extension, not per-context).
// -------------------------------------------------------------------

struct DnsPending {
    int ticket = 0;
    QString extId;
    QPointer<ExtensionShimBackend> backend;
    int lookupId = -1;
};

QList<DnsPending> &dnsPendingList()
{
    static QList<DnsPending> list;
    return list;
}

int &dnsTicketSeq()
{
    static int seq = 0;
    return seq;
}

// The deferred dns.resolve answer — same discipline dlAnswerTicket
// applies: the relay reaches the extension's registered contexts; a
// bare backend with none (tests, mid-respawn) hears it on pushEvent.
void dnsAnswerTicket(const DnsPending &pending, const QJsonObject &reply)
{
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->deliverEvent(pending.extId,
        QStringLiteral("__arktos.dnsResolved"), reply);
    if (pending.backend && !runtime->hasRelayContext(pending.extId))
        emit pending.backend->pushEvent(
            QStringLiteral("__arktos.dnsResolved"), reply);
}

void dnsFinishTicket(int ticket, const QJsonObject &reply)
{
    QList<DnsPending> &list = dnsPendingList();
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).ticket != ticket)
            continue;
        dnsAnswerTicket(list.takeAt(i), reply);
        return;
    }
}

QJsonObject ExtensionShimBackend::invokeDns(
        const QString &method, const QJsonObject &args)
{
    // Chrome exposes the namespace only under the declared 'dns'
    // grant — the bridge object answers a direct call from any code
    // in the context, so the gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("dns")))
        return errReply(QStringLiteral("no-permission"));

    if (method != QLatin1String("resolve"))
        return errReply(QStringLiteral("unimplemented"));

    const QJsonValue hostnameValue =
        args.value(QLatin1String("hostname"));
    if (!hostnameValue.isString())
        return errReply(QStringLiteral("invalid-argument"));
    const QString hostname = hostnameValue.toString().trimmed();
    // A name getaddrinfo cannot accept (empty, over the 253-byte DNS
    // name bound) is the argument verdict — no resolver round-trip.
    if (hostname.isEmpty()
            || hostname.toUtf8().size() > 253)
        return errReply(QStringLiteral("invalid-argument"));

    QObject *context = QCoreApplication::instance();
    if (!context)
        // No event loop can ever deliver the lookup — refuse at the
        // ticket rather than stranding the JS waiter.
        return errReply(QStringLiteral("resolve-failed"));

    const int ticket = ++dnsTicketSeq();
    DnsPending pending;
    pending.ticket = ticket;
    pending.extId = m_extensionId;
    pending.backend = this;
    dnsPendingList().append(pending);

    // The system resolver can wait out its own retry budget on a
    // dead upstream — bound the ticket so the JS waiter cannot hang
    // forever (the same bound downloads.download's ticket gets).
    QTimer::singleShot(30000, context, [ticket]() {
        QList<DnsPending> &list = dnsPendingList();
        for (int i = 0; i < list.size(); ++i) {
            if (list.at(i).ticket != ticket)
                continue;
            const DnsPending p = list.takeAt(i);
            if (p.lookupId > 0)
                QHostInfo::abortHostLookup(p.lookupId);
            dnsAnswerTicket(p, QJsonObject{
                {QStringLiteral("ticket"), ticket},
                {QStringLiteral("error"),
                 QStringLiteral("resolve-failed")},
                {QStringLiteral("detail"),
                 QStringLiteral("lookup timed out")}});
            return;
        }
    });
    pending.lookupId = QHostInfo::lookupHost(
        hostname, context, [ticket](const QHostInfo &info) {
            QJsonObject reply{{QStringLiteral("ticket"), ticket}};
            if (info.error() == QHostInfo::NoError) {
                QJsonArray addresses;
                for (const QHostAddress &address : info.addresses())
                    addresses.append(address.toString());
                reply.insert(QStringLiteral("addresses"), addresses);
            } else {
                reply.insert(QStringLiteral("error"),
                             QStringLiteral("resolve-failed"));
                reply.insert(QStringLiteral("detail"),
                             info.errorString());
            }
            dnsFinishTicket(ticket, reply);
        });
    // lookupHost ran synchronously only in the never-happens
    // immediate-failure shape (id 0) — stamp the real id onto the
    // pending row so the timeout above can abort the query.
    for (DnsPending &p : dnsPendingList()) {
        if (p.ticket == ticket)
            p.lookupId = pending.lookupId;
    }

    return okReply(QJsonObject{{QStringLiteral("ticket"), ticket}});
}

// -------------------------------------------------------------------
// chrome.system — MD30.  Chrome's honest-machine-facts namespace.
// Each sub-namespace gates on its own 'system.<sub>' declared grant
// like Chrome (system.memory/cpu/display/storage/...) — the gate
// runs BEFORE the surface check so an extension that declares a
// grant for a sub Arktos cannot serve (network, powerSource, the
// ChromeOS-only surfaces) hears 'unsupported' rather than
// 'no-permission': the grant was honored, the surface is missing.
// Every answer is measured, never faked: memory rides
// /proc/meminfo with a sysconf fallback, cpu rides QSysInfo +
// /proc/cpuinfo + /proc/stat per-core jiffies, display enumerates
// the live QScreen list, storage answers QStorageInfo on the
// volumes that actually hold profile state.  Members Chrome
// publishes that have no Arktos surface (storage.ejectDevice, the
// ChromeOS-only display mutators) keep typed 'unsupported'.
// -------------------------------------------------------------------

// MemTotal/MemAvailable in bytes — /proc/meminfo on Linux, the
// sysconf page counters elsewhere on POSIX.  -1 = not measurable;
// the caller refuses rather than reporting a fake capacity.
static void systemMemoryBytes(qint64 *total, qint64 *available)
{
    *total = -1;
    *available = -1;
#if defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/meminfo"));
    if (file.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = file.readAll().split('\n');
        for (const QByteArray &line : lines) {
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray key = line.left(colon).trimmed();
            if (key != "MemTotal" && key != "MemAvailable")
                continue;
            // "  16384000 kB" — the number is the first field.
            const qint64 kb = line.mid(colon + 1).simplified()
                .split(' ').value(0).toLongLong();
            if (key == "MemTotal")
                *total = kb * 1024;
            else
                *available = kb * 1024;
        }
    }
#endif
    if (*total < 0) {
#if defined(Q_OS_UNIX)
        const long pageSize = sysconf(_SC_PAGESIZE);
        const long pages = sysconf(_SC_PHYS_PAGES);
        const long freePages = sysconf(_SC_AVPHYS_PAGES);
        if (pageSize > 0 && pages > 0)
            *total = qint64(pages) * pageSize;
        if (pageSize > 0 && freePages >= 0)
            *available = qint64(freePages) * pageSize;
#endif
    }
}

// system.cpu.getInfo — Chrome's CpuInfo shape, every field measured:
// archName mapped onto Chrome's vocabulary (unrecognized spellings
// pass through raw — a real name is more honest than claiming
// 'x86'), modelName/features off /proc/cpuinfo, numOfProcessors off
// the scheduler's view, processors off /proc/stat's per-core
// cumulative jiffies converted to seconds.  A build with no way to
// measure per-core time answers an empty processors array — Chrome's
// shape calls for one entry per core, but a fake zero row is worse
// than a missing row.
static QJsonObject systemCpuJson()
{
    const QString arch = QSysInfo::currentCpuArchitecture();
    QString archName;
    if (arch == QLatin1String("x86_64")
            || arch == QLatin1String("amd64"))
        archName = QStringLiteral("x86_64");
    else if (arch == QLatin1String("aarch64")
             || arch == QLatin1String("arm64"))
        archName = QStringLiteral("arm64");
    else if (arch.startsWith(QLatin1String("arm")))
        archName = QStringLiteral("arm");
    else if (arch.startsWith(QLatin1String("mips64")))
        archName = QStringLiteral("mips64");
    else if (arch.startsWith(QLatin1String("mips")))
        archName = QStringLiteral("mips");
    else if (arch.startsWith(QLatin1String("i"))
             && arch.endsWith(QLatin1String("86")))
        archName = QStringLiteral("x86");
    else
        archName = arch;   // honest passthrough, never a claim

    QJsonObject info{{QStringLiteral("archName"), archName}};

#if defined(Q_OS_LINUX)
    QString modelName;
    QJsonArray features;
    QFile cpuinfo(QStringLiteral("/proc/cpuinfo"));
    if (cpuinfo.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = cpuinfo.readAll().split('\n');
        for (const QByteArray &line : lines) {
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray key = line.left(colon).trimmed();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (modelName.isEmpty() && key == "model name")
                modelName = QString::fromLocal8Bit(value);
            if (features.isEmpty()
                    && (key == "flags" || key == "Features")) {
                const QList<QByteArray> flags =
                    value.simplified().split(' ');
                for (const QByteArray &flag : flags) {
                    if (!flag.isEmpty())
                        features.append(QString::fromLocal8Bit(flag));
                }
            }
        }
    }
    info.insert(QStringLiteral("modelName"), modelName);
    info.insert(QStringLiteral("features"), features);
#else
    info.insert(QStringLiteral("modelName"), QString());
    info.insert(QStringLiteral("features"), QJsonArray());
#endif

    int numOfProcessors = QThread::idealThreadCount();
    if (numOfProcessors <= 0)
        numOfProcessors = 1;
    info.insert(QStringLiteral("numOfProcessors"), numOfProcessors);

    QJsonArray processors;
#if defined(Q_OS_LINUX)
    // /proc/stat cpuN rows carry cumulative jiffies per state;
    // Chrome wants seconds, so convert on the clock tick.  The
    // field mapping mirrors Chrome's own accounting: user =
    // user+nice, kernel = system+irq+softirq, idle = idle+iowait,
    // total = every measured column (guest time is already folded
    // into user/nice by the kernel, so it is not summed again).
    long ticksPerSec = 100;
#if defined(Q_OS_UNIX)
    const long confTicks = sysconf(_SC_CLK_TCK);
    if (confTicks > 0)
        ticksPerSec = confTicks;
#endif
    QFile stat(QStringLiteral("/proc/stat"));
    if (stat.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> lines = stat.readAll().split('\n');
        for (const QByteArray &line : lines) {
            // "cpu0 123 0 456 ..." — the aggregate "cpu " row is
            // excluded; per-core rows only.
            if (!line.startsWith("cpu") || line.startsWith("cpu "))
                continue;
            const QList<QByteArray> fields =
                line.mid(3).simplified().split(' ');
            if (fields.size() < 4)
                continue;
            double columns[8] = {};
            for (int i = 0; i < fields.size() && i < 8; ++i)
                columns[i] = double(fields.at(i).toLongLong())
                    / double(ticksPerSec);
            double total = 0;
            for (int i = 0; i < fields.size() && i < 8; ++i)
                total += columns[i];
            processors.append(QJsonObject{
                {QStringLiteral("usage"), QJsonObject{
                    {QStringLiteral("user"),
                     columns[0] + columns[1]},
                    {QStringLiteral("kernel"),
                     columns[2] + columns[5] + columns[6]},
                    {QStringLiteral("idle"),
                     columns[3] + columns[4]},
                    {QStringLiteral("total"), total}}}});
        }
    }
#endif
    info.insert(QStringLiteral("processors"), processors);
    return info;
}

// system.display.getInfo — the live QScreen list projected onto
// Chrome's DisplayInfo.  ids prefer the panel's serial number and
// fall back to its name so they stay stable across calls;
// bounds/workArea ride geometry()/availableGeometry(), rotation is
// the real orientation delta.  isInternal is the honest connector-
// name heuristic (eDP/LVDS/DSI are the spellings display servers
// hand internal panels) — nothing deeper exists below the platform
// plugins.  A screen reporting bounds identical to an
// already-emitted one IS the mirror: mirroringSourceId names it.
static QJsonArray systemDisplayList()
{
    QJsonArray displays;
    QScreen *primary = QGuiApplication::primaryScreen();
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (!screen)
            continue;
        const QString name = screen->name();
        QString id = screen->serialNumber();
        if (id.isEmpty())
            id = name;
        if (id.isEmpty())
            id = QString::number(displays.size() + 1);
        const QRect bounds = screen->geometry();
        const QRect work = screen->availableGeometry();
        QJsonObject info{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("isPrimary"), screen == primary},
            {QStringLiteral("isInternal"),
             name.startsWith(QLatin1String("eDP"))
                 || name.startsWith(QLatin1String("LVDS"))
                 || name.startsWith(QLatin1String("DSI"))},
            {QStringLiteral("isEnabled"), true},
            {QStringLiteral("rotation"),
             screen->angleBetween(screen->primaryOrientation(),
                                  screen->orientation())},
            {QStringLiteral("dpiX"), screen->logicalDotsPerInchX()},
            {QStringLiteral("dpiY"), screen->logicalDotsPerInchY()},
            {QStringLiteral("bounds"), QJsonObject{
                {QStringLiteral("left"), bounds.x()},
                {QStringLiteral("top"), bounds.y()},
                {QStringLiteral("width"), bounds.width()},
                {QStringLiteral("height"), bounds.height()}}},
            {QStringLiteral("workArea"), QJsonObject{
                {QStringLiteral("left"), work.x()},
                {QStringLiteral("top"), work.y()},
                {QStringLiteral("width"), work.width()},
                {QStringLiteral("height"), work.height()}}},
        };
        for (const QJsonValue &v : displays) {
            const QJsonObject prior = v.toObject()
                .value(QStringLiteral("bounds")).toObject();
            if (prior.value(QStringLiteral("left")).toInt()
                        == bounds.x()
                    && prior.value(QStringLiteral("top")).toInt()
                           == bounds.y()
                    && prior.value(QStringLiteral("width")).toInt()
                           == bounds.width()
                    && prior.value(QStringLiteral("height")).toInt()
                           == bounds.height()) {
                info.insert(
                    QStringLiteral("mirroringSourceId"),
                    v.toObject().value(QStringLiteral("id")));
                break;
            }
        }
        displays.append(info);
    }
    return displays;
}

// The storage units Chrome's StorageUnitInfo enumerates are mounted
// volumes; the honest local set is the volumes that actually hold
// profile state — the filesystem root plus the profile data dir's
// volume (deduped on device, ready-only).  Widening to every mount
// in /proc/mounts would report volumes the browser cannot touch.
static QList<QStorageInfo> systemStorageVolumes()
{
    QList<QStorageInfo> volumes;
    QSet<QByteArray> seen;
    const auto add = [&volumes, &seen](const QStorageInfo &info) {
        if (!info.isValid() || !info.isReady())
            return;
        const QByteArray device = info.device();
        if (device.isEmpty() || seen.contains(device))
            return;
        seen.insert(device);
        volumes.append(info);
    };
    add(QStorageInfo::root());
    add(QStorageInfo(
        BrowserPaths::profileDataDir(BrowserPaths::activeProfileId())));
    return volumes;
}

// Chrome's StorageUnitInfo shape — id is the volume's real device
// path so getAvailableCapacity can address it back stably.
static QJsonObject systemStorageUnitJson(const QStorageInfo &info)
{
    return QJsonObject{
        {QStringLiteral("id"), QString::fromLocal8Bit(info.device())},
        {QStringLiteral("name"), info.displayName()},
        {QStringLiteral("type"), QStringLiteral("fixed")},
        {QStringLiteral("capacity"), double(info.bytesTotal())}};
}

QJsonObject ExtensionShimBackend::invokeSystem(
        const QString &method, const QJsonObject &args)
{
    // Dotted "sub.member" like storage's "local.get" — a flat name
    // is the typed unreachable-member verdict.
    const int dot = method.indexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == method.size() - 1)
        return errReply(QStringLiteral("unimplemented"));
    const QString sub = method.left(dot);
    const QString member = method.mid(dot + 1);
    if (member.contains(QLatin1Char('.')))
        return errReply(QStringLiteral("unimplemented"));

    // Chrome gates each sub-namespace on its own 'system.<sub>'
    // declared grant; the gate runs BEFORE the surface check so a
    // declared-but-unserved sub hears 'unsupported' — the grant was
    // honored, the surface is missing — while an undeclared call
    // stays no-permission.
    const QString grant = QStringLiteral("system.") + sub;
    if (!manifestPermissions().contains(grant))
        return errReply(QStringLiteral("no-permission"));

    if (sub == QLatin1String("memory")) {
        if (member == QLatin1String("getInfo")) {
            qint64 capacity = -1, available = -1;
            systemMemoryBytes(&capacity, &available);
            if (capacity < 0)
                // Nothing measured — a typed refusal beats a fake
                // capacity.
                return errReply(QStringLiteral("unsupported"));
            QJsonObject info{
                {QStringLiteral("capacity"), double(capacity)}};
            if (available >= 0)
                info.insert(QStringLiteral("availableCapacity"),
                            double(available));
            return okReply(info);
        }
        return errReply(QStringLiteral("unsupported"));
    }
    if (sub == QLatin1String("cpu")) {
        if (member == QLatin1String("getInfo"))
            return okReply(systemCpuJson());
        return errReply(QStringLiteral("unsupported"));
    }
    if (sub == QLatin1String("display")) {
        if (member == QLatin1String("getInfo")) {
            const QJsonValue flags =
                args.value(QLatin1String("flags"));
            if (!flags.isUndefined() && !flags.isObject())
                return errReply(QStringLiteral("invalid-argument"));
            return okReply(systemDisplayList());
        }
        // setMirrorMode/setDisplayProperties/the overscan +
        // touch-calibration family are ChromeOS-only surfaces
        // upstream.
        return errReply(QStringLiteral("unsupported"));
    }
    if (sub == QLatin1String("storage")) {
        if (member == QLatin1String("getInfo")) {
            const QList<QStorageInfo> volumes =
                systemStorageVolumes();
            QJsonArray units;
            for (const QStorageInfo &info : volumes)
                units.append(systemStorageUnitJson(info));
            return okReply(units);
        }
        if (member == QLatin1String("getAvailableCapacity")) {
            const QString id =
                args.value(QLatin1String("id")).toString();
            if (id.isEmpty())
                return errReply(QStringLiteral("invalid-argument"));
            const QList<QStorageInfo> volumes =
                systemStorageVolumes();
            for (const QStorageInfo &info : volumes) {
                if (QString::fromLocal8Bit(info.device()) != id)
                    continue;
                return okReply(QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("availableCapacity"),
                     double(info.bytesAvailable())}});
            }
            return errReply(QStringLiteral("not-found"));
        }
        // ejectDevice — no device-ejection surface exists.
        return errReply(QStringLiteral("unsupported"));
    }
    // system.network/powerSource/private/... — declared grant, no
    // Arktos surface.
    return errReply(QStringLiteral("unsupported"));
}

// -------------------------------------------------------------------
// chrome.privacy — MD31.  Chrome's ChromeSetting surface over the
// browser's real settings.  The whole namespace gates on the
// declared 'privacy' grant like Chrome; every member answers
// get/set/clear in the "<sub>.<member>.<op>" vocabulary.
//
// Served members — each maps onto a persisted persona toggle and
// re-applies live on write:
//
//   websites.thirdPartyCookiesAllowed   <-> cookies/blockThirdPartyCookies
//       (inverted; the jar's loadSettings re-read lands it in the
//       live policy snapshot)
//   websites.hyperlinkAuditingEnabled   <-> privacy/blockPings
//       (inverted — the PING01 toggle also gates sendBeacon and CSP
//       report uploads, the honest superset of <a ping> auditing)
//   websites.referrersEnabled           <-> privacy/refererPolicy
//       (bool can't express the four levels: true -> EngineDefault,
//       false -> Never; get is enabled for every level but Never)
//   websites.doNotTrackEnabled          <-> privacy/gpcSignal
//       (deprecated upstream; the Sec-GPC header +
//       navigator.globalPrivacyControl pair is the DNT signal's
//       living successor — the same "send a privacy preference"
//       semantics)
//   network.networkPredictionEnabled    <-> privacy/blockPrefetch
//       (inverted; setting false also drops the opt-in dnsPrefetch
//       key — Chrome's prediction-off kills every speculative
//       resolution — while setting true leaves DNS prefetch off:
//       TELEM02 keeps it opt-in)
//   network.webRTCIPHandlingPolicy      <-> privacy/webrtcIpProtection
//       ("default" -> off, "disable_non_proxied_udp" -> on — the
//       two values the bool+process-flag pair can honestly express;
//       Chrome's two intermediate policies stay typed unsupported)
//   services.passwordSavingEnabled      <-> autofill/passwordForms
//   services.searchSuggestEnabled       <-> openSearch/suggestInAddressField
//       (through OpenSearchManager so the toolbar's suggestion
//       fetching flips live, not just on next read)
//
// ChromeSetting semantics: get() answers {value, levelOfControl};
// levelOfControl is per-extension — the member's last set()ter
// controls it (Chrome's most-preferred-extension layering,
// flattened: last writer wins).  The value an extension changed is
// snapshotted on the first claim so clear() restores the
// pre-extension state like Chrome's restore-default; clear() by a
// non-controller is Chrome's silent no-op.  'scope' values other
// than 'regular' mean incognito layering the persona stores cannot
// express — typed unsupported.  The unserved half of Chrome's
// published vocabulary stays typed 'unsupported' too; an unknown
// member or op is the unreachable-member verdict.  onChange details
// carry the acting extension's id under __controller — fanOut's
// shapedPayload resolves each observer's own levelOfControl from
// it, the same call Chrome's per-extension control makes.
// -------------------------------------------------------------------

enum class PrivacyMember {
    Invalid,
    NetworkPredictionEnabled,
    WebRTCIPHandlingPolicy,
    PasswordSavingEnabled,
    SearchSuggestEnabled,
    ThirdPartyCookiesAllowed,
    HyperlinkAuditingEnabled,
    ReferrersEnabled,
    DoNotTrackEnabled,
};

struct PrivacyMemberDef {
    const char *sub;
    const char *name;
    PrivacyMember id;
};

static const PrivacyMemberDef kPrivacyMembers[] = {
    { "network", "networkPredictionEnabled",
      PrivacyMember::NetworkPredictionEnabled },
    { "network", "webRTCIPHandlingPolicy",
      PrivacyMember::WebRTCIPHandlingPolicy },
    { "services", "passwordSavingEnabled",
      PrivacyMember::PasswordSavingEnabled },
    { "services", "searchSuggestEnabled",
      PrivacyMember::SearchSuggestEnabled },
    { "websites", "thirdPartyCookiesAllowed",
      PrivacyMember::ThirdPartyCookiesAllowed },
    { "websites", "hyperlinkAuditingEnabled",
      PrivacyMember::HyperlinkAuditingEnabled },
    { "websites", "referrersEnabled",
      PrivacyMember::ReferrersEnabled },
    { "websites", "doNotTrackEnabled",
      PrivacyMember::DoNotTrackEnabled },
};

// The rest of Chrome's published vocabulary — known members with no
// honest mapping.  autofillAddress/CreditCard ride Chromium's
// autofill feature (Arktos's fill engine has no enable toggle),
// safeBrowsing* need the Safe Browsing service, spellingService the
// Google-backed remote spellcheck, translationEnabled the translate
// pipeline, protectedContent + webRTCUDPPortRange are ChromeOS-only,
// webRTCNonProxiedUdpEnabled is the deprecated webRTCIPHandlingPolicy
// spelling, and the Privacy Sandbox family (fledge/topics/
// adMeasurement/privacySandbox) has no local counterpart.
static const char *const kPrivacyUnserved[][2] = {
    { "network", "webRTCNonProxiedUdpEnabled" },
    { "network", "webRTCUDPPortRange" },
    { "services", "autofillAddressEnabled" },
    { "services", "autofillCreditCardEnabled" },
    { "services", "autofillEnabled" },
    { "services", "safeBrowsingEnabled" },
    { "services", "safeBrowsingExtendedReportingEnabled" },
    { "services", "spellingServiceEnabled" },
    { "services", "translationEnabled" },
    { "websites", "adMeasurementEnabled" },
    { "websites", "fledgeEnabled" },
    { "websites", "privacySandboxEnabled" },
    { "websites", "protectedContentEnabled" },
    { "websites", "topicsEnabled" },
};

static PrivacyMember privacyMemberFor(const QString &sub,
                                      const QString &member)
{
    for (const PrivacyMemberDef &def : kPrivacyMembers) {
        if (sub == QLatin1String(def.sub)
                && member == QLatin1String(def.name))
            return def.id;
    }
    return PrivacyMember::Invalid;
}

static bool privacyMemberUnserved(const QString &sub,
                                  const QString &member)
{
    for (const auto &entry : kPrivacyUnserved) {
        if (sub == QLatin1String(entry[0])
                && member == QLatin1String(entry[1]))
            return true;
    }
    return false;
}

static QString privacyMemberPath(PrivacyMember id)
{
    for (const PrivacyMemberDef &def : kPrivacyMembers) {
        if (def.id == id)
            return QString::fromLatin1(def.sub) + QLatin1Char('.')
                + QString::fromLatin1(def.name);
    }
    return QString();
}

// Per-member extension control — the flattened Chrome layering.
// extensionId is the last successful set()ter (empty = unclaimed);
// prior snapshots the pre-claim value clear() restores.
struct PrivacyControl {
    QString extensionId;
    QVariant prior;
    bool hasPrior = false;
};

static QHash<QString, PrivacyControl> &privacyControl()
{
    static QHash<QString, PrivacyControl> map;
    return map;
}

// The Chrome-level value a member reports — reads the live persona
// store so a user-side flip is the answer, never a cached snapshot.
static QVariant privacyMemberValue(PrivacyMember id)
{
    const QString profileId = BrowserPaths::activeProfileId();
    switch (id) {
    case PrivacyMember::ThirdPartyCookiesAllowed:
        return !ProfileSettings(profileId).value(
            QLatin1String("cookies/blockThirdPartyCookies"), true)
                .toBool();
    case PrivacyMember::HyperlinkAuditingEnabled:
        return !ProfileSettings(profileId).value(
            QLatin1String("privacy/blockPings"), true).toBool();
    case PrivacyMember::ReferrersEnabled:
        return PrivacyRequestInterceptor::storedRefererPolicy()
            != int(PrivacyRequestInterceptor::RefererNever);
    case PrivacyMember::DoNotTrackEnabled:
        return ProfileSettings(profileId).value(
            QLatin1String("privacy/gpcSignal"), true).toBool();
    case PrivacyMember::NetworkPredictionEnabled:
        return !ProfileSettings(profileId).value(
            QLatin1String("privacy/blockPrefetch"), true).toBool();
    case PrivacyMember::WebRTCIPHandlingPolicy:
        return ProfileSettings(profileId).value(
            QLatin1String("privacy/webrtcIpProtection"), true).toBool()
            ? QStringLiteral("disable_non_proxied_udp")
            : QStringLiteral("default");
    case PrivacyMember::PasswordSavingEnabled:
        return ProfileSettings(profileId).value(
            QLatin1String("autofill/passwordForms"), true).toBool();
    case PrivacyMember::SearchSuggestEnabled:
        // The manager's live member is the truth — its autosave may
        // not have flushed a just-set toggle to disk yet.
        return ToolbarSearch::openSearchManager()
            ->suggestionsInAddressField();
    case PrivacyMember::Invalid:
        break;
    }
    return QVariant();
}

// The IO-thread snapshot + engine-attribute half of a privacy/*
// write.  The interceptor re-read is unconditional — it is the
// request-path consumer; applySettings only runs on profiles
// already materialized (a profile created later reads the fresh
// key at construction).
static void privacyApplyLive(const QString &profileId)
{
    PrivacyRequestInterceptor::loadSettings();
    if (profileId == QLatin1String("default")) {
        // normalProfile() lazily creates the shared persona profile
        // — materializing it to push a live toggle is the same
        // bring-up the settings dialog performs.
        if (QWebEngineProfile *profile = BrowserProfile::normalProfile())
            BrowserProfile::applySettings(profile);
    } else if (QWebEngineProfile *profile =
                   BrowserProfile::personaProfileIfCreated(profileId)) {
        BrowserProfile::applySettings(profile);
    }
    if (QWebEngineProfile *otr =
            BrowserProfile::privateProfileIfCreated())
        BrowserProfile::applySettings(otr);
}

// The referer half of a write — the level int is the native value
// (the Chrome bool collapses four levels into allow/never, so
// clear()'s restore needs the level itself to hand the user's exact
// policy back).  The PRIV01-era bool stays in sync like the
// settings dialog's write — an older build maps "Chromium default"
// to off and everything else to trimmed.
static void privacyRefererWrite(int level)
{
    const QString profileId = BrowserPaths::activeProfileId();
    ProfileSettings settings(profileId);
    settings.setValue(QLatin1String("privacy/refererPolicy"), level);
    settings.setValue(QLatin1String("privacy/trimReferer"),
                      level != int(PrivacyRequestInterceptor::
                                       RefererEngineDefault));
    privacyApplyLive(profileId);
}

// Write a member's Chrome-level value onto the real setting and
// apply it through the live machinery.  The caller already
// validated the value's shape.
static void privacyMemberWrite(PrivacyMember id, const QVariant &value)
{
    const QString profileId = BrowserPaths::activeProfileId();
    switch (id) {
    case PrivacyMember::ThirdPartyCookiesAllowed: {
        ProfileSettings settings(profileId);
        settings.setValue(QLatin1String("cookies/blockThirdPartyCookies"),
                          !value.toBool());
        CookieJar::instanceFor(profileId)->loadSettings();
        break;
    }
    case PrivacyMember::HyperlinkAuditingEnabled: {
        ProfileSettings settings(profileId);
        settings.setValue(QLatin1String("privacy/blockPings"),
                          !value.toBool());
        privacyApplyLive(profileId);
        break;
    }
    case PrivacyMember::ReferrersEnabled:
        privacyRefererWrite(value.toBool()
            ? int(PrivacyRequestInterceptor::RefererEngineDefault)
            : int(PrivacyRequestInterceptor::RefererNever));
        break;
    case PrivacyMember::DoNotTrackEnabled: {
        ProfileSettings settings(profileId);
        settings.setValue(QLatin1String("privacy/gpcSignal"),
                          value.toBool());
        privacyApplyLive(profileId);
        break;
    }
    case PrivacyMember::NetworkPredictionEnabled: {
        ProfileSettings settings(profileId);
        settings.setValue(QLatin1String("privacy/blockPrefetch"),
                          !value.toBool());
        if (!value.toBool())
            // Prediction off kills every speculative channel —
            // DNS prefetch is the second half.
            settings.setValue(QLatin1String("privacy/dnsPrefetch"),
                              false);
        privacyApplyLive(profileId);
        break;
    }
    case PrivacyMember::WebRTCIPHandlingPolicy: {
        ProfileSettings settings(profileId);
        settings.setValue(
            QLatin1String("privacy/webrtcIpProtection"),
            value.toString()
                == QLatin1String("disable_non_proxied_udp"));
        privacyApplyLive(profileId);
        break;
    }
    case PrivacyMember::PasswordSavingEnabled: {
        ProfileSettings settings(profileId);
        settings.setValue(QLatin1String("autofill/passwordForms"),
                          value.toBool());
        AutoFillManager::instanceFor(profileId)->loadSettings();
        break;
    }
    case PrivacyMember::SearchSuggestEnabled:
        // The manager writes + autosaves itself and flips the
        // toolbar's suggestion fetching live.
        ToolbarSearch::openSearchManager()
            ->setSuggestionsInAddressField(value.toBool());
        break;
    case PrivacyMember::Invalid:
        break;
    }
}

// The native value clear() restores — identical to the Chrome-level
// value for every member whose mapping is a plain bool/enum; for
// referrersEnabled it is the four-level int (the bool can't carry
// "Strict" or "Trimmed" back).
static QVariant privacyNativeValue(PrivacyMember id)
{
    if (id == PrivacyMember::ReferrersEnabled)
        return PrivacyRequestInterceptor::storedRefererPolicy();
    return privacyMemberValue(id);
}

static void privacyNativeWrite(PrivacyMember id, const QVariant &value)
{
    if (id == PrivacyMember::ReferrersEnabled) {
        privacyRefererWrite(value.toInt());
        return;
    }
    privacyMemberWrite(id, value);
}

static QString privacyLevelOfControl(const QString &path,
                                     const QString &extensionId)
{
    const QString controller =
        privacyControl().value(path).extensionId;
    if (controller.isEmpty())
        return QStringLiteral("controllable_by_this_extension");
    return controller == extensionId
        ? QStringLiteral("controlled_by_this_extension")
        : QStringLiteral("controlled_by_other_extensions");
}

// onChange for one member — ChromeSetting details {value,
// levelOfControl}; the level is per-observer, so the payload ships
// the acting extension's id under __controller and shapedPayload
// resolves each context's own control level.
static void privacyFanOut(PrivacyMember id, const QString &actorId,
                          ExtensionShimBackend *backend)
{
    const QString path = privacyMemberPath(id);
    const QString name = QStringLiteral("privacy.") + path
        + QLatin1String(".onChange");
    const QJsonObject payload{
        {QStringLiteral("__args"), QJsonArray{QJsonObject{
            {QStringLiteral("value"),
             QJsonValue::fromVariant(privacyMemberValue(id))},
            {QStringLiteral("levelOfControl"),
             QStringLiteral("controlled_by_other_extensions")}}}},
        {QStringLiteral("__controller"), actorId}};
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->broadcastEvent(name, payload);
    // Same bare-backend fallback managementFanOut applies — the
    // actor's own level is the controlled_by_this_extension shape.
    if (backend && !runtime->hasRelayContext(actorId)) {
        const QJsonObject shaped{
            {QStringLiteral("__args"), QJsonArray{QJsonObject{
                {QStringLiteral("value"),
                 QJsonValue::fromVariant(privacyMemberValue(id))},
                {QStringLiteral("levelOfControl"),
                 QStringLiteral("controlled_by_this_extension")}}}}};
        emit backend->pushEvent(name, shaped);
    }
}

QJsonObject ExtensionShimBackend::invokePrivacy(
        const QString &method, const QJsonObject &args)
{
    // Chrome exposes the namespace only under the declared
    // 'privacy' grant — the bridge object answers a direct call
    // from any code in the context, so the gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("privacy")))
        return errReply(QStringLiteral("no-permission"));

    if (!m_privacyNoted) {
        m_privacyNoted = true;
        recordNote(QStringLiteral(
            "chrome.privacy: served ChromeSetting members are "
            "websites.thirdPartyCookiesAllowed/hyperlinkAuditing"
            "Enabled/referrersEnabled/doNotTrackEnabled, network."
            "networkPredictionEnabled/webRTCIPHandlingPolicy and "
            "services.passwordSavingEnabled/searchSuggestEnabled — "
            "writes land the real persona toggles and re-apply live; "
            "extension control is flattened (last set()ter controls, "
            "clear() restores the pre-extension value); user-driven "
            "settings flips emit no onChange"));
    }

    // "<sub>.<member>.<op>" — exactly two dots.
    const int firstDot = method.indexOf(QLatin1Char('.'));
    if (firstDot <= 0)
        return errReply(QStringLiteral("unimplemented"));
    const int lastDot = method.lastIndexOf(QLatin1Char('.'));
    if (lastDot == firstDot || lastDot == method.size() - 1)
        return errReply(QStringLiteral("unimplemented"));
    const QString sub = method.left(firstDot);
    const QString member = method.mid(firstDot + 1,
                                      lastDot - firstDot - 1);
    const QString op = method.mid(lastDot + 1);

    const PrivacyMember id = privacyMemberFor(sub, member);
    const QString path = privacyMemberPath(id);
    if (id == PrivacyMember::Invalid) {
        // Declared grant honored — a member Chrome publishes but
        // this build cannot map hears 'unsupported'; anything else
        // is the unreachable-member verdict.
        if (privacyMemberUnserved(sub, member))
            return errReply(QStringLiteral("unsupported"));
        return errReply(QStringLiteral("unimplemented"));
    }

    const QJsonValue detailsValue = args.value(QLatin1String("details"));
    QJsonObject details;
    if (detailsValue.isObject())
        details = detailsValue.toObject();
    else if (!detailsValue.isUndefined())
        return errReply(QStringLiteral("invalid-argument"));

    // ChromeSetting scopes beyond 'regular' mean incognito layering
    // the persona stores cannot express — a recognized-but-
    // unservable scope is 'unsupported', an unrecognized one the
    // argument verdict.
    const auto scopeCheck = [](const QJsonObject &d) -> QString {
        const QJsonValue scope = d.value(QLatin1String("scope"));
        if (scope.isUndefined())
            return QString();
        if (!scope.isString())
            return QStringLiteral("invalid-argument");
        const QString value = scope.toString();
        if (value == QLatin1String("regular"))
            return QString();
        if (value == QLatin1String("incognito_persistent")
                || value == QLatin1String("incognito_session_only"))
            return QStringLiteral("unsupported");
        return QStringLiteral("invalid-argument");
    };

    if (op == QLatin1String("get")) {
        const QJsonValue incognito =
            details.value(QLatin1String("incognito"));
        if (!incognito.isUndefined() && !incognito.isBool())
            return errReply(QStringLiteral("invalid-argument"));
        // get() answers the regular-profile value — there is no
        // incognito overlay to resolve a different one from, so an
        // incognito:true query hears the same honest value.
        return okReply(QJsonObject{
            {QStringLiteral("value"),
             QJsonValue::fromVariant(privacyMemberValue(id))},
            {QStringLiteral("levelOfControl"),
             privacyLevelOfControl(path, m_extensionId)}});
    }

    if (op == QLatin1String("set")) {
        const QString scopeVerdict = scopeCheck(details);
        if (!scopeVerdict.isEmpty())
            return errReply(scopeVerdict);
        const QJsonValue value = details.value(QLatin1String("value"));
        QVariant applied;
        if (id == PrivacyMember::WebRTCIPHandlingPolicy) {
            if (!value.isString())
                return errReply(QStringLiteral("invalid-argument"));
            const QString policy = value.toString();
            if (policy == QLatin1String("default")
                    || policy == QLatin1String(
                           "disable_non_proxied_udp")) {
                applied = policy;
            } else if (policy == QLatin1String(
                           "default_public_and_private_interfaces")
                       || policy == QLatin1String(
                           "default_public_interface_only")) {
                // Chrome-published values the bool+process-flag
                // pair cannot express — typed unservable, never a
                // silent clamp to a different policy.
                return errReply(QStringLiteral("unsupported"));
            } else {
                return errReply(QStringLiteral("invalid-argument"));
            }
        } else {
            if (!value.isBool())
                return errReply(QStringLiteral("invalid-argument"));
            applied = value.toBool();
        }

        // First claim snapshots the pre-extension value — clear()
        // restores it like Chrome's restore-default.  The native
        // value is captured, not the Chrome projection — the referer
        // member's four levels would otherwise collapse to a bool.
        PrivacyControl &control = privacyControl()[path];
        if (control.extensionId.isEmpty()) {
            control.prior = privacyNativeValue(id);
            control.hasPrior = true;
        }
        privacyMemberWrite(id, applied);
        control.extensionId = m_extensionId;
        privacyFanOut(id, m_extensionId, this);
        return okReply();
    }

    if (op == QLatin1String("clear")) {
        const QString scopeVerdict = scopeCheck(details);
        if (!scopeVerdict.isEmpty())
            return errReply(scopeVerdict);
        PrivacyControl &control = privacyControl()[path];
        if (control.extensionId == m_extensionId) {
            // The controller's clear restores the pre-extension
            // value and releases the claim — Chrome's
            // restore-default; a clear by a non-controller (or on
            // an unclaimed member) is Chrome's silent no-op.
            if (control.hasPrior)
                privacyNativeWrite(id, control.prior);
            control = PrivacyControl();
            privacyFanOut(id, m_extensionId, this);
        }
        return okReply();
    }

    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.fontSettings — MD33.  Chrome's flat member vocabulary over
// the engine's real font defaults.  The whole namespace gates on
// the declared 'fontSettings' grant like Chrome; every op's first
// argument is a details object (getFontList takes none).
//
// Served surfaces and their real mappings:
//
//   getFontList                — the installed system families
//                                (QFontDatabase)
//   getFont/setFont/clearFont  — Chrome's genericFamily enum onto
//     QWebEngineSettings::FontFamily + a persisted websettings row:
//       standard   -> StandardFont   (standardFont — the settings
//                                     dialog's composite QFont row:
//                                     family + default size ride one
//                                     value)
//       fixed      -> FixedFont      (fixedFont, same composite)
//       serif      -> SerifFont      (serifFont)
//       sansserif  -> SansSerifFont  (sansSerifFont)
//       cursive    -> CursiveFont    (cursiveFont)
//       fantasy    -> FantasyFont    (fantasyFont)
//       math       -> no Qt family   (typed 'unsupported')
//   get/set/clearDefaultFontSize      — the standardFont row's size
//   get/set/clearDefaultFixedFontSize — the fixedFont row's size
//   get/set/clearMinimumFontSize      — minimumFontSize (0 = off)
//
// Chrome also scopes fonts per ISO-15924 script — QWebEngineSettings
// carries one family per generic family, so only the 'Zyyy' (common)
// row is servable; a named script is typed 'unsupported'.  Pixel
// semantics follow the settings dialog's own: the QFont's pointSize
// is what applySettings hands setFontSize — the same conflation the
// dialog already makes, not a new one.
//
// Writes persist through websettings and re-apply live via
// BrowserProfile::applySettings on every materialized profile bound
// to the persona — the same fan-out the settings dialog's save runs
// (profile + OTR + containers).
//
// ChromeSetting-style control like privacy: a member's first set()
// snapshots the pre-extension value and claims it; get() resolves
// levelOfControl per extension; the controller's clear*() restores
// the snapshot while a non-controller's clear is Chrome's silent
// no-op.  The four on*Changed events carry __controller so
// shapedPayload resolves each observer's own level; user-driven
// settings-dialog changes emit no event (there is no source).
// -------------------------------------------------------------------

enum class FontSizeMember {
    DefaultFontSize,
    DefaultFixedFontSize,
    MinimumFontSize,
};

struct FontFamilyDef {
    const char *name;                      // Chrome genericFamily
    QWebEngineSettings::FontFamily engine;
    const char *key;                       // websettings row
    bool composite;                        // QFont row (family+size)
    bool served;
};

// Chrome's published genericFamily enum.  'math' is published but
// has no QWebEngineSettings family in this Qt (the enum ends at
// PictographFont, which Chrome does not expose) — it keeps the
// typed refusal like every other unmappable member.
static const FontFamilyDef kFontFamilies[] = {
    { "standard",  QWebEngineSettings::StandardFont,
      "standardFont",   true,  true  },
    { "serif",     QWebEngineSettings::SerifFont,
      "serifFont",      false, true  },
    { "sansserif", QWebEngineSettings::SansSerifFont,
      "sansSerifFont",  false, true  },
    { "fixed",     QWebEngineSettings::FixedFont,
      "fixedFont",      true,  true  },
    { "cursive",   QWebEngineSettings::CursiveFont,
      "cursiveFont",    false, true  },
    { "fantasy",   QWebEngineSettings::FantasyFont,
      "fantasyFont",    false, true  },
    { "math",      QWebEngineSettings::StandardFont,
      nullptr,          false, false },
};

static const FontFamilyDef *fontFamilyDefFor(const QString &name)
{
    for (const FontFamilyDef &def : kFontFamilies) {
        if (name == QLatin1String(def.name))
            return &def;
    }
    return nullptr;
}

// Chromium's out-of-box font defaults — a pristine unnamed (OTR)
// profile no persona's applySettings ever touched.  The baseline for
// an unmaterialized persona and the seed for a first write into a
// composite row.
static QWebEngineSettings *fontBaselineSettings()
{
    static QWebEngineProfile *baseline = nullptr;
    if (!baseline)
        baseline = new QWebEngineProfile(qApp);
    return baseline->settings();
}

// The persona's live engine settings — the materialized profile's
// view, where applySettings already folded the stored rows in.
// nullptr for an unmaterialized non-default persona (a read must
// never materialize one).
static QWebEngineSettings *fontLiveSettings(const QString &profileId)
{
    if (profileId == QLatin1String("default"))
        return BrowserProfile::normalProfile()->settings();
    if (QWebEngineProfile *profile =
            BrowserProfile::personaProfileIfCreated(profileId))
        return profile->settings();
    return nullptr;
}

static QWebEngineSettings *fontEngineOrBaseline(const QString &profileId)
{
    if (QWebEngineSettings *live = fontLiveSettings(profileId))
        return live;
    return fontBaselineSettings();
}

// The stored variant -> QFont decode, with the missing/undecodable
// cases folded to "absent": an empty variant yields QFont() — the
// system default, family non-empty and size the app default — and a
// legacy '@Type(...)' ini blob (imported by RC08 as a raw string it
// cannot decode) yields the same resolved default.  Both are "no
// persona override", never a real value.
static bool fontFromStored(const QVariant &stored, QFont *out)
{
    if (!stored.isValid())
        return false;
    if (stored.typeId() == QMetaType::QString
            && stored.toString().startsWith(QLatin1Char('@')))
        return false;
    const QFont font = stored.value<QFont>();
    if (font.family().isEmpty())
        return false;
    if (out)
        *out = font;
    return true;
}

// The Chrome-level fontId a family reports — the persona's stored
// row when it carries one, else the engine's resolved default.
static QString fontFamilyValue(const FontFamilyDef &def,
                               const QString &profileId)
{
    ProfileSettings store(profileId);
    store.beginGroup(QLatin1String("websettings"));
    const QVariant stored = store.value(QLatin1String(def.key));
    QFont font;
    const QString family =
        (def.composite && fontFromStored(stored, &font))
            ? font.family()
            : (!def.composite ? stored.toString() : QString());
    if (!family.isEmpty())
        return family;
    return fontEngineOrBaseline(profileId)->fontFamily(def.engine);
}

static QWebEngineSettings::FontSize fontSizeSlot(FontSizeMember member)
{
    switch (member) {
    case FontSizeMember::DefaultFixedFontSize:
        return QWebEngineSettings::DefaultFixedFontSize;
    case FontSizeMember::MinimumFontSize:
        return QWebEngineSettings::MinimumFontSize;
    case FontSizeMember::DefaultFontSize:
        break;
    }
    return QWebEngineSettings::DefaultFontSize;
}

static int fontSizeValue(FontSizeMember member,
                         const QString &profileId)
{
    ProfileSettings store(profileId);
    store.beginGroup(QLatin1String("websettings"));
    if (member == FontSizeMember::MinimumFontSize) {
        const QVariant stored =
            store.value(QLatin1String("minimumFontSize"));
        if (stored.isValid())
            return stored.toInt();
    } else {
        const QString key =
            member == FontSizeMember::DefaultFontSize
                ? QStringLiteral("standardFont")
                : QStringLiteral("fixedFont");
        const QVariant stored = store.value(key);
        QFont font;
        if (fontFromStored(stored, &font)) {
            const int size = font.pointSize();
            if (size > 0)
                return size;
        }
    }
    return fontEngineOrBaseline(profileId)->fontSize(
        fontSizeSlot(member));
}

// The live half of a write — applySettings re-reads the persona's
// whole websettings group onto every materialized profile that binds
// it: the persona profile, the OTR overlay and the persona's
// container profiles.  The same fan-out the settings dialog's save
// runs (its normalProfile + OTR + reapplySettings trio).
static void fontApplyLive(const QString &profileId)
{
    if (profileId == QLatin1String("default"))
        BrowserProfile::applySettings(BrowserProfile::normalProfile());
    else if (QWebEngineProfile *profile =
                 BrowserProfile::personaProfileIfCreated(profileId))
        BrowserProfile::applySettings(profile);
    if (QWebEngineProfile *otr =
            BrowserProfile::privateProfileIfCreated())
        BrowserProfile::applySettings(otr);
    if (ContainerManager *containers =
            ContainerManager::instanceIfCreated(profileId))
        containers->reapplySettings();
}

static void fontWriteFamily(const FontFamilyDef &def,
                            const QString &family,
                            const QString &profileId)
{
    ProfileSettings store(profileId);
    store.beginGroup(QLatin1String("websettings"));
    if (def.composite) {
        // standard/fixed ride the dialog's {family,size} QFont row —
        // a family write must preserve the size half, so a
        // never-written row seeds from the engine first.
        const QVariant stored = store.value(QLatin1String(def.key));
        QFont font;
        if (!fontFromStored(stored, &font)) {
            QWebEngineSettings *engine =
                fontEngineOrBaseline(profileId);
            font = QFont(engine->fontFamily(def.engine),
                         engine->fontSize(
                             def.engine == QWebEngineSettings::FixedFont
                                 ? QWebEngineSettings::DefaultFixedFontSize
                                 : QWebEngineSettings::DefaultFontSize));
        }
        font.setFamily(family);
        store.setValue(QLatin1String(def.key), font);
    } else {
        store.setValue(QLatin1String(def.key), family);
    }
    fontApplyLive(profileId);
}

static void fontWriteSize(FontSizeMember member, int px,
                          const QString &profileId)
{
    ProfileSettings store(profileId);
    store.beginGroup(QLatin1String("websettings"));
    if (member == FontSizeMember::MinimumFontSize) {
        store.setValue(QLatin1String("minimumFontSize"), px);
    } else {
        // The size half of the composite row — a never-written row
        // seeds from the engine so the family half is preserved.
        const bool standard =
            member == FontSizeMember::DefaultFontSize;
        const QString key = standard
            ? QStringLiteral("standardFont")
            : QStringLiteral("fixedFont");
        const QVariant stored = store.value(key);
        QFont font;
        if (!fontFromStored(stored, &font)) {
            QWebEngineSettings *engine =
                fontEngineOrBaseline(profileId);
            font = QFont(engine->fontFamily(
                             standard ? QWebEngineSettings::StandardFont
                                      : QWebEngineSettings::FixedFont),
                         px);
        }
        font.setPointSize(px);
        store.setValue(key, font);
    }
    fontApplyLive(profileId);
}

// Per-member extension control — the flattened Chrome layering
// (same discipline privacy applies): the last successful set()ter
// controls; prior snapshots the pre-claim value the controller's
// clear*() restores.  Keys are the Chrome generic family names for
// the font ops and the member names for the size ops.
struct FontControl {
    QString extensionId;
    QVariant prior;
    bool hasPrior = false;
};

static QHash<QString, FontControl> &fontControl()
{
    static QHash<QString, FontControl> map;
    return map;
}

static QString fontLevelOfControl(const QString &key,
                                  const QString &extensionId)
{
    const QString controller = fontControl().value(key).extensionId;
    if (controller.isEmpty())
        return QStringLiteral("controllable_by_this_extension");
    return controller == extensionId
        ? QStringLiteral("controlled_by_this_extension")
        : QStringLiteral("controlled_by_other_extensions");
}

static QJsonArray fontListJson()
{
    QStringList families = QFontDatabase::families();
    families.removeDuplicates();
    families.sort(Qt::CaseInsensitive);
    QJsonArray list;
    for (const QString &family : families) {
        list.append(QJsonObject{
            {QStringLiteral("fontId"), family},
            {QStringLiteral("displayName"), family}});
    }
    return list;
}

// The on*Changed fan-out — Chrome's details shape plus a
// levelOfControl that resolves per observer through __controller,
// the same call privacy's onChange makes.  A bare backend hears its
// own controlled_by_this_extension copy on pushEvent.
static void fontFanOut(const QString &event, QJsonObject details,
                       const QString &actorId,
                       ExtensionShimBackend *backend)
{
    const QString name = QStringLiteral("fontSettings.") + event;
    details.insert(QStringLiteral("levelOfControl"),
                   QStringLiteral("controlled_by_other_extensions"));
    const QJsonObject payload{
        {QStringLiteral("__args"), QJsonArray{details}},
        {QStringLiteral("__controller"), actorId}};
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->broadcastEvent(name, payload);
    if (backend && !runtime->hasRelayContext(actorId)) {
        details.insert(
            QStringLiteral("levelOfControl"),
            QStringLiteral("controlled_by_this_extension"));
        emit backend->pushEvent(name, QJsonObject{
            {QStringLiteral("__args"), QJsonArray{details}}});
    }
}

QJsonObject ExtensionShimBackend::invokeFontSettings(
        const QString &method, const QJsonObject &args)
{
    // Chrome exposes the namespace only under the declared
    // 'fontSettings' grant — the same gate privacy applies.
    if (!manifestPermissions().contains(QLatin1String("fontSettings")))
        return errReply(QStringLiteral("no-permission"));

    if (!m_fontSettingsNoted) {
        m_fontSettingsNoted = true;
        recordNote(QStringLiteral(
            "chrome.fontSettings: standard/serif/sansserif/fixed/"
            "cursive/fantasy families and default/fixed/minimum "
            "sizes are served over the persona's real engine "
            "defaults — writes land the persisted websettings rows "
            "and re-apply live; 'math' has no Qt family and "
            "per-script (non-'Zyyy') fonts no granularity, both "
            "typed unsupported; extension control is flattened "
            "(last set()ter controls, clear() restores the "
            "pre-extension value); user-driven settings flips emit "
            "no on*Changed"));
    }

    const QString profileId = BrowserPaths::activeProfileId();

    if (method == QLatin1String("getFontList"))
        return okReply(fontListJson());

    // Every op's first argument is a details object.
    const QJsonValue detailsValue =
        args.value(QLatin1String("details"));
    QJsonObject details;
    if (detailsValue.isObject())
        details = detailsValue.toObject();
    else if (!detailsValue.isUndefined())
        return errReply(QStringLiteral("invalid-argument"));

    // ---- generic-family font ops -----------------------------------
    if (method == QLatin1String("getFont")
            || method == QLatin1String("setFont")
            || method == QLatin1String("clearFont")) {
        // genericFamily defaults to 'standard' like Chrome.
        const QJsonValue familyValue =
            details.value(QLatin1String("genericFamily"));
        QString familyName = QStringLiteral("standard");
        if (!familyValue.isUndefined()) {
            if (!familyValue.isString())
                return errReply(QStringLiteral("invalid-argument"));
            familyName = familyValue.toString();
        }
        const FontFamilyDef *def = fontFamilyDefFor(familyName);
        // A name Chrome never published is the argument verdict; a
        // published one this build cannot map ('math') is the typed
        // refusal — the grant was honored, the surface is missing.
        if (!def)
            return errReply(QStringLiteral("invalid-argument"));
        if (!def->served)
            return errReply(QStringLiteral("unsupported"));

        // Per-script fonts have no QWebEngineSettings granularity —
        // only Chrome's 'Zyyy' (common) row is servable.
        const QJsonValue scriptValue =
            details.value(QLatin1String("script"));
        QString script = QStringLiteral("Zyyy");
        if (!scriptValue.isUndefined()) {
            if (!scriptValue.isString())
                return errReply(QStringLiteral("invalid-argument"));
            script = scriptValue.toString();
            if (script != QLatin1String("Zyyy"))
                return errReply(QStringLiteral("unsupported"));
        }

        const QString key = QLatin1String(def->name);

        if (method == QLatin1String("getFont")) {
            // Chrome's FontDetails: the resolved fontId plus the
            // request's family/script echoed back.
            return okReply(QJsonObject{
                {QStringLiteral("fontId"),
                 fontFamilyValue(*def, profileId)},
                {QStringLiteral("genericFamily"), familyName},
                {QStringLiteral("script"), script},
                {QStringLiteral("levelOfControl"),
                 fontLevelOfControl(key, m_extensionId)}});
        }
        if (method == QLatin1String("setFont")) {
            const QJsonValue idValue =
                details.value(QLatin1String("fontId"));
            if (!idValue.isString())
                return errReply(QStringLiteral("invalid-argument"));
            const QString fontId = idValue.toString();
            // Chrome's fontId names a system font — anything else is
            // an argument error, never a silently-registered miss.
            if (!QFontDatabase::families().contains(fontId))
                return errReply(QStringLiteral("invalid-argument"));

            FontControl &control = fontControl()[key];
            if (control.extensionId.isEmpty()) {
                control.prior = fontFamilyValue(*def, profileId);
                control.hasPrior = true;
            }
            fontWriteFamily(*def, fontId, profileId);
            control.extensionId = m_extensionId;
            fontFanOut(QStringLiteral("onFontChanged"),
                       QJsonObject{
                           {QStringLiteral("fontId"), fontId},
                           {QStringLiteral("genericFamily"),
                            familyName},
                           {QStringLiteral("script"), script}},
                       m_extensionId, this);
            return okReply();
        }
        // clearFont — the controller's restore-default; a clear by a
        // non-controller (or on an unclaimed family) is Chrome's
        // silent no-op.
        FontControl &control = fontControl()[key];
        if (control.extensionId == m_extensionId) {
            if (control.hasPrior)
                fontWriteFamily(*def, control.prior.toString(),
                                profileId);
            control = FontControl();
            fontFanOut(QStringLiteral("onFontChanged"),
                       QJsonObject{
                           {QStringLiteral("fontId"),
                            fontFamilyValue(*def, profileId)},
                           {QStringLiteral("genericFamily"),
                            familyName},
                           {QStringLiteral("script"), script}},
                       m_extensionId, this);
        }
        return okReply();
    }

    // ---- the three get/set/clear*FontSize triples ------------------
    struct SizeMemberDef {
        const char *prefix;
        FontSizeMember member;
        const char *event;
    };
    static const SizeMemberDef kSizeMembers[] = {
        { "DefaultFontSize",      FontSizeMember::DefaultFontSize,
          "onDefaultFontSizeChanged" },
        { "DefaultFixedFontSize", FontSizeMember::DefaultFixedFontSize,
          "onFixedFontSizeChanged" },
        { "MinimumFontSize",      FontSizeMember::MinimumFontSize,
          "onMinimumFontSizeChanged" },
    };
    for (const SizeMemberDef &def : kSizeMembers) {
        const QString prefix = QLatin1String(def.prefix);
        const QString key = prefix;
        if (method == QLatin1String("get") + prefix) {
            return okReply(QJsonObject{
                {QStringLiteral("pixelSize"),
                 fontSizeValue(def.member, profileId)},
                {QStringLiteral("levelOfControl"),
                 fontLevelOfControl(key, m_extensionId)}});
        }
        if (method == QLatin1String("set") + prefix) {
            const QJsonValue size =
                details.value(QLatin1String("pixelSize"));
            if (!size.isDouble())
                return errReply(QStringLiteral("invalid-argument"));
            const double px = size.toDouble();
            // The engine stores an int pixel size; outside a sane
            // bound is an argument error, never a silent clamp.
            const int floor =
                def.member == FontSizeMember::MinimumFontSize ? 0 : 1;
            if (px < floor || px > 500)
                return errReply(QStringLiteral("invalid-argument"));

            FontControl &control = fontControl()[key];
            if (control.extensionId.isEmpty()) {
                control.prior = fontSizeValue(def.member, profileId);
                control.hasPrior = true;
            }
            fontWriteSize(def.member, qRound(px), profileId);
            control.extensionId = m_extensionId;
            fontFanOut(QLatin1String(def.event),
                       QJsonObject{{QStringLiteral("pixelSize"),
                                    qRound(px)}},
                       m_extensionId, this);
            return okReply();
        }
        if (method == QLatin1String("clear") + prefix) {
            FontControl &control = fontControl()[key];
            if (control.extensionId == m_extensionId) {
                if (control.hasPrior)
                    fontWriteSize(def.member, control.prior.toInt(),
                                  profileId);
                control = FontControl();
                fontFanOut(QLatin1String(def.event),
                           QJsonObject{{QStringLiteral("pixelSize"),
                                        fontSizeValue(def.member,
                                                      profileId)}},
                           m_extensionId, this);
            }
            return okReply();
        }
    }

    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.cookies — MD23.  Reads answer out of the RC15 persistent
// mirror (the jar's on-disk shadow the CookieJar signals keep
// current; the C++ jar's live mirror covers no-rust and FFI
// failure); set/remove write the QWebEngineCookieStore live jar AND
// the mirror synchronously, so a same-context get() sees its own
// change.  The engine's cookieAdded/cookieRemoved then fire the
// onChanged broadcast through tapCookieEvents — one event per
// landed change, API-driven ones included, so nothing is emitted
// twice.
//
// Chrome's gate shape: 'cookies' unlocks the namespace (ER16);
// host_permissions scope what a context may touch — every
// url-bearing op requires coverage of that url (the same
// requestVisibleTo check webRequest rides; 'tabs' does NOT cover
// cookie access), and getAll drops any row on an uncovered domain.
// Arktos serves exactly one CookieStore, id "0" — the persistent
// jar of the ambient persona (the profile extensions host on);
// any other storeId is Chrome's no-such-store empty answer.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeCookies(
        const QString &method, const QJsonObject &args)
{
    // ER16 shape: Chrome exposes the namespace only under the
    // declared 'cookies' grant — the bridge object answers a direct
    // call from any code in the context, so the gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("cookies")))
        return errReply(QStringLiteral("no-permission"));

    CookieJar *jar =
        CookieJar::instanceFor(BrowserPaths::activeProfileId());
    if (!jar || !jar->profile())
        return errReply(QStringLiteral("no-store"));
    tapCookieEvents(jar);
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    if (!m_cookieCauseNoted) {
        m_cookieCauseNoted = true;
        recordNote(QStringLiteral(
            "cookies.onChanged: Qt's cookie-store signals distinguish "
            "neither evictions, expirations nor overwrites — cause "
            "answers 'explicit' for every change"));
    }

    // The only store this runtime serves.  A details.storeId naming
    // anything else is Chrome's no-such-store empty answer, not an
    // error.
    const auto knownStore = [](const QJsonObject &details) {
        const QString id = details.value(QLatin1String("storeId"))
            .toString();
        return id.isEmpty() || id == QLatin1String("0");
    };
    const QString kStoreId = QStringLiteral("0");

    // Host coverage — Chromium fails url-bearing ops whose url the
    // extension's host_permissions don't cover; cookie access has no
    // 'tabs' shortcut.
    const auto urlCovered = [this, runtime](const QString &url) {
        return runtime->requestVisibleTo(m_extensionId, QUrl(url));
    };
    const auto domainCovered = [this, runtime](const QString &domain) {
        return runtime->requestVisibleTo(
            m_extensionId, QUrl(QStringLiteral("https://") + domain
                                + QLatin1Char('/')));
    };

    if (method == QLatin1String("getAllCookieStores")) {
        // One store — the persistent jar — with the live tab ids
        // bound to its profile, Chrome's {id,tabIds} shape.
        QJsonArray tabIds;
        for (BrowserMainWindow *window
             : ExtensionEventHub::browsingWindows()) {
            if (!window || !window->tabWidget())
                continue;
            TabWidget *tw = window->tabWidget();
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (!ExtensionEventHub::trackable(view)
                        || !view->page())
                    continue;
                if (view->page()->profile() == jar->profile())
                    tabIds.append(idFor(view));
            }
        }
        return okReply(QJsonArray{QJsonObject{
            {QStringLiteral("id"), kStoreId},
            {QStringLiteral("tabIds"), tabIds}}});
    }

    if (method == QLatin1String("get")) {
        const QJsonValue urlArg = args.value(QLatin1String("url"));
        const QJsonValue nameArg = args.value(QLatin1String("name"));
        if (!urlArg.isString() || !nameArg.isString()
                || nameArg.toString().isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        const QString url = urlArg.toString();
        const QString name = nameArg.toString();
        const QUrl qurl(url);
        if (!qurl.isValid() || qurl.host().isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        if (!knownStore(args))
            return okReply();
        if (!urlCovered(url))
            return errReply(QStringLiteral("no-host-access"));
        // for_url answers RFC6265 send order — the first name match
        // is the cookie Chrome's get() returns.
        const QJsonArray rows = cookieRowsForUrl(jar, qurl);
        for (const QJsonValue &v : rows) {
            const QJsonObject rec = v.toObject();
            if (rec.value(QLatin1String("name")).toString() != name)
                continue;
            // The cookie's own domain needs coverage too — a parent-
            // domain row is not reachable through a child url alone.
            if (!domainCovered(
                    rec.value(QLatin1String("domain")).toString()))
                return okReply();
            return okReply(chromeCookieJson(rec, kStoreId));
        }
        return okReply();
    }

    if (method == QLatin1String("getAll")) {
        if (!knownStore(args))
            return okReply(QJsonArray());
        if (args.contains(QLatin1String("partitionKey"))) {
            // No CHIPS partitions in this jar — an explicit
            // partitionKey filter can only match nothing.
            return okReply(QJsonArray());
        }
        QJsonArray rows;
        if (args.value(QLatin1String("url")).isString()) {
            const QUrl qurl(args.value(QLatin1String("url")).toString());
            if (!qurl.isValid() || qurl.host().isEmpty())
                return errReply(QStringLiteral("invalid-argument"));
            if (!urlCovered(qurl.toString()))
                return errReply(QStringLiteral("no-host-access"));
            rows = cookieRowsForUrl(jar, qurl);
        } else {
            rows = cookieMirrorRows(jar);
        }
        const QString fName =
            args.value(QLatin1String("name")).toString();
        QString fDomain =
            args.value(QLatin1String("domain")).toString();
        if (fDomain.startsWith(QLatin1Char('.')))
            fDomain = fDomain.mid(1);
        const QString fPath =
            args.value(QLatin1String("path")).toString();
        const bool filterSecure = args.contains(QLatin1String("secure"));
        const bool secure = args.value(QLatin1String("secure")).toBool();
        const bool filterSession =
            args.contains(QLatin1String("session"));
        const bool session =
            args.value(QLatin1String("session")).toBool();
        QJsonArray out;
        for (const QJsonValue &v : rows) {
            const QJsonObject rec = v.toObject();
            const QString domain =
                rec.value(QLatin1String("domain")).toString();
            const QJsonValue exp = rec.value(QLatin1String("expires"));
            if (!fName.isEmpty()
                    && rec.value(QLatin1String("name")).toString()
                           != fName)
                continue;
            // Chrome: the filter domain matches the cookie's domain
            // or any subdomain of it.
            if (!fDomain.isEmpty() && domain != fDomain
                    && !domain.endsWith(QStringLiteral(".") + fDomain))
                continue;
            if (!fPath.isEmpty()
                    && rec.value(QLatin1String("path")).toString()
                           != fPath)
                continue;
            if (filterSecure
                    && rec.value(QLatin1String("secure")).toBool()
                           != secure)
                continue;
            if (filterSession
                    && (exp.isNull() || exp.isUndefined()) != session)
                continue;
            // Per-cookie access: the extension must hold host
            // coverage of the cookie's domain — the same check
            // Chrome applies before a Cookie object leaves.
            if (!domainCovered(domain))
                continue;
            out.append(chromeCookieJson(rec, kStoreId));
        }
        return okReply(out);
    }

    if (method == QLatin1String("set")) {
        const QString url = args.value(QLatin1String("url")).toString();
        const QString name = args.value(QLatin1String("name")).toString();
        const QUrl qurl(url);
        if (url.isEmpty() || !qurl.isValid())
            return errReply(QStringLiteral("invalid-argument"));
        const QString scheme = qurl.scheme();
        if (scheme != QLatin1String("http")
                && scheme != QLatin1String("https"))
            return errReply(QStringLiteral("scheme-refused"));
        if (qurl.host().isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        if (!knownStore(args))
            return okReply();
        if (args.contains(QLatin1String("partitionKey")))
            return errReply(QStringLiteral("unsupported"));
        if (!urlCovered(url))
            return errReply(QStringLiteral("no-host-access"));

        QNetworkCookie cookie(name.toUtf8(),
                              args.value(QLatin1String("value"))
                                  .toString().toUtf8());
        // Domain: absent -> host cookie on the url's host; present ->
        // a domain cookie that must cover the url's host (the jar
        // marks domain cookies with a leading dot, verbatim Qt).
        QString domain =
            args.value(QLatin1String("domain")).toString();
        if (domain.startsWith(QLatin1Char('.')))
            domain = domain.mid(1);
        if (!domain.isEmpty()) {
            const QString host = qurl.host();
            if (host != domain
                    && !host.endsWith(QStringLiteral(".") + domain))
                return errReply(QStringLiteral("invalid-argument"));
            cookie.setDomain(QStringLiteral(".") + domain);
        } else {
            cookie.setDomain(qurl.host());
        }
        // Path: absent -> the RFC6265 default-path of the url (the
        // directory up to and including the rightmost '/').
        QString path = args.value(QLatin1String("path")).toString();
        if (path.isEmpty()) {
            const QString p = qurl.path();
            const int slash = p.lastIndexOf(QLatin1Char('/'));
            path = slash <= 0 ? QStringLiteral("/")
                              : p.left(slash + 1);
        }
        if (!path.startsWith(QLatin1Char('/')))
            return errReply(QStringLiteral("invalid-argument"));
        cookie.setPath(path);
        cookie.setSecure(args.value(QLatin1String("secure")).toBool());
        cookie.setHttpOnly(
            args.value(QLatin1String("httpOnly")).toBool());
        const QString ss =
            args.value(QLatin1String("sameSite")).toString();
        if (ss == QLatin1String("no_restriction"))
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::None);
        else if (ss == QLatin1String("lax"))
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::Lax);
        else if (ss == QLatin1String("strict"))
            cookie.setSameSitePolicy(QNetworkCookie::SameSite::Strict);
        else if (!ss.isEmpty()
                 && ss != QLatin1String("unspecified"))
            return errReply(QStringLiteral("invalid-argument"));
        if (args.contains(QLatin1String("expirationDate"))) {
            const double secs =
                args.value(QLatin1String("expirationDate")).toDouble();
            if (secs <= 0)
                return errReply(QStringLiteral("invalid-argument"));
            cookie.setExpirationDate(
                QDateTime::fromMSecsSinceEpoch(qint64(secs * 1000)));
        }

        // Live first — the engine lands it asynchronously and fires
        // the onChanged tap; the mirror write is synchronous so a
        // same-context get() sees the set before the engine ack.
        QWebEngineCookieStore *store = jar->profile()->cookieStore();
        if (!store)
            return errReply(QStringLiteral("no-store"));
        store->setCookie(cookie, qurl);
#ifdef ARKTOS_RUSTCORE
        {
            AcCookies *h = nullptr;
            const QString profileId = jar->profileId();
            if (RustCoreBridge *bridge = RustCoreBridge::serving())
                h = bridge->cookies(profileId);
            if (!h)
                legacyCookiesLane(profileId);
            const QByteArray json = QJsonDocument(
                cookieRecordJson(cookie)).toJson(QJsonDocument::Compact);
            const AcStatus st = h
                ? ac_cookies_set(
                      h, reinterpret_cast<const uint8_t *>(
                             json.constData()), size_t(json.size()))
                : rc_cookies_set(
                      reinterpret_cast<const uint8_t *>(
                          json.constData()), size_t(json.size()));
            ac_cookies_close(h);
            if (st != AC_OK)
                return errReply(QStringLiteral("store-failed"));
        }
#endif
        return okReply(chromeCookieJson(cookieRecordJson(cookie),
                                        kStoreId));
    }

    if (method == QLatin1String("remove")) {
        const QJsonValue urlArg = args.value(QLatin1String("url"));
        const QJsonValue nameArg = args.value(QLatin1String("name"));
        if (!urlArg.isString() || !nameArg.isString()
                || nameArg.toString().isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        const QString url = urlArg.toString();
        const QString name = nameArg.toString();
        const QUrl qurl(url);
        if (!qurl.isValid() || qurl.host().isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        if (!knownStore(args))
            return okReply();
        if (!urlCovered(url))
            return errReply(QStringLiteral("no-host-access"));
        // Chrome removes the cookie get() would return — the first
        // name match in send order.
        const QJsonArray rows = cookieRowsForUrl(jar, qurl);
        for (const QJsonValue &v : rows) {
            const QJsonObject rec = v.toObject();
            if (rec.value(QLatin1String("name")).toString() != name)
                continue;
            const QString domain =
                rec.value(QLatin1String("domain")).toString();
            if (!domainCovered(domain))
                return okReply();
            QWebEngineCookieStore *store =
                jar->profile()->cookieStore();
            if (!store)
                return errReply(QStringLiteral("no-store"));
            // Chromium deletes by exact stored identity — hand back
            // the verbatim cookie object the engine reported (the
            // same discipline handleCookieAdded/setCookies keep), not
            // a record rebuilt from the mirror.
            const QString recPath =
                rec.value(QLatin1String("path")).toString();
            QNetworkCookie doomed;
            for (const QNetworkCookie &c : jar->cookies()) {
                QString cDomain = c.domain();
                if (cDomain.startsWith(QLatin1Char('.')))
                    cDomain = cDomain.mid(1);
                if (QString::fromUtf8(c.name()) == name
                        && cDomain == domain && c.path() == recPath) {
                    doomed = c;
                    break;
                }
            }
            if (doomed.name().isEmpty()) {
                doomed = QNetworkCookie(
                    rec.value(QLatin1String("name")).toString().toUtf8(),
                    rec.value(QLatin1String("value")).toString().toUtf8());
                doomed.setDomain(
                    rec.value(QLatin1String("hostOnly")).toBool()
                        ? domain : QStringLiteral(".") + domain);
                doomed.setPath(recPath);
            }
            store->deleteCookie(doomed);
#ifdef ARKTOS_RUSTCORE
            {
                AcCookies *h = nullptr;
                const QString profileId = jar->profileId();
                if (RustCoreBridge *bridge = RustCoreBridge::serving())
                    h = bridge->cookies(profileId);
                if (!h)
                    legacyCookiesLane(profileId);
                const QByteArray d = domain.toUtf8();
                const QByteArray n =
                    rec.value(QLatin1String("name")).toString().toUtf8();
                const QByteArray p =
                    rec.value(QLatin1String("path")).toString().toUtf8();
                if (h)
                    ac_cookies_remove(h, d.constData(), n.constData(),
                                      p.constData(), "");
                else
                    rc_cookies_remove(d.constData(), n.constData(),
                                      p.constData(), "");
                ac_cookies_close(h);
            }
#endif
            QJsonObject removed{{QStringLiteral("url"), url},
                                {QStringLiteral("name"), name},
                                {QStringLiteral("storeId"), kStoreId}};
            return okReply(removed);
        }
        return okReply();
    }

    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.downloads — MD24.  The subset over DownloadManager's record
// list: download() rides the manager's own policy path (the dlwpolicy
// verdict, save-as prompt gate, external-handler claim and rustdl
// handoff all run exactly like a page-issued download) through the
// hidden retry page; search/erase enumerate the same item cards the
// sidebar renders; pause/resume/cancel/open/show/removeFile drive the
// card's own actions.  The 'downloads' manifest permission gates the
// namespace (ER16) and the onCreated/onChanged/onErased fan-out —
// every event is tap-driven so API and UI changes fan out
// identically.  Off-the-record rows are invisible throughout: Chrome
// never surfaces incognito downloads to regular-profile contexts, so
// downloadsItemJson answers them empty and every enumeration skips
// the empty shape.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::downloadsItemJson(DownloadItem *item)
{
    using State = Engine::DownloadRequest::State;
    QJsonObject o;
    if (!item || item->m_offTheRecord)
        return o;
    o.insert(QStringLiteral("id"), idFor(item));
    const QString url = QString::fromUtf8(item->m_url.toEncoded());
    o.insert(QStringLiteral("url"), url);
    // finalUrl: the card keeps only the request url — no redirect
    // chain is tracked, so the honest answer echoes it.
    o.insert(QStringLiteral("finalUrl"), url);
    o.insert(QStringLiteral("filename"),
             item->m_outputFileName.isEmpty()
                 ? QString()
                 : QFileInfo(item->m_outputFileName).absoluteFilePath());
    o.insert(QStringLiteral("incognito"), false);
    // danger — the SEC01 threat verdict's honest residue: flagged
    // names/mimes the user saved answer 'accepted', flagged ones that
    // never completed answer 'file'; unflagged rows answer
    // 'noValidatedDanger' — Chrome's "no known danger" default, the
    // exact truth here (no safe-browsing verdict ever runs).
    {
        const bool flagged =
            DownloadItem::isDangerousExtension(item->m_outputFileName)
            || DownloadItem::isExecutableMimeType(item->mimeType());
        o.insert(QStringLiteral("danger"),
                 !flagged ? QStringLiteral("noValidatedDanger")
                 : item->downloadedSuccessfully()
                     ? QStringLiteral("accepted")
                     : QStringLiteral("file"));
    }
    o.insert(QStringLiteral("mime"), item->mimeType());
    if (item->startedTime().isValid())
        o.insert(QStringLiteral("startTime"),
                 item->startedTime().toUTC()
                     .toString(Qt::ISODateWithMs));
    if (item->finishedTime().isValid())
        o.insert(QStringLiteral("endTime"),
                 item->finishedTime().toUTC()
                     .toString(Qt::ISODateWithMs));
    // state — a restored card reports Requested with no live backend;
    // split it by the card's own verdicts (restored-complete vs
    // retryable-interrupted).
    QString state;
    switch (item->currentState()) {
    case State::Completed:
        state = QStringLiteral("complete");
        break;
    case State::Cancelled:
    case State::Interrupted:
        state = QStringLiteral("interrupted");
        break;
    default:
        break;
    }
    if (state.isEmpty()) {
        bool haveBackend = item->m_download != nullptr;
#ifdef ARKTOS_RUSTDL
        haveBackend = haveBackend || item->m_engine != nullptr;
#endif
        state = !haveBackend
            ? (item->downloadedSuccessfully()
                   ? QStringLiteral("complete")
                   : QStringLiteral("interrupted"))
            : QStringLiteral("in_progress");
    }
    o.insert(QStringLiteral("state"), state);
    const bool paused = item->isPaused();
    o.insert(QStringLiteral("paused"), paused);
    o.insert(QStringLiteral("canResume"), paused && item->canPause());
    // error — Chrome's InterruptReason vocabulary: a cancelled card
    // answers USER_CANCELED; a genuine interruption maps the engine's
    // numeric reason where one is reachable (the Qt enum carries
    // Chromium's own values).  Restored rows and the rustdl backend
    // carry only a reason string — error stays honestly undefined.
    QString error;
    if (item->currentState() == State::Cancelled) {
        error = QStringLiteral("USER_CANCELED");
    } else if (item->currentState() == State::Interrupted) {
        if (WebEngineDownloadRequest *wrapped =
                WebEngineDownloadRequest::of(item->m_download)) {
            if (QWebEngineDownloadRequest *req =
                    wrapped->webEngineRequest()) {
                using R = QWebEngineDownloadRequest;
                switch (req->interruptReason()) {
                case R::FileFailed:
                    error = QStringLiteral("FILE_FAILED"); break;
                case R::FileAccessDenied:
                    error = QStringLiteral("FILE_ACCESS_DENIED"); break;
                case R::FileNoSpace:
                    error = QStringLiteral("FILE_NO_SPACE"); break;
                case R::FileNameTooLong:
                    error = QStringLiteral("FILE_NAME_TOO_LONG"); break;
                case R::FileTooLarge:
                    error = QStringLiteral("FILE_TOO_LARGE"); break;
                case R::FileVirusInfected:
                    error = QStringLiteral("FILE_VIRUS_INFECTED"); break;
                case R::FileTransientError:
                    error = QStringLiteral("FILE_TRANSIENT_ERROR"); break;
                case R::FileBlocked:
                    error = QStringLiteral("FILE_BLOCKED"); break;
                case R::FileSecurityCheckFailed:
                    error = QStringLiteral("FILE_SECURITY_CHECK_FAILED");
                    break;
                case R::FileTooShort:
                    error = QStringLiteral("FILE_TOO_SHORT"); break;
                case R::FileHashMismatch:
                    error = QStringLiteral("FILE_HASH_MISMATCH"); break;
                case R::NetworkFailed:
                    error = QStringLiteral("NETWORK_FAILED"); break;
                case R::NetworkTimeout:
                    error = QStringLiteral("NETWORK_TIMEOUT"); break;
                case R::NetworkDisconnected:
                    error = QStringLiteral("NETWORK_DISCONNECTED"); break;
                case R::NetworkServerDown:
                    error = QStringLiteral("NETWORK_SERVER_DOWN"); break;
                case R::NetworkInvalidRequest:
                    error = QStringLiteral("NETWORK_INVALID_REQUEST");
                    break;
                case R::ServerFailed:
                    error = QStringLiteral("SERVER_FAILED"); break;
                case R::ServerBadContent:
                    error = QStringLiteral("SERVER_BAD_CONTENT"); break;
                case R::ServerUnauthorized:
                    error = QStringLiteral("SERVER_UNAUTHORIZED"); break;
                case R::ServerCertProblem:
                    error = QStringLiteral("SERVER_CERT_PROBLEM"); break;
                case R::ServerForbidden:
                    error = QStringLiteral("SERVER_FORBIDDEN"); break;
                case R::ServerUnreachable:
                    error = QStringLiteral("SERVER_UNREACHABLE"); break;
                case R::UserCanceled:
                    error = QStringLiteral("USER_CANCELED"); break;
                default:
                    break;
                }
            }
        }
    }
    if (!error.isEmpty())
        o.insert(QStringLiteral("error"), error);
    const QString path = item->m_outputFileName;
    o.insert(QStringLiteral("exists"),
             !path.isEmpty() && QFileInfo::exists(path));
    qint64 fileSize = item->bytesTotal();
    if (fileSize <= 0)
        fileSize = qMax(item->bytesReceived(),
                        item->m_restoredTotalBytes);
    o.insert(QStringLiteral("fileSize"), double(fileSize));
    o.insert(QStringLiteral("totalBytes"), double(fileSize));
    o.insert(QStringLiteral("bytesReceived"),
             double(item->bytesReceived()));
    // Chrome's byExtId/byExtName — the ticket claim stamps these on
    // API-initiated downloads.
    const QString byId = item->property("arktosExtDlBy").toString();
    if (!byId.isEmpty()) {
        o.insert(QStringLiteral("byExtId"), byId);
        const QString byName =
            item->property("arktosExtDlName").toString();
        if (!byName.isEmpty())
            o.insert(QStringLiteral("byExtName"), byName);
    }
    return o;
}

QJsonObject ExtensionShimBackend::downloadsDeltaJson(
        DownloadItem *item, bool finished)
{
    const QJsonObject full = downloadsItemJson(item);
    QJsonObject delta;
    if (full.isEmpty())
        return delta;
    const auto current = [&full](const QString &key) {
        return QJsonObject{
            {QStringLiteral("current"), full.value(key)}};
    };
    delta.insert(QStringLiteral("state"),
                 current(QStringLiteral("state")));
    delta.insert(QStringLiteral("paused"),
                 current(QStringLiteral("paused")));
    delta.insert(QStringLiteral("canResume"),
                 current(QStringLiteral("canResume")));
    if (finished) {
        if (full.contains(QLatin1String("endTime")))
            delta.insert(QStringLiteral("endTime"),
                         current(QStringLiteral("endTime")));
        if (full.contains(QLatin1String("error")))
            delta.insert(QStringLiteral("error"),
                         current(QStringLiteral("error")));
        delta.insert(QStringLiteral("exists"),
                     current(QStringLiteral("exists")));
        delta.insert(QStringLiteral("fileSize"),
                     current(QStringLiteral("fileSize")));
        delta.insert(QStringLiteral("danger"),
                     current(QStringLiteral("danger")));
        delta.insert(QStringLiteral("mime"),
                     current(QStringLiteral("mime")));
    }
    return delta;
}

void ExtensionShimBackend::downloadsPauseTap(DownloadItem *item)
{
    if (!item || !item->m_download)
        return;
    WebEngineDownloadRequest *raw =
        WebEngineDownloadRequest::of(item->m_download);
    if (!raw || !raw->webEngineRequest())
        return;
    const QPointer<DownloadItem> weak(item);
    QObject::connect(raw->webEngineRequest(),
        &QWebEngineDownloadRequest::isPausedChanged, item, [weak]() {
            if (!weak || !weak->property("arktosDlVisible").toBool())
                return;
            QJsonObject delta = downloadsDeltaJson(weak, false);
            delta.insert(QStringLiteral("id"), idFor(weak.data()));
            dlBroadcast(QStringLiteral("downloads.onChanged"),
                        QJsonArray{delta});
        });
}

bool ExtensionShimBackend::downloadsClaimPending(DownloadItem *item)
{
    if (!item)
        return false;
    Engine::Page *issuer = item->page();
    QList<DlPending> &pending = dlPendingList();
    for (int i = 0; i < pending.size(); ++i) {
        const DlPending &p = pending.at(i);
        if (p.url != item->m_url)
            continue;
        // The issuer check keeps a UI-driven download of the same
        // url from stealing an API ticket; a request whose page()
        // already died falls back to the url match.
        if (issuer && p.issuer && issuer != p.issuer)
            continue;
        item->setProperty("arktosExtDlBy", p.extId);
        item->setProperty("arktosExtDlName", p.extName);
        // page->download() requests arrive already accepted — the
        // card's setDownloadDirectory/setDownloadFileName are refused
        // and the engine writes under its own profile default with
        // its own name.  Record the real landing target now (the
        // request can be dead by finish) so downloadsFileFixup can
        // reconcile it with the recorded name Chrome reports.
        if (WebEngineDownloadRequest *raw =
                WebEngineDownloadRequest::of(item->m_download))
            if (QWebEngineDownloadRequest *r = raw->webEngineRequest())
                if (!r->downloadDirectory().isEmpty()
                        && !r->downloadFileName().isEmpty())
                    item->setProperty("arktosExtDlActual",
                        r->downloadDirectory() + QLatin1Char('/')
                            + r->downloadFileName());
        const QPointer<DownloadItem> weak(item);
        QObject::connect(item, &DownloadItem::downloadFinished, item,
                         [weak]() {
            ExtensionShimBackend::downloadsFileFixup(weak.data());
        });
        const DlPending taken = pending.takeAt(i);
        dlAnswerTicket(taken, QJsonObject{
            {QStringLiteral("ticket"), taken.ticket},
            {QStringLiteral("id"), idFor(item)}});
        return true;
    }
    return false;
}

// The page->download() auto-accept quirk, reconciled: a completed
// file is moved onto the card's recorded name (the one the API's
// 'filename' suggestion produced and every search answer reports);
// a cancelled/interrupted transfer's engine-side partial is removed
// — Chrome's cancel drops the partial too.  Only API-minted items
// carry 'arktosExtDlActual', so ordinary downloads are untouched.
void ExtensionShimBackend::downloadsFileFixup(DownloadItem *item)
{
    if (!item)
        return;
    const QString actualProp =
        item->property("arktosExtDlActual").toString();
    if (actualProp.isEmpty())
        return;
    const QString actual = QFileInfo(actualProp).absoluteFilePath();
    const QString wanted =
        QFileInfo(item->m_outputFileName).absoluteFilePath();
    if (item->downloadedSuccessfully()) {
        if (actual != wanted && QFileInfo(actual).isFile()
                && !wanted.isEmpty() && !QFileInfo::exists(wanted)
                && !QFile::rename(actual, wanted)) {
            // Cross-mount destinations fall back to copy+remove.
            if (QFile::copy(actual, wanted))
                QFile::remove(actual);
        }
        return;
    }
    for (const QString &p : {actual,
                             actual + QLatin1String(".download")}) {
        const QFileInfo info(p);
        if (info.isFile() && !info.isSymLink())
            QFile::remove(p);
    }
}

// DownloadQuery time fields accept either epoch ms or an
// ISO-parseable string — Chrome feeds both forms through Date.
bool dlQueryTime(const QJsonValue &value, qint64 *out)
{
    if (value.isDouble()) {
        *out = qint64(value.toDouble());
        return true;
    }
    if (value.isString()) {
        const QDateTime dt = QDateTime::fromString(
            value.toString(), Qt::ISODateWithMs);
        if (dt.isValid()) {
            *out = dt.toMSecsSinceEpoch();
            return true;
        }
    }
    return false;
}

bool ExtensionShimBackend::downloadsQueryMatch(
        const QJsonObject &query, DownloadItem *item)
{
    const QJsonObject j = downloadsItemJson(item);
    if (j.isEmpty())
        return false;
    if (query.contains(QLatin1String("id"))
            && j.value(QLatin1String("id")).toInt()
                   != int(query.value(QLatin1String("id")).toDouble()))
        return false;
    // substring filters, case-insensitive like Chrome's own match.
    for (const QString &key : {QStringLiteral("url"),
                               QStringLiteral("finalUrl"),
                               QStringLiteral("filename"),
                               QStringLiteral("mime")}) {
        const QString want = query.value(key).toString();
        if (!want.isEmpty()
                && !j.value(key).toString().contains(
                    want, Qt::CaseInsensitive))
            return false;
    }
    // query[] terms — each must hit the url or the filename.
    const QJsonArray terms = query.value(QLatin1String("query")).toArray();
    for (const QJsonValue &t : terms) {
        const QString term = t.toString();
        if (term.isEmpty())
            continue;
        if (!j.value(QLatin1String("url")).toString()
                    .contains(term, Qt::CaseInsensitive)
                && !j.value(QLatin1String("filename")).toString()
                    .contains(term, Qt::CaseInsensitive))
            return false;
    }
    for (const QString &key : {QStringLiteral("state"),
                               QStringLiteral("danger")}) {
        if (query.contains(key)
                && j.value(key).toString() != query.value(key).toString())
            return false;
    }
    for (const QString &key : {QStringLiteral("paused"),
                               QStringLiteral("canResume"),
                               QStringLiteral("exists"),
                               QStringLiteral("incognito")}) {
        if (query.contains(key)
                && j.value(key).toBool() != query.value(key).toBool())
            return false;
    }
    if (query.contains(QLatin1String("error"))
            && j.value(QLatin1String("error")).toString()
                   != query.value(QLatin1String("error")).toString())
        return false;
    // *Regex filters — the caller validated the patterns already.
    for (const QString &key : {QStringLiteral("urlRegex"),
                               QStringLiteral("finalUrlRegex"),
                               QStringLiteral("filenameRegex")}) {
        if (!query.contains(key))
            continue;
        const QString field = key.left(key.size() - 5);
        const QRegularExpression re(query.value(key).toString());
        if (!re.match(j.value(field).toString()).hasMatch())
            return false;
    }
    // Time bounds — startedAfter/Before on startTime, endedAfter/
    // Before on endTime; a row missing the bound's field fails the
    // bound.
    const QDateTime started = QDateTime::fromString(
        j.value(QLatin1String("startTime")).toString(),
        Qt::ISODateWithMs);
    const QDateTime ended = QDateTime::fromString(
        j.value(QLatin1String("endTime")).toString(), Qt::ISODateWithMs);
    qint64 bound = 0;
    if (query.contains(QLatin1String("startedAfter"))
            && (!dlQueryTime(query.value(QLatin1String("startedAfter")),
                             &bound)
                || !started.isValid()
                || started.toMSecsSinceEpoch() < bound))
        return false;
    if (query.contains(QLatin1String("startedBefore"))
            && (!dlQueryTime(query.value(QLatin1String("startedBefore")),
                             &bound)
                || !started.isValid()
                || started.toMSecsSinceEpoch() > bound))
        return false;
    if (query.contains(QLatin1String("endedAfter"))
            && (!dlQueryTime(query.value(QLatin1String("endedAfter")),
                             &bound)
                || !ended.isValid()
                || ended.toMSecsSinceEpoch() < bound))
        return false;
    if (query.contains(QLatin1String("endedBefore"))
            && (!dlQueryTime(query.value(QLatin1String("endedBefore")),
                             &bound)
                || !ended.isValid()
                || ended.toMSecsSinceEpoch() > bound))
        return false;
    if (query.contains(QLatin1String("totalBytesGreater"))
            && j.value(QLatin1String("fileSize")).toDouble()
                   <= query.value(QLatin1String("totalBytesGreater"))
                          .toDouble())
        return false;
    if (query.contains(QLatin1String("totalBytesLess"))
            && j.value(QLatin1String("fileSize")).toDouble()
                   >= query.value(QLatin1String("totalBytesLess"))
                          .toDouble())
        return false;
    return true;
}

QJsonObject ExtensionShimBackend::invokeDownloads(
        const QString &method, const QJsonObject &args)
{
    // ER16 shape: Chrome exposes the namespace only under the
    // declared 'downloads' grant — the bridge object answers a direct
    // call from any code in the context, so the gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("downloads")))
        return errReply(QStringLiteral("no-permission"));

    DownloadManager *manager =
        DownloadManager::instanceFor(BrowserPaths::activeProfileId());
    if (!manager)
        return errReply(QStringLiteral("no-store"));
    tapDlEvents(manager);
    if (!m_dlFinalUrlNoted) {
        m_dlFinalUrlNoted = true;
        recordNote(QStringLiteral(
            "downloads: finalUrl echoes the request url (no redirect "
            "tracking); danger answers 'noValidatedDanger' for "
            "unflagged rows — Arktos runs no safe-browsing verdict"));
    }

    // (item, json) pairs — the json carries the OTR filter: an empty
    // object means the row is invisible to this namespace.
    const auto snapshot = [manager]() {
        QList<QPair<DownloadItem *, QJsonObject>> out;
        const int rows =
            manager->model() ? manager->model()->rowCount() : 0;
        for (int r = 0; r < rows; ++r) {
            DownloadItem *item = manager->itemAt(r);
            if (!item)
                continue;
            const QJsonObject json = downloadsItemJson(item);
            if (json.isEmpty())
                continue;
            out.append({item, json});
        }
        return out;
    };

    if (method == QLatin1String("download")) {
        const QString urlText =
            args.value(QLatin1String("url")).toString();
        const QUrl url(urlText);
        if (urlText.isEmpty() || !url.isValid() || url.isRelative())
            return errReply(QStringLiteral("invalid-argument"));
        // The manager's own path handles the remote-download schemes
        // plus data: (the fixture channel its autotests prove);
        // everything else is refused before it reaches the engine.
        const QString scheme = url.scheme().toLower();
        if (scheme != QLatin1String("http")
                && scheme != QLatin1String("https")
                && scheme != QLatin1String("ftp")
                && scheme != QLatin1String("data"))
            return errReply(QStringLiteral("scheme-refused"));
        // Options the engine path cannot honour — a POST or custom
        // headers would silently fetch the wrong thing, so they are
        // typed refusals, never dropped.
        const QString httpMethod =
            args.value(QLatin1String("method")).toString().toUpper();
        if (!httpMethod.isEmpty()
                && httpMethod != QLatin1String("GET"))
            return errReply(QStringLiteral("unsupported"));
        if (args.contains(QLatin1String("headers"))
                || args.contains(QLatin1String("body")))
            return errReply(QStringLiteral("unsupported"));
        const QString conflict =
            args.value(QLatin1String("conflictAction")).toString();
        if (conflict == QLatin1String("overwrite"))
            // The naming path only ever uniquifies — silently
            // ignoring 'overwrite' could delete a user's file.
            return errReply(QStringLiteral("unsupported"));
        if (!conflict.isEmpty()
                && conflict != QLatin1String("uniquify")
                && conflict != QLatin1String("prompt"))
            return errReply(QStringLiteral("invalid-argument"));
        const bool saveAs =
            args.value(QLatin1String("saveAs")).toBool()
            || conflict == QLatin1String("prompt");
        const QString filename =
            args.value(QLatin1String("filename")).toString();
        Engine::Page *page = manager->retryPage(false);
        if (!page)
            return errReply(QStringLiteral("start-failed"));
        const int ticket = ++s_dlTicketSeq;
        DlPending pending;
        pending.ticket = ticket;
        pending.extId = m_extensionId;
        pending.extName = m_extensionName;
        pending.url = url;
        pending.issuer = page;
        pending.backend = this;
        dlPendingList().append(pending);
        // The request can resolve to no item at all (a dlwpolicy
        // block, an external-handler claim, a dead network service) —
        // bound the wait so the JS waiter cannot hang forever.
        QTimer::singleShot(30000, this, [ticket]() {
            QList<DlPending> &list = dlPendingList();
            for (int i = 0; i < list.size(); ++i) {
                if (list.at(i).ticket != ticket)
                    continue;
                const DlPending p = list.takeAt(i);
                dlAnswerTicket(p, QJsonObject{
                    {QStringLiteral("ticket"), ticket},
                    {QStringLiteral("error"),
                     QStringLiteral("download-failed")}});
                return;
            }
        });
        manager->download(page, url, saveAs, filename);
        return okReply(QJsonObject{{QStringLiteral("ticket"), ticket}});
    }

    if (method == QLatin1String("search")
            || method == QLatin1String("erase")) {
        if (!args.value(QLatin1String("query")).isObject())
            return errReply(QStringLiteral("invalid-argument"));
        const QJsonObject query =
            args.value(QLatin1String("query")).toObject();
        for (const QString &key : {QStringLiteral("urlRegex"),
                                   QStringLiteral("finalUrlRegex"),
                                   QStringLiteral("filenameRegex")}) {
            if (query.contains(key)
                    && !QRegularExpression(query.value(key).toString())
                            .isValid())
                return errReply(QStringLiteral("invalid-argument"));
        }
        int limit = 1000;
        if (query.contains(QLatin1String("limit"))) {
            limit = query.value(QLatin1String("limit")).toInt();
            if (limit < 1)
                return errReply(QStringLiteral("invalid-argument"));
        }
        QList<QPair<DownloadItem *, QJsonObject>> matched;
        const QList<QPair<DownloadItem *, QJsonObject>> all = snapshot();
        for (const auto &entry : all) {
            if (downloadsQueryMatch(query, entry.first))
                matched.append(entry);
        }
        // orderBy — '-'<field> descends; Chrome's default order is
        // most-recent-first.
        QStringList orderBy;
        for (const QJsonValue &v
                 : query.value(QLatin1String("orderBy")).toArray())
            orderBy << v.toString();
        if (orderBy.isEmpty())
            orderBy << QStringLiteral("-startTime");
        std::sort(matched.begin(), matched.end(),
                  [&orderBy](const auto &x, const auto &y) {
            for (const QString &key : orderBy) {
                const bool desc = key.startsWith(QLatin1Char('-'));
                const QString field =
                    desc ? key.mid(1) : key;
                const QJsonValue xv = x.second.value(field);
                const QJsonValue yv = y.second.value(field);
                if (xv == yv)
                    continue;
                bool less;
                if (xv.isDouble() && yv.isDouble())
                    less = xv.toDouble() < yv.toDouble();
                else if (xv.isBool() && yv.isBool())
                    less = !xv.toBool() && yv.toBool();
                else
                    less = xv.toString() < yv.toString();
                return desc ? !less : less;
            }
            return false;
        });
        if (method == QLatin1String("search")) {
            QJsonArray out;
            for (const auto &entry : matched) {
                out.append(entry.second);
                if (out.size() >= limit)
                    break;
            }
            return okReply(out);
        }
        // erase — remove matching non-in-flight rows through the
        // model's own removability gate; the rowsAboutToBeRemoved tap
        // fires onErased per row that actually leaves.
        int erased = 0;
        for (const auto &entry : matched) {
            if (erased >= limit)
                break;
            const int row = manager->rowForItem(entry.first);
            if (row >= 0 && manager->model()->removeRow(row))
                ++erased;
        }
        return okReply();
    }

    if (method == QLatin1String("showDefaultFolder")) {
        const QString dir = manager->downloadDirectory();
        if (dir.isEmpty() || !DownloadItem::revealInFileManager(dir))
            return errReply(QStringLiteral("reveal-failed"));
        return okReply();
    }

    // The remaining served members are all id-addressed.
    static const QSet<QString> idOps = {
        QStringLiteral("pause"), QStringLiteral("resume"),
        QStringLiteral("cancel"), QStringLiteral("open"),
        QStringLiteral("show"), QStringLiteral("removeFile"),
        QStringLiteral("getFileIcon")};
    if (idOps.contains(method)) {
        if (!args.value(QLatin1String("id")).isDouble())
            return errReply(QStringLiteral("invalid-argument"));
        const int id = int(args.value(QLatin1String("id")).toDouble());
        DownloadItem *item = nullptr;
        const QList<QPair<DownloadItem *, QJsonObject>> all = snapshot();
        for (const auto &entry : all) {
            if (entry.second.value(QLatin1String("id")).toInt() == id) {
                item = entry.first;
                break;
            }
        }
        if (!item)
            return errReply(QStringLiteral("not-found"));

        if (method == QLatin1String("pause")) {
            if (!item->canPause() || !item->downloading())
                return errReply(QStringLiteral("not-pausable"));
            if (!item->isPaused())
                item->pauseOrResume();
            return okReply();
        }
        if (method == QLatin1String("resume")) {
            if (!item->isPaused() || !item->canPause())
                return errReply(QStringLiteral("not-resumable"));
            item->pauseOrResume();
            return okReply();
        }
        if (method == QLatin1String("cancel")) {
            if (!item->downloading()
                    && item->currentState()
                           != Engine::DownloadRequest::State::Requested)
                return errReply(QStringLiteral("not-in-progress"));
            item->stop();
            return okReply();
        }
        if (method == QLatin1String("open")) {
            if (!item->downloadedSuccessfully())
                return errReply(QStringLiteral("not-complete"));
            const QString path =
                QFileInfo(item->m_outputFileName).absoluteFilePath();
            if (item->m_outputFileName.isEmpty()
                    || !QFileInfo(path).isFile())
                return errReply(QStringLiteral("file-missing"));
            item->open();
            return okReply();
        }
        if (method == QLatin1String("show")) {
            if (item->m_outputFileName.isEmpty()
                    || DownloadItem::revealTarget(
                           item->m_outputFileName).isEmpty())
                return errReply(QStringLiteral("file-missing"));
            if (!DownloadItem::revealInFileManager(
                    item->m_outputFileName))
                return errReply(QStringLiteral("reveal-failed"));
            return okReply();
        }
        if (method == QLatin1String("removeFile")) {
            if (!item->downloadedSuccessfully())
                return errReply(QStringLiteral("not-complete"));
            const QString path =
                QFileInfo(item->m_outputFileName).absoluteFilePath();
            const QFileInfo info(path);
            // Same deletion safety removePartialFile applies — only
            // ever a regular file the browser itself wrote.
            if (!info.isFile() || info.isSymLink())
                return errReply(QStringLiteral("file-missing"));
            if (!QFile::remove(path))
                return errReply(QStringLiteral("file-missing"));
            dlBroadcast(QStringLiteral("downloads.onChanged"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), idFor(item)},
                    {QStringLiteral("exists"), QJsonObject{
                        {QStringLiteral("current"), false}}}}});
            return okReply();
        }
        if (method == QLatin1String("getFileIcon")) {
            const int size =
                args.value(QLatin1String("options")).toObject()
                    .value(QLatin1String("size")).toInt(32);
            const int px = size == 16 ? 16 : 32;
            QFileIconProvider provider;
            QIcon icon =
                provider.icon(QFileInfo(item->m_outputFileName));
            if (icon.isNull() && qApp)
                icon = qApp->style()->standardIcon(
                    QStyle::SP_FileIcon);
            if (icon.isNull())
                return errReply(QStringLiteral("no-icon"));
            const QPixmap pixmap = icon.pixmap(px, px);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            pixmap.save(&buffer, "PNG");
            return okReply(QStringLiteral("data:image/png;base64,")
                           + QString::fromLatin1(png.toBase64()));
        }
    }

    // Chrome-shelf surface — Arktos has no downloads shelf, so the
    // shelf actions and the danger-accept path stay typed.
    for (const QString &m : {QStringLiteral("acceptDanger"),
                             QStringLiteral("drag"),
                             QStringLiteral("setShelfEnabled"),
                             QStringLiteral("setUiOptions")}) {
        if (method == m)
            return errReply(QStringLiteral("unsupported"));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.tabGroups — MD26: get/query/update/move/ungroup over the
// TABGRP machinery (per-window membership keyed on the WebView, so
// drags never lose it).  The strip's group ids are per-window
// strings; Chrome's ints ride the same minted map tabObject's
// groupId field reports.  Group CREATION lives in chrome.tabs.group
// like the real API — update() on an unknown id is Chrome's
// not-found, never a silent create.  The four on* events are
// entirely tap-driven (ExtensionEventHub diffs the strip's group
// set on every tabGroupsChanged), so API and UI mutations fan out
// identically through the broadcast lane — no member-emit path.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::tabGroupJson(TabWidget *tw,
                                             const QString &groupId,
                                             int windowId)
{
    QJsonObject group;
    group.insert(QStringLiteral("id"),
                 chromeGroupId(windowId, groupId));
    group.insert(QStringLiteral("windowId"), windowId);
    group.insert(QStringLiteral("collapsed"),
                 tw && tw->tabGroupIsCollapsed(groupId));
    group.insert(QStringLiteral("color"),
                 chromeGroupColorName(
                     tw ? tw->tabGroupColor(groupId) : QColor()));
    const QString title =
        tw ? tw->tabGroupName(groupId) : QString();
    // Chrome's TabGroup.title is optional — omitted when unnamed.
    if (!title.isEmpty())
        group.insert(QStringLiteral("title"), title);
    return group;
}

int ExtensionShimBackend::chromeGroupIdFor(int windowId,
                                           const QString &groupId)
{
    return chromeGroupId(windowId, groupId);
}

QJsonObject ExtensionShimBackend::invokeTabGroups(
        const QString &method, const QJsonObject &args)
{
    // ER16 shape: 'tabGroups' gates the namespace — the bridge
    // answers a direct call from any code in the context, so the
    // gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("tabGroups")))
        return errReply(QStringLiteral("no-permission"));

    // 'groupId' is a required int for every member but query/ungroup
    // — validate once; a non-numeric value is Chrome's argument
    // error, not the not-found a well-typed miss deserves.
    const auto groupIdArg = [&args](int *out) {
        const QJsonValue v = args.value(QStringLiteral("groupId"));
        if (!v.isDouble())
            return false;
        *out = int(v.toDouble(-1));
        return true;
    };

    if (method == QLatin1String("get")) {
        int chromeId = -1;
        if (!groupIdArg(&chromeId))
            return errReply(QStringLiteral("invalid-argument"));
        BrowserMainWindow *window = nullptr;
        QString gid;
        if (!resolveTabGroup(chromeId, &window, &gid))
            return errReply(QStringLiteral("not-found"));
        return okReply(tabGroupJson(window->tabWidget(), gid,
                                    idFor(window)));
    }

    if (method == QLatin1String("query")) {
        const QJsonObject q =
            args.value(QStringLiteral("query")).toObject();
        BrowserMainWindow *current = currentWindow();
        const int currentWid = idFor(current);
        // Chrome's QueryInfo.shared belongs to shared tab groups —
        // Arktos has none, so an explicit shared:true matches
        // nothing rather than lying about the filter.
        if (q.value(QStringLiteral("shared")).toBool(false))
            return okReply(QJsonArray());
        QJsonArray out;
        for (BrowserMainWindow *window
             : ExtensionEventHub::browsingWindows()) {
            if (!window)
                continue;
            const int wid = idFor(window);
            if (q.contains(QStringLiteral("windowId"))) {
                int wanted = int(q.value(QStringLiteral("windowId"))
                                     .toDouble(-2));
                if (wanted == -2)       // WINDOW_ID_CURRENT
                    wanted = currentWid;
                if (wanted != wid)
                    continue;
            }
            TabWidget *tw = window->tabWidget();
            if (!tw)
                continue;
            for (const QString &gid : tw->tabGroupIds()) {
                const QJsonObject group =
                    tabGroupJson(tw, gid, wid);
                if (q.contains(QStringLiteral("collapsed"))
                        && q.value(QStringLiteral("collapsed"))
                               .toBool()
                               != group.value(
                                   QStringLiteral("collapsed"))
                                   .toBool())
                    continue;
                if (q.contains(QStringLiteral("color"))
                        && q.value(QStringLiteral("color")).toString()
                               != group.value(QStringLiteral("color"))
                                      .toString())
                    continue;
                if (q.contains(QStringLiteral("title"))) {
                    const QString needle =
                        q.value(QStringLiteral("title")).toString();
                    if (!group.value(QStringLiteral("title"))
                            .toString()
                            .contains(needle, Qt::CaseInsensitive))
                        continue;
                }
                if (q.contains(QStringLiteral("status"))) {
                    // "loading" when any visible member loads —
                    // the group-level aggregate of the tab status
                    // tabs.query already reports.
                    bool loading = false;
                    for (const int member : tw->tabGroupMembers(gid)) {
                        WebView *view = tw->webView(member);
                        if (view && view->isLoading()) {
                            loading = true;
                            break;
                        }
                    }
                    if (q.value(QStringLiteral("status")).toString()
                            != QLatin1String(loading ? "loading"
                                                     : "complete"))
                        continue;
                }
                out.append(group);
            }
        }
        return okReply(out);
    }

    if (method == QLatin1String("update")) {
        int chromeId = -1;
        if (!groupIdArg(&chromeId))
            return errReply(QStringLiteral("invalid-argument"));
        BrowserMainWindow *window = nullptr;
        QString gid;
        if (!resolveTabGroup(chromeId, &window, &gid))
            return errReply(QStringLiteral("not-found"));
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        TabWidget *tw = window->tabWidget();
        // An unknown color name is Chrome's argument error — check
        // before ANY mutation so a bad call stays atomic.
        QColor color;
        if (props.contains(QStringLiteral("color"))) {
            color = chromeGroupColor(
                props.value(QStringLiteral("color")).toString());
            if (!color.isValid())
                return errReply(QStringLiteral("invalid-argument"));
        }
        if (props.contains(QStringLiteral("title")))
            tw->renameTabGroup(
                gid, props.value(QStringLiteral("title")).toString());
        if (color.isValid())
            tw->setTabGroupColor(gid, color);
        if (props.contains(QStringLiteral("collapsed")))
            tw->setTabGroupCollapsed(
                gid, props.value(QStringLiteral("collapsed"))
                         .toBool());
        // The group can vanish mid-update only if it died a moment
        // ago — answer the live shape when it still resolves.
        return okReply(tabGroupJson(tw, gid, idFor(window)));
    }

    if (method == QLatin1String("move")) {
        int chromeId = -1;
        if (!groupIdArg(&chromeId))
            return errReply(QStringLiteral("invalid-argument"));
        BrowserMainWindow *window = nullptr;
        QString gid;
        if (!resolveTabGroup(chromeId, &window, &gid))
            return errReply(QStringLiteral("not-found"));
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        TabWidget *tw = window->tabWidget();
        const int wid = idFor(window);
        if (props.contains(QStringLiteral("windowId"))) {
            int wanted = int(
                props.value(QStringLiteral("windowId")).toDouble(-2));
            if (wanted == -2)
                wanted = idFor(currentWindow());
            // TAB02's carry machinery moves a group's members one
            // tab at a time between windows — there is no atomic
            // cross-window group move, so the honest answer is the
            // typed refusal, not a torn run.
            if (wanted != wid)
                return errReply(QStringLiteral("unsupported"));
        }
        const QJsonValue indexValue =
            props.value(QStringLiteral("index"));
        if (!indexValue.isDouble())
            return errReply(QStringLiteral("invalid-argument"));
        int index = int(indexValue.toDouble(-2));
        // Chrome's -1 parks the group at the strip's end; anything
        // lower — or past the last slot — is an argument error.
        if (index == -1)
            index = tw->count();
        else if (index < -1 || index >= tw->count())
            return errReply(QStringLiteral("invalid-argument"));
        if (!tw->moveTabGroup(gid, index))
            return errReply(QStringLiteral("move-refused"));
        return okReply(tabGroupJson(tw, gid, wid));
    }

    if (method == QLatin1String("ungroup")) {
        // Chrome accepts one id or an array; the call is atomic —
        // resolve every id before any dissolve so a stale member of
        // the list leaves the earlier groups untouched.
        const QJsonValue gv =
            args.value(QStringLiteral("groupIds"));
        QList<int> ids;
        if (gv.isArray()) {
            for (const QJsonValue &v : gv.toArray()) {
                if (!v.isDouble())
                    return errReply(
                        QStringLiteral("invalid-argument"));
                ids.append(int(v.toDouble(-1)));
            }
        } else if (gv.isDouble()) {
            ids.append(int(gv.toDouble(-1)));
        }
        if (ids.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        QList<QPair<TabWidget *, QString>> resolved;
        for (const int chromeId : ids) {
            BrowserMainWindow *window = nullptr;
            QString gid;
            if (!resolveTabGroup(chromeId, &window, &gid))
                return errReply(QStringLiteral("not-found"));
            resolved.append({window->tabWidget(), gid});
        }
        for (const auto &entry : resolved)
            entry.first->ungroupTabs(entry.second);
        return okReply();
    }

    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.declarativeNetRequest — MD34.  The rule registry, validation
// and IO-thread evaluation live in DeclarativeNetRequest; this
// handler only shapes arguments and applies Chrome's grant gates:
//   declarativeNetRequest               — whole rule surface
//   declarativeNetRequestWithHostAccess — same, but rule evaluation
//       additionally requires host-permission coverage of the
//       request or initiator (DNR itself grants no host access)
//   declarativeNetRequestFeedback       — getMatchedRules /
//       onRuleMatchedDebug only
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeDeclarativeNetRequest(
        const QString &method, const QJsonObject &args)
{
    const QStringList perms = manifestPermissions();
    const bool hasDnr =
        perms.contains(QLatin1String("declarativeNetRequest"));
    const bool hasHostDnr = perms.contains(
        QLatin1String("declarativeNetRequestWithHostAccess"));
    const bool hasFeedback = perms.contains(
        QLatin1String("declarativeNetRequestFeedback"));
    const bool granted = hasDnr || hasHostDnr;

    DeclarativeNetRequest *dnr = DeclarativeNetRequest::instance();
    const QJsonObject manifest = manifestJson();
    const QString dir = storageDir();

    if (method == QLatin1String("updateSessionRules")
            || method == QLatin1String("updateDynamicRules")) {
        if (!granted)
            return errReply(QStringLiteral("no-permission"));
        const QJsonObject options =
            args.value(QStringLiteral("options")).toObject();
        const QJsonObject reply = dnr->updateRules(
            m_extensionId, m_extensionPath, manifest, dir,
            method == QLatin1String("updateSessionRules"),
            hasHostDnr, hasFeedback,
            options.value(QStringLiteral("addRules")).toArray(),
            options.value(QStringLiteral("removeRuleIds")).toArray());
        if (reply.value(QStringLiteral("ok")).toBool())
            return okReply();
        return errReply(
            reply.value(QStringLiteral("error")).toString());
    }
    if (method == QLatin1String("getSessionRules")
            || method == QLatin1String("getDynamicRules")) {
        if (!granted)
            return errReply(QStringLiteral("no-permission"));
        const QJsonObject filter =
            args.value(QStringLiteral("filter")).toObject();
        QList<int> ids;
        for (const QJsonValue &v :
             filter.value(QStringLiteral("ruleIds")).toArray()) {
            const double d = v.toDouble(-1);
            if (!v.isDouble() || d != std::floor(d) || d < 1)
                return errReply(QStringLiteral("invalid-argument"));
            ids.append(int(d));
        }
        return okReply(dnr->rulesFor(
            m_extensionId, m_extensionPath, manifest, dir,
            method == QLatin1String("getSessionRules"),
            hasHostDnr, hasFeedback, ids));
    }
    if (method == QLatin1String("getEnabledRulesets")) {
        if (!granted)
            return errReply(QStringLiteral("no-permission"));
        QJsonArray sets;
        for (const QString &id : dnr->enabledRulesets(
                 m_extensionId, m_extensionPath, manifest, dir,
                 hasHostDnr, hasFeedback))
            sets.append(id);
        return okReply(sets);
    }
    if (method == QLatin1String("updateEnabledRulesets")) {
        if (!granted)
            return errReply(QStringLiteral("no-permission"));
        const QJsonObject options =
            args.value(QStringLiteral("options")).toObject();
        QStringList enable, disable;
        for (const QJsonValue &v : options.value(
                 QStringLiteral("enableRulesetIds")).toArray())
            enable.append(v.toString());
        for (const QJsonValue &v : options.value(
                 QStringLiteral("disableRulesetIds")).toArray())
            disable.append(v.toString());
        const QJsonObject reply = dnr->updateEnabledRulesets(
            m_extensionId, m_extensionPath, manifest, dir,
            hasHostDnr, hasFeedback, enable, disable);
        if (reply.value(QStringLiteral("ok")).toBool())
            return okReply();
        return errReply(
            reply.value(QStringLiteral("error")).toString());
    }
    if (method == QLatin1String("getAvailableStaticRuleCount")) {
        if (!granted)
            return errReply(QStringLiteral("no-permission"));
        return okReply(dnr->availableStaticRuleCount(
            m_extensionId, m_extensionPath, manifest, dir,
            hasHostDnr, hasFeedback));
    }
    if (method == QLatin1String("isRegexSupported")) {
        if (!granted)
            return errReply(QStringLiteral("no-permission"));
        const QJsonObject query =
            args.value(QStringLiteral("query")).toObject();
        QString reason;
        const bool supported = DeclarativeNetRequest::regexSupported(
            query.value(QStringLiteral("regex")).toString(),
            query.value(QStringLiteral("isCaseSensitive")).toBool(true),
            query.value(QStringLiteral("requireCapturing")).toBool(false),
            &reason);
        QJsonObject result{{QStringLiteral("isSupported"), supported}};
        if (!supported)
            result.insert(QStringLiteral("reason"), reason);
        return okReply(result);
    }
    if (method == QLatin1String("testMatchOutcome")) {
        if (!granted && !hasFeedback)
            return errReply(QStringLiteral("no-permission"));
        const QJsonObject request =
            args.value(QStringLiteral("request")).toObject();
        const QString type =
            request.value(QStringLiteral("type")).toString();
        static const QSet<QString> knownTypes = {
            QStringLiteral("main_frame"), QStringLiteral("sub_frame"),
            QStringLiteral("stylesheet"), QStringLiteral("script"),
            QStringLiteral("image"), QStringLiteral("font"),
            QStringLiteral("object"), QStringLiteral("xmlhttprequest"),
            QStringLiteral("ping"), QStringLiteral("csp_report"),
            QStringLiteral("media"), QStringLiteral("websocket"),
            QStringLiteral("webtransport"), QStringLiteral("webbundle"),
            QStringLiteral("webidentity"), QStringLiteral("other")};
        if (request.value(QStringLiteral("url")).toString().isEmpty()
                || !knownTypes.contains(type))
            return errReply(QStringLiteral("invalid-argument"));
        return okReply(QJsonObject{
            {QStringLiteral("matchedRules"),
             dnr->testMatchOutcome(m_extensionId, m_extensionPath,
                                   manifest, dir, hasHostDnr,
                                   hasFeedback, request)}});
    }
    if (method == QLatin1String("getMatchedRules")) {
        if (!hasFeedback)
            return errReply(QStringLiteral("no-permission"));
        if (!dnr->matchedRulesQuota(m_extensionId))
            return errReply(QStringLiteral("quota-exceeded"));
        const QJsonObject filter =
            args.value(QStringLiteral("filter")).toObject();
        return okReply(QJsonObject{
            {QStringLiteral("rulesMatchedInfo"),
             dnr->matchedRules(
                 m_extensionId,
                 filter.contains(QStringLiteral("tabId"))
                     ? filter.value(QStringLiteral("tabId")).toInt(-1)
                     : -1,
                 filter.value(QStringLiteral("minTimeStamp"))
                     .toDouble(0))}});
    }
    if (method == QLatin1String("setExtensionActionOptions")) {
        if (!granted)
            return errReply(QStringLiteral("no-permission"));
        // Badge/icon action-options need the action-surface plumbing
        // this layer does not own — typed unsupported, not a stub.
        return errReply(QStringLiteral("unsupported"));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.sessions — MD25.  A minimal surface over TabWidget's TABS05
// closed-tab undo stack.  getDevices answers the honest empty list —
// Arktos has no sync service, so no other device can report sessions.
// getRecentlyClosed enumerates the same records the "Recently Closed
// Tabs" menu drives, merged across every browsing window newest-first
// (each stack is per-window, Chrome's list is global) and capped at
// MAX_SESSION_RESULTS — 25, the same bound the stack already
// enforces.  restore() resolves the serial sessionId a Session's tab
// carries back through restoreRecentlyClosed — list positions shift
// under a caller holding an id, so the stack never addresses by
// index.  'sessions' gates the whole namespace like Chrome; onChanged
// is tap-driven (the hub wires recentlyClosedTabsChanged), so UI and
// API mutations fan out identically and no member-emit path exists
// here.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeSessions(
        const QString &method, const QJsonObject &args)
{
    // ER16 shape: Chrome exposes the namespace only under the
    // declared 'sessions' grant — the bridge object answers a direct
    // call from any code in the context, so the gate runs here too.
    if (!manifestPermissions().contains(QLatin1String("sessions")))
        return errReply(QStringLiteral("no-permission"));

    // The closed-tab records across every browsing window — each
    // (window, record) pair keeps its owner so restore() and the
    // windowId fill resolve against the stack the record lives on.
    const auto collect = []() {
        QList<QPair<BrowserMainWindow *, TabWidget::ClosedTabRecord>>
            out;
        for (BrowserMainWindow *window
             : ExtensionEventHub::browsingWindows()) {
            TabWidget *tw = window ? window->tabWidget() : nullptr;
            if (!tw)
                continue;
            const QList<TabWidget::ClosedTabRecord> records =
                tw->recentlyClosedRecords();
            for (const TabWidget::ClosedTabRecord &record : records)
                out.append({window, record});
        }
        // Chrome's list is newest-first globally — merge the
        // per-window stacks on the close timestamp.
        std::sort(out.begin(), out.end(),
                  [](const auto &x, const auto &y) {
            return x.second.lastModified > y.second.lastModified;
        });
        return out;
    };
    // The chrome.sessions Session shape — a closed tab's Session is
    // {lastModified, tab}; window sessions have no Arktos record (a
    // closed window leaves no undo entry).
    const auto sessionJson = [](BrowserMainWindow *window,
                                const TabWidget::ClosedTabRecord &r) {
        QJsonObject tab;
        tab.insert(QStringLiteral("index"), r.index);
        tab.insert(QStringLiteral("windowId"), windowIdFor(window));
        tab.insert(QStringLiteral("pinned"), r.pinned);
        tab.insert(QStringLiteral("active"), false);
        // The strip has no multi-select — highlighted/selected
        // collapse onto active like tabObject() answers.
        tab.insert(QStringLiteral("highlighted"), false);
        tab.insert(QStringLiteral("selected"), false);
        // OTR pages never enter the undo stack (SEC07).
        tab.insert(QStringLiteral("incognito"), false);
        tab.insert(QStringLiteral("url"), r.url.toString());
        tab.insert(QStringLiteral("title"), r.title);
        // sessions.restore() addresses this serial — a stale id
        // (restored, evicted, cleared) simply misses.
        tab.insert(QStringLiteral("sessionId"),
                   QString::number(r.serial));
        QJsonObject session;
        session.insert(QStringLiteral("lastModified"),
                       double(r.lastModified));
        session.insert(QStringLiteral("tab"), tab);
        return session;
    };

    if (method == QLatin1String("getDevices")) {
        if (!m_sessionsDevicesNoted) {
            m_sessionsDevicesNoted = true;
            recordNote(QStringLiteral(
                "sessions: getDevices answers the honest empty list "
                "— Arktos has no sync service, so no other device "
                "can report sessions"));
        }
        return okReply(QJsonArray());
    }

    if (method == QLatin1String("getRecentlyClosed")) {
        // Chrome's Filter.maxResults bound — values outside
        // [0, MAX_SESSION_RESULTS] are argument errors, not clamps.
        int maxResults = 25;
        const QJsonObject filter =
            args.value(QLatin1String("filter")).toObject();
        if (filter.contains(QLatin1String("maxResults"))) {
            const QJsonValue v =
                filter.value(QLatin1String("maxResults"));
            if (!v.isDouble() || v.toDouble() < 0
                    || v.toDouble() > 25)
                return errReply(QStringLiteral("invalid-argument"));
            maxResults = v.toInt();
        }
        QJsonArray out;
        const QList<QPair<BrowserMainWindow *,
                          TabWidget::ClosedTabRecord>> all = collect();
        for (const auto &entry : all) {
            if (out.size() >= maxResults)
                break;
            out.append(sessionJson(entry.first, entry.second));
        }
        return okReply(out);
    }

    if (method == QLatin1String("restore")) {
        const QString sessionId =
            args.value(QLatin1String("sessionId")).toString();
        const QList<QPair<BrowserMainWindow *,
                          TabWidget::ClosedTabRecord>> all = collect();
        if (all.isEmpty())
            return errReply(QStringLiteral("not-found"));
        if (sessionId.isEmpty()) {
            // Chrome's no-arg form restores the newest session.
            const auto &top = all.first();
            TabWidget *tw = top.first->tabWidget();
            WebView *view =
                tw ? tw->restoreRecentlyClosed(top.second.serial)
                   : nullptr;
            if (!view)
                return errReply(QStringLiteral("restore-failed"));
            QJsonObject session = sessionJson(top.first, top.second);
            // The rebuilt tab's live id — Chrome's restored Session
            // answers the tab that now exists.
            QJsonObject tab =
                session.value(QLatin1String("tab")).toObject();
            tab.insert(QStringLiteral("id"), tabIdFor(view));
            session.insert(QStringLiteral("tab"), tab);
            return okReply(session);
        }
        bool parsed = false;
        const quint64 serial = sessionId.toULongLong(&parsed);
        if (!parsed || serial == 0)
            return errReply(QStringLiteral("invalid-argument"));
        for (const auto &entry : all) {
            if (entry.second.serial != serial)
                continue;
            TabWidget *tw = entry.first->tabWidget();
            WebView *view =
                tw ? tw->restoreRecentlyClosed(serial) : nullptr;
            if (!view)
                return errReply(QStringLiteral("restore-failed"));
            QJsonObject session =
                sessionJson(entry.first, entry.second);
            QJsonObject tab =
                session.value(QLatin1String("tab")).toObject();
            tab.insert(QStringLiteral("id"), tabIdFor(view));
            session.insert(QStringLiteral("tab"), tab);
            return okReply(session);
        }
        // An id from before the last restore/clear, or never ours —
        // Chrome's stale-session answer.
        return errReply(QStringLiteral("not-found"));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.extension — ER15.  reloadView is the one bridge op the shim
// cannot answer in-page: a getViews() handle's location.reload() —
// the real capability Chrome's Window reference would carry, routed
// to the context's actual page.  The isAllowed*Access pair are
// capability verdicts: Arktos hosts every extension context on the
// regular profile (there is no incognito extension split) and the
// loader never grants file: scheme access, so both honestly answer
// false today — routed through invoke anyway so the day either grant
// surface exists the answer flips here, not in a page-side stub.
// -------------------------------------------------------------------

// -------------------------------------------------------------------
// chrome.identity — MD35.  getRedirectURL is the one member Chrome
// serves without the 'identity' grant and without any account
// surface: Chromium answers it in a renderer-side custom binding
// (identity_custom_bindings.js) — the path normalized to a leading
// '/', appended to the derived https://<id>.chromiumapp.org origin.
// Ungated like Chrome.  The rest of the published vocabulary is the
// Google-account family (getAccounts/getAuthToken/
// clearAllCachedAuthTokens/getProfileUserInfo/launchWebAuthFlow/
// removeCachedAuthToken) plus onSignInChanged — no Arktos surface
// exists, so they stay typed 'unsupported'.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeIdentity(
        const QString &method, const QJsonObject &args)
{
    if (method == QLatin1String("getRedirectURL")) {
        // The custom binding's semantics: a missing/null path is
        // '/', anything else String()-coerced and normalized to a
        // leading slash before it joins the derived origin.
        const QJsonValue pathArg = args.value(QStringLiteral("path"));
        QString path;
        if (pathArg.isString())
            path = pathArg.toString();
        else if (pathArg.isDouble())
            path = QString::number(pathArg.toDouble());
        else if (pathArg.isBool())
            path = pathArg.toBool() ? QStringLiteral("true")
                                    : QStringLiteral("false");
        if (path.isEmpty())
            path = QStringLiteral("/");
        else if (!path.startsWith(QLatin1Char('/')))
            path.prepend(QLatin1Char('/'));
        return okReply(QStringLiteral("https://") + m_extensionId
                       + QStringLiteral(".chromiumapp.org") + path);
    }
    if (method == QLatin1String("getAccounts")
            || method == QLatin1String("getAuthToken")
            || method == QLatin1String("clearAllCachedAuthTokens")
            || method == QLatin1String("getProfileUserInfo")
            || method == QLatin1String("launchWebAuthFlow")
            || method == QLatin1String("removeCachedAuthToken"))
        return errReply(QStringLiteral("unsupported"));
    return errReply(QStringLiteral("unimplemented"));
}

QJsonObject ExtensionShimBackend::invokeExtension(
        const QString &method, const QJsonObject &args)
{
    if (method == QLatin1String("isAllowedIncognitoAccess")
            || method == QLatin1String("isAllowedFileSchemeAccess"))
        return okReply(false);
    if (method == QLatin1String("reloadView")) {
        const QString contextId =
            args.value(QStringLiteral("contextId")).toString();
        if (contextId.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        return ExtensionBackgroundRuntime::instance()
            ->reloadContextPage(m_extensionId, contextId);
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.sidebarAction — ER15.  The Firefox-shape sugar over the
// sidePanel bookkeeping: panel/title/icon are recorded honestly the
// same way m_sidePanelOptions is; isOpen answers false (no sidebar
// surface can be open — that is the truth, not a stub); open/close/
// toggle degrade typed because there is nothing to open.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeSidebarAction(
        const QString &method, const QJsonObject &args)
{
    // ER16: Firefox installs browser.sidebarAction only when the
    // manifest declares sidebar_action — the shim mirrors that at
    // install time; this is the call-level boundary behind it.
    if (!manifestJson().contains(QStringLiteral("sidebar_action")))
        return errReply(QStringLiteral("no-permission"));
    if (method == QLatin1String("setPanel")) {
        m_sidebarAction.insert(
            QStringLiteral("panel"),
            args.value(QStringLiteral("panel")).toString());
        return okReply();
    }
    if (method == QLatin1String("getPanel")) {
        QJsonObject out;
        out.insert(QStringLiteral("panel"),
                   m_sidebarAction.value(
                       QStringLiteral("panel")).toString());
        return okReply(out);
    }
    if (method == QLatin1String("setTitle")) {
        m_sidebarAction.insert(
            QStringLiteral("title"),
            args.value(QStringLiteral("title")).toString());
        return okReply();
    }
    if (method == QLatin1String("getTitle")) {
        QJsonObject out;
        out.insert(QStringLiteral("title"),
                   m_sidebarAction.value(
                       QStringLiteral("title")).toString());
        return okReply(out);
    }
    if (method == QLatin1String("setIcon")) {
        // No icon surface exists — accepted for bookkeeping only,
        // the note makes the unrendered state visible.
        m_sidebarAction.insert(
            QStringLiteral("icon"),
            args.value(QStringLiteral("icon")));
        recordNote(QStringLiteral(
            "sidebarAction.setIcon: recorded; Arktos has no sidebar "
            "surface to render it"));
        return okReply();
    }
    if (method == QLatin1String("isOpen"))
        return okReply(false);
    if (method == QLatin1String("open")
            || method == QLatin1String("close")
            || method == QLatin1String("toggle"))
        return errReply(QStringLiteral("unsupported"));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.runtime extras — context bookkeeping.  getViews can't move
// Window objects across the channel; it answers context counts and
// the shim composes the return array in-page.  MD18 serves the
// lifecycle trio: reload rides ExtensionManager's unload+load of
// the calling extension, requestUpdateCheck rides
// checkForUpdates/updateResultFor (the verdict pushes back as
// '__arktos.updateCheck' — the check runs real fetches the invoke
// reply cannot wait out), and setUninstallURL persists the page
// removeExtension() opens once the removal lands.
// -------------------------------------------------------------------

// MD18: one UpdateResult -> Chrome's requestUpdateCheck vocabulary.
// 'throttled' is also Chrome's failure verdict — the honest detail
// rides along in 'error' when the check produced one.
static QJsonObject updateCheckJson(
        const ExtensionManager::UpdateResult &result)
{
    QJsonObject verdict;
    if (!result.availableVersion.isEmpty()) {
        verdict.insert(QStringLiteral("status"),
                       QStringLiteral("update_available"));
        verdict.insert(QStringLiteral("version"),
                       result.availableVersion);
    } else if (result.upToDate || result.noSource
               || result.id.isEmpty()) {
        // An id the check never produced a row for (untracked
        // caller, built-in component) is Chrome's "no update".
        verdict.insert(QStringLiteral("status"),
                       QStringLiteral("no_update"));
    } else {
        verdict.insert(QStringLiteral("status"),
                       QStringLiteral("throttled"));
    }
    if (!result.error.isEmpty())
        verdict.insert(QStringLiteral("error"), result.error);
    return verdict;
}

QJsonObject ExtensionShimBackend::invokeRuntime(const QString &method,
                                                const QJsonObject &args)
{
    if (method == QLatin1String("getViews")) {
        QJsonObject counts;
        counts.insert(QStringLiteral("background"), 1);
        counts.insert(QStringLiteral("popup"), 0);
        counts.insert(QStringLiteral("tab"), 0);
        return okReply(counts);
    }
    if (method == QLatin1String("getManifest"))
        return okReply(manifestJson());
    // EXT17: the shim's merged getContexts — synthetic entries for
    // every registered context of THIS extension (the hosted
    // background, open popups, extension pages in tabs).
    if (method == QLatin1String("getContexts"))
        return okReply(ExtensionBackgroundRuntime::instance()
                           ->contextsJson(m_extensionId));
    if (method == QLatin1String("openOptionsPage")) {
        // options_ui.page (or the legacy options_page) opens in a
        // normal tab — the EXT17 page injection gives it the shim
        // there like any extension document.
        const QJsonObject manifest = manifestJson();
        QString page = manifest.value(QStringLiteral("options_ui"))
            .toObject().value(QStringLiteral("page")).toString();
        if (page.isEmpty())
            page = manifest.value(QStringLiteral("options_page"))
                .toString();
        if (page.isEmpty() || page.startsWith(QLatin1Char('/'))
                || page.contains(QLatin1Char(':'))
                || page.split(QLatin1Char('/')).contains(
                    QStringLiteral("..")))
            return errReply(QStringLiteral("no-options-page"));
        const QJsonObject open = invokeTabs(
            QStringLiteral("create"),
            QJsonObject{{QStringLiteral("props"),
                         QJsonObject{{QStringLiteral("url"),
                                      QStringLiteral(
                                          "chrome-extension://")
                                          + m_extensionId
                                          + QLatin1Char('/') + page},
                                     {QStringLiteral("active"),
                                      true}}}});
        if (!open.value(QStringLiteral("ok")).toBool())
            return open;
        return okReply();
    }
    if (method == QLatin1String("platformInfo")) {
        // chrome.runtime.getPlatformInfo shape — real values from
        // QSysInfo so extensions feature-detecting the platform get
        // an honest answer.
        QJsonObject info;
        const QString product = QSysInfo::productType();
        info.insert(QStringLiteral("os"),
                    product.startsWith(QLatin1String("win"))
                        ? QStringLiteral("win")
                    : product.startsWith(QLatin1String("mac"))
                          || product.startsWith(QLatin1String("osx"))
                        ? QStringLiteral("mac")
                    : product.contains(QLatin1String("android"))
                        ? QStringLiteral("android")
                    : product.contains(QLatin1String("bsd"))
                          || product.startsWith(QLatin1String("ios"))
                        ? QStringLiteral("openbsd")
                    : QStringLiteral("linux"));
        const QString cpu = QSysInfo::currentCpuArchitecture();
        info.insert(QStringLiteral("arch"),
                    cpu.contains(QLatin1String("64"))
                        ? (cpu.startsWith(QLatin1String("arm"))
                               ? QStringLiteral("arm64")
                               : QStringLiteral("x86-64"))
                    : cpu.startsWith(QLatin1String("arm"))
                        ? QStringLiteral("arm")
                        : QStringLiteral("x86-32"));
        info.insert(QStringLiteral("nacl_arch"),
                    info.value(QStringLiteral("arch")).toString());
        return okReply(info);
    }
    if (method == QLatin1String("reload")) {
        // chrome.runtime.reload — the manager's unload+load of the
        // calling extension.  Deferred a turn so this reply can
        // leave before the host dies; Chrome gives reload no
        // callback anyway.
        ExtensionManager *manager = ExtensionManager::instance();
        bool tracked = false;
        bool builtin = false;
        if (manager) {
            const QList<ExtensionManager::ExtensionInfo> records =
                manager->extensions();
            for (const ExtensionManager::ExtensionInfo &info : records) {
                if (info.id != m_extensionId)
                    continue;
                tracked = info.loaded;
                builtin = info.builtin || info.path.isEmpty();
                break;
            }
        }
        if (!tracked)
            return errReply(QStringLiteral("not-found"));
        if (builtin)
            return errReply(QStringLiteral("cannot-modify"));
        QPointer<ExtensionManager> guard(manager);
        const QString id = m_extensionId;
        QTimer::singleShot(0, manager, [guard, id]() {
            if (guard)
                guard->reloadExtension(id);
        });
        return okReply();
    }
    if (method == QLatin1String("requestUpdateCheck")) {
        ExtensionManager *manager = ExtensionManager::instance();
        if (!manager || !ExtensionManager::isSupported())
            return errReply(QStringLiteral("unsupported"));
        if (m_updateCheckPending || manager->updateCheckInProgress()) {
            // Chrome throttles a request while a check is in flight.
            return okReply(QJsonObject{{QStringLiteral("status"),
                                        QStringLiteral("throttled")}});
        }
        m_updateCheckPending = true;
        connect(manager, &ExtensionManager::updateCheckFinished, this,
                [this, manager](
                    const QList<ExtensionManager::UpdateResult> &) {
            if (!m_updateCheckPending)
                return;
            m_updateCheckPending = false;
            const ExtensionManager::UpdateResult result =
                manager->updateResultFor(m_extensionId);
            const QJsonObject verdict = updateCheckJson(result);
            emit pushEvent(QStringLiteral("__arktos.updateCheck"),
                           verdict);
            // Chrome also fires onUpdateAvailable on the caller's
            // contexts when the check finds one.
            if (verdict.value(QStringLiteral("status")).toString()
                    == QLatin1String("update_available")) {
                emit pushEvent(QStringLiteral("runtime.onUpdateAvailable"),
                    QJsonObject{{QStringLiteral("__args"),
                        QJsonArray{QJsonObject{
                            {QStringLiteral("version"),
                             result.availableVersion}}}}});
            }
        });
        manager->checkForUpdates(true);
        return okReply();
    }
    if (method == QLatin1String("setUninstallURL")) {
        ExtensionManager *manager = ExtensionManager::instance();
        if (!manager)
            return errReply(QStringLiteral("unsupported"));
        const QUrl url(args.value(QStringLiteral("url")).toString());
        return manager->setUninstallUrl(m_extensionId, url)
            ? okReply() : errReply(QStringLiteral("invalid-argument"));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// shim.* — diagnostics channel: benign notes vs health-relevant gaps.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeShim(const QString &method,
                                             const QJsonObject &args)
{
    if (method == QLatin1String("note")) {
        const QString message =
            args.value(QStringLiteral("message")).toString().left(2048);
        if (m_notes.size() < 500)
            m_notes.append(message);
        emit noteRecorded(message);
        return okReply();
    }
    if (method == QLatin1String("gap")) {
        const QString api =
            args.value(QStringLiteral("api")).toString().left(512);
        const QString error =
            args.value(QStringLiteral("error")).toString().left(512);
        const QString entry = api + QLatin1String(": ") + error;
        if (!m_gaps.contains(entry) && m_gaps.size() < 500) {
            m_gaps.append(entry);
            emit gapReported(api, error);
        }
        return okReply();
    }
    return errReply(QStringLiteral("unimplemented"));
}
