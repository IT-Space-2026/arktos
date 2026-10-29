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

#include "siteshield.h"

#include "adblockmanager.h"
#include "browserprofile.h"
#include "arktosicon.h"
#include "arktosstyle.h"
#include "browserapplication.h"
#include "browsertheme.h"
#include "domainblocklist.h"
#include "engineinterface.h"
#include "menufit.h"
#include "engineregistry.h"
#include "enginetab.h"
#include "sitebinding_p.h"
#include "sitepanel.h"
#include "trancolist.h"
#include "webview.h"

#include <qevent.h>
#include <qhash.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwidgetaction.h>

// TRUST09: verdicts cached per host+https, shared across every
// button/tab — one local FFI lookup per combination per session.
// The https bit is part of the key so a scheme flip on the same host
// re-evaluates rather than inheriting the other scheme's verdict.
static QHash<QString, QString> s_trustCache;
static int s_trustLookups = 0;

SiteShieldButton::SiteShieldButton(QWidget *parent)
    : QToolButton(parent)
    , m_webView(nullptr)
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    // Slightly larger than the stock 16px chrome icons so the shield
    // reads clearly at a glance; still well inside the bar's row.
    setIconSize(QSize(20, 20));
    setAccessibleName(tr("Site Privacy"));
    setAccessibleDescription(
        tr("Opens the privacy and permissions panel for this site."));
    setToolTip(tr("Site privacy and permissions"));

    // A menu-hosted widget gets popup positioning, click-outside
    // dismissal and Escape handling for free — a hand-rolled Qt::Popup
    // frame would reimplement all three badly.
    m_menu = new QMenu(this);
    m_panel = new SitePanel(m_menu);
    m_panelAction = new QWidgetAction(m_menu);
    m_panelAction->setDefaultWidget(m_panel);
    m_menu->addAction(m_panelAction);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow,
            this, &SiteShieldButton::populateMenu);
    // The panel refreshes itself on web-view signals and its own
    // control writes; a refresh that lands while the popup is open can
    // grow the content (TOR16's rule — a shown QMenu never re-fits a
    // hosted widget on its own).
    connect(m_panel, &SitePanel::refreshed,
            this, &SiteShieldButton::growMenuToFit);
    connect(AdBlockManager::instance(), &AdBlockManager::rulesChanged,
            this, &SiteShieldButton::refreshIcon);
    // ICONS05: the icon is baked to a pixmap while the badges are
    // drawn — re-bake on a light<->dark flip so the themed shield art
    // swaps to its -dark variant.  Queued: the signal fires mid-flip,
    // before palette propagation finishes.
    connect(BrowserTheme::themeNotifier(),
            &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
            this, &SiteShieldButton::refreshIcon, Qt::QueuedConnection);

    // TRUST09: a refreshed blocklist or Tranco table can change the
    // verdict for already-seen hosts — drop the level cache and
    // repaint so the badge follows the new data.
    wireTrustListRefresh(this, [this] { refreshIcon(); });

    refreshIcon();
    // TRUST15: never hide the button.  It rides the navigation bar as
    // a QWidgetAction, whose visibility is latched from the widget's
    // state at insertion — a ctor-hidden widget leaves the item
    // permanently absent no matter how many setVisible(true) calls
    // follow (the action's flag governs, not the widget's).
}

void SiteShieldButton::changeEvent(QEvent *event)
{
    // TRUST18: the glyph tint follows the resolved palette — rebake
    // when it moves.  The tor window's violet stylesheet lands on the
    // toolbar AFTER this button is constructed, and palette() only
    // resolves to the chrome's light text once that polish passes.
    if (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::StyleChange)
        refreshIcon();
    QToolButton::changeEvent(event);
}

