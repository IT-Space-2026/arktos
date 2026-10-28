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

#include "extensionactionbar.h"

#include "arktosicon.h"
#include "arktosstyle.h"
#include "browserapplication.h"
#include "extensionbackgroundhost.h"
#include "extensionmanager.h"

#include <qaction.h>
#include <qevent.h>
#include <qfile.h>
#include <qframe.h>
#include <qguiapplication.h>
#include <qimage.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qmap.h>
#include <qpixmap.h>
#include <qscreen.h>
#include <qtimer.h>
#include <qtoolbutton.h>
#include <qvariant.h>

#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineview.h>

namespace {

// Manifest-V3 icon picking: "action".default_icon (a single path or a
// size->path map) wins; the top-level "icons" map is the fallback.
// Best fit = the smallest declared size >= target, else the largest
// available.  Returns a path relative to the extension dir, empty
// when the manifest declares nothing usable.
QString manifestIconPath(const QString &extensionPath, int targetSize)
{
    if (extensionPath.isEmpty())
        return QString();
    QFile file(extensionPath + QLatin1String("/manifest.json"));
    if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return QString();
    const QJsonObject root =
        QJsonDocument::fromJson(file.readAll()).object();

    const auto collect = [](const QJsonValue &value,
                            QMap<int, QString> *candidates) {
        if (value.isString()) {
            // Size unknown — treat as mid-size so explicit sizes win.
            candidates->insert(16, value.toString());
        } else if (value.isObject()) {
            const QJsonObject sizes = value.toObject();
            for (auto it = sizes.begin(); it != sizes.end(); ++it)
                candidates->insert(it.key().toInt(), it.value().toString());
        }
    };

    QMap<int, QString> candidates;
    for (const char *key : {"action", "browser_action", "page_action"})
        collect(root.value(QLatin1String(key))
                    .toObject().value(QLatin1String("default_icon")),
                &candidates);
    if (candidates.isEmpty())
        collect(root.value(QLatin1String("icons")), &candidates);

    // Smallest declared size >= target, else the largest available.
    int best = -1;
    int largest = -1;
    for (auto it = candidates.constBegin(); it != candidates.constEnd(); ++it) {
        if (it.value().isEmpty())
            continue;
        if (it.key() > largest)
            largest = it.key();
        if (it.key() >= targetSize && (best < 0 || it.key() < best))
            best = it.key();
    }
    if (best < 0)
        best = largest;
    return best >= 0 ? candidates.value(best) : QString();
}

// Best-fit key in a size map: the smallest declared size >= target,
// else the largest, else the unsized (0) entry — the same pick rule
// manifestIconPath uses.
template <typename T>
int pickIconSize(const QMap<int, T> &sizes, int targetSize)
{
    int best = -1;
    int largest = -1;
    for (auto it = sizes.constBegin(); it != sizes.constEnd(); ++it) {
        if (it.key() <= 0)
            continue;
        if (it.key() > largest)
            largest = it.key();
        if (it.key() >= targetSize && (best < 0 || it.key() < best))
            best = it.key();
    }
    if (best < 0)
        best = largest;
    if (best < 0 && sizes.contains(0))
        best = 0;
    return best;
}

// Extension glyph at toolbar size — a chrome.action.setIcon runtime
// override first (decoded pixels are authoritative, then bundle-
// relative paths), then the manifest's own artwork, else the generic
// add-on glyph through the icon theme (native sets carry a real
// application-x-addon; the bundled art is the fallback).  Cached by
// extension id — icon files change only with an extension update,
// which reloads the extension anyway; recorded overrides bypass the
// cache since they change at runtime.
QIcon extensionIcon(const ExtensionManager::ExtensionInfo &info, int edge)
{
    if (!info.actionIconImages.isEmpty()) {
        const int k = pickIconSize(info.actionIconImages, edge);
        if (k >= 0) {
            const QImage image = info.actionIconImages.value(k);
            if (!image.isNull())
                return QIcon(QPixmap::fromImage(
                    image.scaled(edge, edge, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation)));
        }
    }
    if (!info.actionIconPaths.isEmpty() && !info.path.isEmpty()) {
        const int k = pickIconSize(info.actionIconPaths, edge);
        if (k >= 0) {
            const QPixmap pixmap(info.path + QLatin1Char('/')
                                 + info.actionIconPaths.value(k));
            if (!pixmap.isNull())
                return QIcon(pixmap.scaled(edge, edge,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation));
        }
    }

    static QHash<QString, QIcon> cache;
    const QString key = info.id + QLatin1Char('@') + QString::number(edge);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();

    QIcon icon;
    const QString rel = manifestIconPath(info.path, edge);
    if (!rel.isEmpty()) {
        const QPixmap pixmap(info.path + QLatin1Char('/') + rel);
        if (!pixmap.isNull())
            icon = QIcon(pixmap.scaled(edge, edge,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
    }
    if (icon.isNull())
        // A chrome stand-in, not identity art — tinted house style
        // so it does not render a raw theme glyph beside chrome peers.
        icon = ArktosIcon::getChrome(QLatin1String("application-x-addon"));

    // The cache is a per-session convenience, not a registry — bound
    // it so a pathological load/unload loop cannot grow it forever.
    if (cache.size() > 128)
        cache.clear();
    cache.insert(key, icon);
    return icon;
}

// Top-left of a `size` rect placed at (x, y), pulled back inside
// `bounds` — the child-overlay version of the old screen-geometry
// clamp; the bubble never leaves the host window.
QPoint clampToRect(int x, int y, const QSize &size, const QRect &bounds)
{
    return QPoint(qBound(bounds.left(), x,
                         qMax(bounds.left(),
                              bounds.right() - size.width() + 1)),
                  qBound(bounds.top(), y,
                         qMax(bounds.top(),
                              bounds.bottom() - size.height() + 1)));
}

} // namespace

ExtensionActionBar::ExtensionActionBar(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QHBoxLayout(this))
{
    setObjectName(QLatin1String("extensionActionBar"));
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(ArktosStyle::Spacing::Small);
    m_iconSize = QSize(18, 18);

    // App-wide: the overlaid bubble is dismissed by any press outside
    // it — the popup installs its own filter only for Escape.
    qApp->installEventFilter(this);

    ExtensionManager *extensions = ExtensionManager::instance();
    // changed() rides along with every finished signal and is also
    // the (delayed) enable/disable pulse — connect all five anyway so
    // the strip reflects load/unload the moment Chromium settles.
    connect(extensions, &ExtensionManager::extensionLoaded,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::extensionUnloaded,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::extensionInstalled,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::extensionUninstalled,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::changed,
            this, &ExtensionActionBar::rebuild);

    rebuild();
}

void ExtensionActionBar::setToolbarAction(QAction *action)
{
    m_toolbarAction = action;
    // Late wiring — the strip is inserted empty on every cold start,
    // so the first state to push to the action is the current one.
    if (m_toolbarAction)
        m_toolbarAction->setVisible(actionCount() > 0);
    if (qEnvironmentVariableIsSet("ARKTOS_DEBUG_POPUP")) {
        QTimer::singleShot(8000, this, [this]() {
            for (const ExtensionManager::ExtensionInfo &i
                 : ExtensionManager::instance()->extensions())
                if (i.enabled && !i.actionPopupUrl.isEmpty()) {
                    openPopup(i.id, i.actionPopupUrl, this);
                    return;
                }
        });
        for (int t : {14000, 20000, 27000})
            QTimer::singleShot(t, this, [this, t]() {
                if (m_popup) {
                    m_popup->grab().save(
                        QStringLiteral("/tmp/popup_%1.png").arg(t));
                    qInfo() << "EXTDBG grab" << t << m_popup->size()
                            << m_popup->pos();
                }
            });
    }
}

void ExtensionActionBar::setIconSize(const QSize &size)
{
    if (m_iconSize == size)
        return;
    m_iconSize = size;
    rebuild();
}

QSize ExtensionActionBar::iconSize() const
{
    return m_iconSize;
}

int ExtensionActionBar::actionCount() const
{
    return m_layout->count();
}

void ExtensionActionBar::rebuild()
{
    QList<ExtensionManager::ExtensionInfo> actions;
    if (ExtensionManager::isSupported()) {
        const QList<ExtensionManager::ExtensionInfo> all =
            ExtensionManager::instance()->extensions();
        for (const ExtensionManager::ExtensionInfo &info : all) {
            if (info.loaded && info.enabled
                && !info.actionPopupUrl.isEmpty())
                actions.append(info);
        }
    }

    while (QLayoutItem *item = m_layout->takeAt(0))
        delete item->widget();

    const int edge = qMax(m_iconSize.width(), m_iconSize.height());
    for (const ExtensionManager::ExtensionInfo &info : actions) {
        QToolButton *button = new QToolButton(this);
        button->setObjectName(
            QLatin1String("extensionActionButton_") + info.id);
        button->setProperty("extensionId", info.id);
        button->setAutoRaise(true);
        button->setCursor(Qt::ArrowCursor);
        button->setFocusPolicy(Qt::ClickFocus);
        button->setIconSize(m_iconSize);
        button->setIcon(extensionIcon(info, edge));
        // EXT12: carry the degradation verdict on the button — an
        // extension that cannot run must not wear a bare name that
        // promises a working popup.
        QString toolTip = info.name;
        if (info.health
                == ExtensionManager::ExtensionHealth::NotSupported)
            toolTip = tr("%1 — not supported: this extension needs "
                         "chrome.* APIs Qt WebEngine doesn't provide")
                          .arg(info.name);
        else if (info.health
                 == ExtensionManager::ExtensionHealth::Limited)
            toolTip = info.hosted
                ? tr("%1 — hosted runtime, limited support: needs %2")
                      .arg(info.name,
                           info.missingApis.join(QLatin1String(", ")))
                : tr("%1 — limited support: needs %2")
                      .arg(info.name,
                           info.missingApis.join(
                               QLatin1String(", ")));
        // ER20: the hosted-runtime note — a bare name would not say
        // who serves the extension; the EXT17 shim-gap degrade rides
        // along so the tooltip can't hide a partial runtime either.
        else if (info.hosted)
            toolTip = info.shimGaps.isEmpty()
                ? tr("%1 — hosted runtime").arg(info.name)
                : tr("%1 — hosted runtime (degraded)")
                      .arg(info.name);
        button->setToolTip(toolTip);
        button->setAccessibleName(info.name);
        const QString id = info.id;
        const QUrl popupUrl = info.actionPopupUrl;
        connect(button, &QToolButton::clicked, this,
                [this, id, popupUrl, button]() {
            openPopup(id, popupUrl, button);
        });
        m_layout->addWidget(button);
    }

    // No popup-bearing extensions => no strip and no splitter gap.
    setVisible(!actions.isEmpty());
    // EXT22: the widget flag alone is not enough — a strip hidden at
    // insertWidget time leaves its bar action permanently invisible
    // under Qt's one-way tracking, so the toolbar slot is driven
    // explicitly too.
    if (m_toolbarAction)
        m_toolbarAction->setVisible(!actions.isEmpty());
}

bool ExtensionActionBar::triggerAction(const QString &id,
                                       const QString &popupPath)
{
    if (popupPath.isEmpty())
        return false;
    // Anchored on the action button when the extension has one — a
    // popup that exists only via chrome.action.setPopup has no
    // button, so the bar itself anchors the bubble then.
    QWidget *anchor = findChild<QToolButton *>(
        QLatin1String("extensionActionButton_") + id);
    if (!anchor)
        anchor = this;
    QString path = popupPath;
    while (path.startsWith(QLatin1Char('/')))
        path = path.mid(1);
    openPopup(id, QUrl(QStringLiteral("chrome-extension://") + id
                       + QLatin1Char('/') + path), anchor);
    return true;
}

bool ExtensionActionBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_popup && event->type() == QEvent::KeyPress) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            m_popup->close();
            return true;
        }
    }
    // The bubble is an overlaid child of the browser window on Linux
    // and a frameless top-level on macOS (see openPopup) — never a
    // Qt::Popup — so it gets no built-in click-outside dismissal.
    // Filter app-wide instead: any press outside the bubble or its
    // button closes it, matching Chrome.
    //
    // MAC08: inside/outside is decided on global GEOMETRY, not on the
    // receiver's widget ancestry.  Every spontaneous press is funnelled
    // through the app filter twice — once to the receiving QWindow
    // (QWidgetWindow, never a QWidget-descendant of the bubble) and
    // once to the widget under the cursor.  The old ancestry test read
    // the QWindow pass as "outside" and closed the bubble on the very
    // first click anywhere, inside included — the macOS dead-buttons
    // report (reproducible on Linux through QTest's QWindow injection).
    // A rect test gives the same verdict on both passes, for both
    // popup shapes.
    if (event->type() == QEvent::MouseButtonPress && m_popup
            && m_popup->isVisible()) {
        const QPoint global = static_cast<QMouseEvent *>(
            event)->globalPosition().toPoint();
        const QRect popupRect(m_popup->mapToGlobal(QPoint(0, 0)),
                              m_popup->size());
        bool inside = popupRect.contains(global);
        if (!inside && m_popupAnchor) {
            const QRect anchorRect(
                m_popupAnchor->mapToGlobal(QPoint(0, 0)),
                m_popupAnchor->size());
            inside = anchorRect.contains(global);
        }
        if (!inside)
            m_popup->close();
    }
    return QWidget::eventFilter(watched, event);
}