void SiteShieldButton::populateMenu()
{
    m_panel->refresh();
    // TRUST17 — same InstantPopup size race as the tor circuit popup
    // (TOR19): the tool button resolves the menu's sizeHint() BEFORE
    // aboutToShow to position the popup, which caches the
    // widget-action rect from the panel's previous render and clears
    // the item layout's dirty flag.  QMenu::popup re-runs that pass
    // only when an item changed since, so a refresh() that resized
    // the panel (a permission row appearing, a longer host or count
    // line) still opened inside the stale rect — the first-click
    // squashed popup.  Mutating the action re-dirties the item layout
    // so the post-aboutToShow pass re-measures the fresh content; the
    // panel side of the fix lives in rebuildPermissionRows, which
    // marks its geometry stale so this re-measure reads a real hint.
    // Cheap: one action-rect pass over a single-item menu.
    m_panelAction->setVisible(false);
    m_panelAction->setVisible(true);
}

// Grow only, never shrink — shrinking a popup under the cursor is the
// "creeping popup" class of bug; spare room is harmless.
void SiteShieldButton::growMenuToFit()
{
    ::growMenuToFit(m_menu, m_panel);
}

void SiteShieldButton::setWebView(WebView *webView)
{
    // The engine-tab binding clears first — an early return while it
    // is still set would keep the wrong tab's connections alive.
    if (m_webView == webView && !m_engineTab)
        return;
    if (m_engineTab) {
        disconnect(m_engineTab, nullptr, this, nullptr);
        m_engineTab = nullptr;
    }
    if (m_webView)
        disconnect(m_webView, nullptr, this, nullptr);
    m_webView = webView;
    m_panel->setWebView(webView);
    if (webView) {
        connect(webView, &QWebEngineView::urlChanged,
                this, [this](const QUrl &) { refreshIcon(); });
        connect(webView, &WebView::javaScriptBlockedChanged,
                this, [this](bool) { refreshIcon(); });
        // Interceptor blocks arrive on the IO thread with no signal —
        // the count badge follows the load's progress/finish beats
        // and the panel recomputes live on popup open.
        connect(webView, &QWebEngineView::loadStarted,
                this, [this]() { refreshIcon(); });
        connect(webView, &QWebEngineView::loadProgress,
                this, [this](int) { refreshIcon(); });
        connect(webView, &QWebEngineView::loadFinished,
                this, [this](bool) { refreshIcon(); });
        // MPROF02: follow the adblock manager this view's persona
        // owns (UniqueConnection across persona revisits).
        QWebEnginePage *page = webView->page();
        connect(AdBlockManager::instanceFor(
                    BrowserProfile::profileIdFor(
                        page ? page->profile() : nullptr)),
                &AdBlockManager::rulesChanged,
                this, &SiteShieldButton::refreshIcon,
                Qt::UniqueConnection);
    }
    refreshIcon();
}

void SiteShieldButton::setEngineTab(EngineTab *tab)
{
    if (m_engineTab == tab && !m_webView)
        return;
    if (m_webView) {
        disconnect(m_webView, nullptr, this, nullptr);
        m_webView = nullptr;
    }
    if (m_engineTab)
        disconnect(m_engineTab, nullptr, this, nullptr);
    m_engineTab = tab;
    // The tab's backend pointer is raw — a stack-owned backend (the
    // autotest fakes) can die while a bound tab still stands, and
    // refresh paths still run during tab-stack teardown.  Keep it as
    // a QPointer so a dead backend reads as absent; the registry
    // membership probe is a plain pointer compare, safe even if the
    // backend is already gone.
    Engine::Backend *backend = tab ? tab->backend() : nullptr;
    m_engineBackend = (backend
        && EngineRegistry::backends().contains(backend))
        ? backend : nullptr;
    m_panel->setEngineTab(tab);
    if (tab) {
        connect(tab, &EngineTab::urlChanged,
                this, [this](const QUrl &) { refreshIcon(); });
        connect(tab, &EngineTab::loadStarted,
                this, [this]() { refreshIcon(); });
        connect(tab, &EngineTab::loadProgress,
                this, [this](int) { refreshIcon(); });
        connect(tab, &EngineTab::loadFinished,
                this, [this](bool) { refreshIcon(); });
        // ENG28: the page object is swapped in place — url() resolves
        // through the tab, so a repaint is all the swap needs.
        connect(tab, &EngineTab::pageRecreated,
                this, [this]() { refreshIcon(); });
    }
    refreshIcon();
}

EngineTab *SiteShieldButton::engineTab() const
{
    return m_engineTab;
}

QString SiteShieldButton::trustLevelFor(const QString &host, bool isHttps)
{
    // TRUST03: the master switch means NO lookup at all — the badge
    // stays the plain lock and the counter proves the FFI was never
    // reached.
    if (host.isEmpty() || !BrowserApplication::siteTrustEnabled())
        return QString();
    // The per-signal switches are part of the key: a Preferences
    // flip produces a different key, so the next visit re-evaluates
    // under the new switches instead of serving the verdict the old
    // ones produced — live toggles, no restart, and stale entries
    // simply age out unreferenced.
    const int probes =
        (BrowserApplication::siteTrustThreatlistEnabled() ? 1 : 0)
        | (BrowserApplication::siteTrustLookalikeEnabled() ? 2 : 0);
    const QString key = host + QLatin1Char('|')
                        + QLatin1Char(isHttps ? '1' : '0')
                        + QLatin1Char('|') + QString::number(probes);
    const auto it = s_trustCache.constFind(key);
    if (it != s_trustCache.constEnd())
        return it.value();
    const QString level = SitePanel::trustLevel(host, isHttps);
    ++s_trustLookups;
    s_trustCache.insert(key, level);
    return level;
}

int SiteShieldButton::trustLookupCount()
{
    return s_trustLookups;
}

void SiteShieldButton::clearTrustCache()
{
    s_trustCache.clear();
}

QIcon SiteShieldButton::tintedBadge(const QIcon &base,
                                    const QSize &iconPx,
                                    const QString &level)
{
    QColor tint;
    if (level == QLatin1String("red"))
        // Near-opaque: a threat-list or lookalike verdict overrides
        // the lock outright.
        tint = QColor(214, 48, 49, 195);
    else if (level == QLatin1String("amber"))
        tint = QColor(240, 173, 0, 140);
    else
        return base;
    QPixmap pixmap = base.pixmap(iconPx);
    QPainter painter(&pixmap);
    // SourceAtop keeps the glyph's alpha — a color wash, not a box.
    painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    painter.fillRect(pixmap.rect(), tint);
    painter.end();
    return QIcon(pixmap);
}