void ExtensionActionBar::openPopup(const QString &id, const QUrl &popupUrl,
                                   QWidget *anchor)
{
    // Chrome toggles: re-clicking the serving button closes the popup.
    if (m_popup && m_popupExtensionId == id) {
        m_popup->close();
        return;
    }
    delete m_popup;

    // EXT12: an extension measured dead would open its action popup
    // on a spinner that never finishes (the engine serves the
    // chrome-extension:// page; the extension's bootstrap then waits
    // on a background worker that does not exist).  Show the reason
    // instead — the honest state of the row, not a fake bubble.
    ExtensionManager::ExtensionInfo info;
    for (const ExtensionManager::ExtensionInfo &entry
         : ExtensionManager::instance()->extensions()) {
        if (entry.id == id) {
            info = entry;
            break;
        }
    }
    if (info.health == ExtensionManager::ExtensionHealth::NotSupported) {
        QWidget *hostWindow = anchor ? anchor->window() : nullptr;
        if (!hostWindow)
            hostWindow = window();
        QFrame *note = new QFrame(hostWindow);
        note->setAttribute(Qt::WA_DeleteOnClose);
        note->setObjectName(QLatin1String("extensionActionPopup"));
        note->setFrameShape(QFrame::StyledPanel);
        QVBoxLayout *noteLayout = new QVBoxLayout(note);
        noteLayout->setContentsMargins(12, 10, 12, 10);
        QLabel *label = new QLabel(note);
        label->setWordWrap(true);
        label->setMaximumWidth(340);
        label->setTextFormat(Qt::PlainText);
        label->setText(tr("%1 can't run in Arktos: %2.")
                           .arg(info.name.isEmpty() ? info.id
                                                    : info.name,
                                info.healthReason));
        noteLayout->addWidget(label);
        note->adjustSize();
        const QPoint topRight = anchor
                ? anchor->mapTo(hostWindow, anchor->rect().bottomRight())
                : QPoint(hostWindow->width() - note->width() - 8, 40);
        const QRect bounds(QPoint(0, 0), hostWindow->size());
        note->move(clampToRect(topRight.x() - note->width(),
                               topRight.y() + 2, note->size(), bounds));
        note->show();
        note->raise();
        m_popup = note;
        m_popupAnchor = anchor;
        m_popupExtensionId = id;
        return;
    }

    QWebEngineProfile *profile =
        BrowserApplication::instance()->webEngineProfile();
    if (!profile || popupUrl.isEmpty())
        return;

    // Owned by the bar (an open popup dies with its window) and
    // deleted rather than hidden on close — a live WebEngine page is
    // too heavy to keep around after the bubble dismisses, and an
    // orphaned page outlives the profile at teardown.
    //
    // On Linux the bubble is an overlaid child of the browser window,
    // not a top-level window at all.  QWebEngineView never composites
    // into a Qt::Popup window — Chromium's renderer has no usable
    // surface there (on macOS the bubble shows dead, on Linux the
    // chrome-extension:// main frame never even commits).  A frameless
    // top-level renders, but on Wayland top-level geometry requests
    // go through compositor placement — every move/resize landed the
    // bubble somewhere new (the "popup jumps everywhere" report).  A
    // child overlay is positioned in widget coordinates on every
    // platform and composites like any tab page — no surface handshake,
    // no placement negotiation, no jumping.
    QWidget *hostWindow = anchor ? anchor->window() : nullptr;
    if (!hostWindow)
        hostWindow = window();
#ifdef Q_OS_MACOS
    // MAC02: the macOS bubble is a real frameless top-level window of
    // the plain Qt::Window class — the same NSWindow class the main
    // window, DevToolsWindow and PipWindow composite through (the
    // only WebEngine surfaces actually proven to paint on this
    // platform).  Qt::Tool produces an NSPanel, the child overlay is
    // an alien widget chain, and neither class of surface is proven;
    // the overlay was also already measured failing under the old
    // deferred-attach ordering (white, then black once native).
    QFrame *popup = new QFrame(hostWindow,
                               Qt::Window | Qt::FramelessWindowHint);
#else
    QFrame *popup = new QFrame(hostWindow);
#endif
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setObjectName(QLatin1String("extensionActionPopup"));
    popup->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    QWebEngineView *view = new QWebEngineView(popup);
    // An extension-chrome page on the extension's own profile —
    // deliberately not the app's WebPage subclass, so no arktos
    // QWebChannel objects or app page policy leak into extension
    // chrome (SEC24 split).  ExtensionChromePage navigation-locks the
    // bubble to the extension's own origin (SEC26 — the ER20
    // JavascriptEnabled lift must never serve foreign web content).
    QWebEnginePage *page = new ExtensionChromePage(profile, id, view);
    // ER20: extension chrome is exempt from the profile's
    // JavascriptEnabled web-content preference — the popup must run
    // even when the user has scripts off (Chrome parity).
    page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled,
                                   true);
    // MAC02: bind the page to the view BEFORE the navigation starts —
    // every other WebEngine surface in the app attaches eagerly (the
    // tabs, PipWindow, DevToolsWindow, the reader overlay, the hosted
    // background page).  The previous load-viewless-then-setPage
    // ordering was the one variable no working surface shares and is
    // the best explanation for the whole macOS report chain — a
    // surface that exists but never gets committed, shown content
    // paints exactly as observed (white under the alien overlay,
    // black under WA_NativeWindow, blank under the Tool window).
    // The page is parented to the view so lifetime needs no extra
    // handling.
    view->setPage(page);
    // EXT17: wire the chromeBridge channel before the first commit —
    // the DocumentCreation preamble polls for qt.webChannelTransport.
    // The view watch still self-wires at commit as the fallback when
    // the hosted background is not up yet (this returns false then,
    // and for non-hosted native extensions).
    ExtensionBackgroundRuntime::instance()
        ->attachExtensionPage(page, id);
    layout->addWidget(view);
    // The starting bubble covers the usual action-popup floor
    // (Bitwarden's document declares html{min-height:600px} and
    // wants ~380+ CSS px of width): if a post-load refit ever lands
    // late or the platform drops the resize, this size is already
    // usable rather than visibly truncating chrome.
    popup->resize(400, 620);

    // Positioning space differs by platform: the Linux overlay child
    // is moved in hostWindow coordinates and clamped to the host
    // rect; the macOS top-level window is moved in global coordinates
    // and
    // clamped to its screen.  The anchor point is the anchor's
    // bottom-right corner in the popup's coordinate space — the
    // bubble's top-right edge aligns to it.
    const auto popupBounds = [](QWidget *host) -> QRect {
#ifdef Q_OS_MACOS
        QScreen *s = host ? host->screen() : nullptr;
        return s ? s->availableGeometry() : QRect();
#else
        return QRect(QPoint(0, 0), host ? host->size() : QSize());
#endif
    };
    const auto placePopup = [popupBounds](QWidget *pop, QWidget *anc,
                                        QWidget *host) {
        const QRect bounds = popupBounds(host);
#ifdef Q_OS_MACOS
        const QPoint topRight =
            anc ? anc->mapToGlobal(anc->rect().bottomRight())
                : (host ? host->frameGeometry().topRight()
                            + QPoint(-8, 40)
                        : bounds.topRight());
#else
        const QPoint topRight =
            anc ? anc->mapTo(host, anc->rect().bottomRight())
                : QPoint(bounds.right() - 8, 40);
#endif
        pop->move(clampToRect(topRight.x() - pop->width(),
                              topRight.y() + 2, pop->size(), bounds));
    };

    // Chrome resizes the bubble to the document — WebEngine cannot
    // report a preferred size up front, so after each load the doc's
    // scroll size re-fits the popup within the usual bubble bounds.
    // SPA popups (Bitwarden) are still laying out at loadFinished —
    // the first measure catches the spinner and truncates the real UI —
    // so re-measure on a bounded schedule while the app settles.
    // The deferred callbacks must survive neither object: the popup is
    // delete-on-close and rebuild() can replace the anchor button
    // while an eval is still in flight — QPointer all, or the
    // resize/move lands on freed widgets.
    const QPointer<QFrame> popupGuard(popup);
    const QPointer<QWidget> anchorGuard(anchor);
    const QPointer<QWidget> hostGuard(hostWindow);
    const QPointer<QWebEnginePage> pageGuard(page);
    const auto measure = [popupGuard, pageGuard, anchorGuard, hostGuard,
                          popupBounds, placePopup]() {
        if (!pageGuard)
            return;
        // MAC12: the documentElement scroll size alone under-reports —
        // when the root element clips (Bitwarden's popup ships
        // html{overflow:hidden}) Chromium clamps the root's
        // scrollWidth/scrollHeight to the element's own size instead
        // of the clipped content's, so the bubble never grew past
        // viewport+slack and the page's own overflow:hidden sliced
        // the content at the window edge (the real-Mac right-edge
        // truncation report).  body's scrollWidth/scrollHeight still
        // report the true overflow, and its bounding rect catches a
        // margined body.  Take the widest honest reading of all three.
        pageGuard->runJavaScript(
            QStringLiteral("(function(){"
                           "var de=document.documentElement;"
                           "var db=document.body;"
                           "var w=de?de.scrollWidth:0;"
                           "var h=de?de.scrollHeight:0;"
                           "if(db){var r=db.getBoundingClientRect();"
                           "w=Math.max(w,db.scrollWidth,Math.ceil(r.right));"
                           "h=Math.max(h,db.scrollHeight,Math.ceil(r.bottom));"
                           "}"
                           "return [w,h];})()"),
            [popupGuard, anchorGuard, hostGuard,
             popupBounds, placePopup](const QVariant &result) {
                if (!popupGuard || !hostGuard)
                    return;
                const QVariantList size = result.toList();
                if (size.size() != 2)
                    return;
                // Chrome caps the bubble at 800x600 — our overlay may
                // grow to 800 tall when the host window has room, so a
                // footer's pop-out/settings row isn't truncated merely
                // because the document outgrows Chrome's bubble (beyond
                // that it scrolls, as in Chrome).
                const QRect bounds = popupBounds(hostGuard);
                const int maxW = qBound(280, bounds.width() - 24, 800);
                const int maxH = qBound(120, bounds.height() - 24, 800);
                popupGuard->resize(
                    qBound(280, size.at(0).toInt() + 24, maxW),
                    qBound(120, size.at(1).toInt() + 24, maxH));
                placePopup(popupGuard, anchorGuard, hostGuard);
            });
    };
    connect(page, &QWebEnginePage::loadFinished, popup,
            [popupGuard, pageGuard, measure](bool ok) {
        if (!ok || !popupGuard || !pageGuard)
            return;
        measure();
    });
    // Retries ride the popup's own lifetime — timers die with it.
    for (int delay : {600, 2000, 5000})
        QTimer::singleShot(delay, popup, measure);

    popup->installEventFilter(this);

    placePopup(popup, anchor, hostWindow);
    // Show before loading: a view whose RWHV never gained a surface
    // can stall or paint white (macOS shows this worst) — the bubble
    // must be visible when the extension navigation begins.
    popup->show();
    popup->raise();
    page->load(popupUrl);
    view->setFocus();

    m_popup = popup;
    m_popupAnchor = anchor;
    m_popupExtensionId = id;
}