void SiteShieldButton::refreshIcon()
{
    // TRUST18: tint from THIS surface's resolved palette, not the
    // application palette — on the tor window's violet bar the
    // stylesheet lifts ButtonText to the light chrome text color, so
    // the shield stays legible there and on a forced dark theme
    // alike, instead of painting the app's dark glyph.
    const QIcon shield = ArktosIcon::getChrome(
        QLatin1String("security-high"), palette());

    m_trustLevel.clear();
    // TRUST15: unbound (widget tab, or a window before its first
    // bind) shows the shield disabled with the honest reason — the
    // row keeps its shape instead of gaining a gap.  TRUST25: a
    // foreign-engine tab binds too, so only a genuinely tab-less
    // surface lands here now.
    if (!m_webView && !m_engineTab) {
        setIcon(shield);
        setEnabled(false);
        setToolTip(tr("Site privacy — not available on this tab"));
        return;
    }
    setEnabled(true);

    const QUrl url = m_webView ? m_webView->url()
                             : m_engineTab->url();

    // Badge art is authored in 16x16 space; scale it to the button's
    // icon size so the overlay follows the icon growth.
    const int px = iconSize().width();
    const QSize iconPx(px, px);
    QWebEnginePage *page = m_webView ? m_webView->page() : nullptr;
    AdBlockManager *adblock = AdBlockManager::instanceFor(
        BrowserProfile::profileIdFor(page ? page->profile() : nullptr));
    const QString host = url.host();
    // TRUST25: the interceptor only runs on WebEngine profiles — a
    // servo profile advertises no requestInterception and carries no
    // blocking persona of its own, so the global host-keyed
    // whitelist is the only store that exists; but on a foreign tab
    // the whitelist/disabled treatments would imply blocking that
    // never runs there, so they are skipped and the tooltip says so
    // instead of implying a count.
    const bool whitelisted = !m_engineTab
        && adblock->isSiteWhitelisted(host);
    QIcon icon;
    QString tip;
    if (!adblock->isEnabled() && !m_engineTab) {
        icon = QIcon(shield.pixmap(iconPx, QIcon::Disabled));
        tip = tr("Site privacy — content blocking is disabled");
    } else if (whitelisted) {
        icon = QIcon(shield.pixmap(iconPx, QIcon::Disabled));
        tip = tr("Site privacy — content blocking is off for %1")
                  .arg(host);
    } else {
        icon = shield;
        tip = tr("Site privacy and permissions");
    }
    if (m_engineTab) {
        const QString engine = m_engineBackend
            ? m_engineBackend->displayName() : QString();
        tip += engine.isEmpty()
            ? tr(" — content blocking does not run on this engine")
            : tr(" — content blocking does not run on %1").arg(engine);
    }

    // TRUST09: the composite site-trust verdict marks the lock —
    // red on threat/lookalike hits, an amber wash on an "amber"
    // verdict, untouched on green or when nothing could be
    // evaluated.  The level comes out of the per host+https cache
    // (refreshed by the navigation signals wired in setWebView), so
    // this repaint path never blocks on the lookup; non-web schemes
    // carry no badge at all.
    const QString scheme = url.scheme();
    if (scheme == QLatin1String("http")
        || scheme == QLatin1String("https")) {
        m_trustLevel = trustLevelFor(
            host, scheme == QLatin1String("https"));
        icon = tintedBadge(icon, iconPx, m_trustLevel);
        if (m_trustLevel == QLatin1String("red"))
            tip += tr(" — flagged by the phishing/lookalike check");
        else if (m_trustLevel == QLatin1String("amber"))
            tip += tr(" — trust check advises caution");
    }

    // SHLD02: the uBO-style blocked-count badge the removed
    // AdBlockButton used to carry, now on the single shield; the
    // panel owns the load-start baseline the count diffs against.
    // WebEngine-only: the interceptor never runs on a foreign
    // engine, so the badge is simply absent there rather than a
    // fake zero.
    const int blocked = m_webView ? m_panel->blockedSinceLoad() : 0;
    const bool jsBlocked =
        m_webView && m_webView->isJavaScriptBlocked();
    if (blocked > 0 || jsBlocked) {
        QPixmap pixmap = icon.pixmap(iconPx);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal s = qreal(pixmap.width()) / 16.0;
        painter.scale(s, s);
        if (blocked > 0) {
            ArktosStyle::drawCountBadge(&painter, palette(), blocked);
            tip += tr(" — %1 requests blocked").arg(blocked);
        }
        // JSCTL: badge the shield when the current page's scripts are
        // blocked — a per-site rule, the Safer/Safest tier or the
        // global setting — so the restriction is visible without
        // opening the panel.  It slides to the left corner when the
        // count badge occupies the right one.
        if (jsBlocked) {
            const qreal x = blocked > 0 ? 0.5 : 9.5;
            painter.setBrush(ArktosStyle::accentFillColor(palette()));
            painter.setPen(ArktosStyle::onAccentColor(palette()));
            painter.drawEllipse(QRectF(x, 9.5, 6.0, 6.0));
            tip += tr(" — JavaScript blocked");
        }
        painter.end();
        icon = QIcon(pixmap);
    }
    setIcon(icon);
    setToolTip(tip);
}
