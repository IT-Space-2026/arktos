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

#include "sitepadlock.h"

#include "browserapplication.h"
#include "browsertheme.h"
#include "domainblocklist.h"
#include "sitepanel.h"
#include "siteshield.h"
#include "tlsverifier.h"
#include "trancolist.h"
#include "webview.h"

#include <qframe.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qwebengineview.h>
#include <qwidgetaction.h>

static QLabel *padlockLabel(const QString &text, QWidget *parent,
                            const char *name)
{
    // Host names are derived from web content — never let QLabel's
    // AutoText render markup in them.
    QLabel *label = new QLabel(text, parent);
    label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText);
    return label;
}

SitePadlockButton::SitePadlockButton(QWidget *parent)
    : QToolButton(parent)
    , m_webView(nullptr)
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setIconSize(QSize(16, 16));
    setAccessibleName(tr("Connection Security"));
    setAccessibleDescription(
        tr("Shows the connection and site-trust state for this page."));
    setObjectName(QLatin1String("sitePadlock"));

    // The info popup is a menu-hosted widget — popup positioning,
    // click-outside dismissal and Escape come for free (the same
    // pattern the site shield uses).
    m_menu = new QMenu(this);
    m_popup = new QFrame(m_menu);
    m_popup->setObjectName(QLatin1String("padlockPopup"));
    m_popup->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    m_popup->setMinimumWidth(240);
    QVBoxLayout *popupLayout = new QVBoxLayout(m_popup);
    popupLayout->setContentsMargins(10, 8, 10, 8);
    popupLayout->setSpacing(4);
    m_popupHost = padlockLabel(QString(), m_popup, "padlockPopupHost");
    QFont bold = m_popupHost->font();
    bold.setBold(true);
    m_popupHost->setFont(bold);
    popupLayout->addWidget(m_popupHost);
    m_popupSecurity = padlockLabel(QString(), m_popup,
                                   "padlockPopupSecurity");
    popupLayout->addWidget(m_popupSecurity);
    m_popupTls = padlockLabel(QString(), m_popup, "padlockPopupTls");
    popupLayout->addWidget(m_popupTls);
    // One row per real trust signal — the same verdict lines the
    // shield panel's Trust section renders, read-only here.
    m_popupTrustBox = new QWidget(m_popup);
    m_popupTrustBox->setObjectName(QLatin1String("padlockPopupTrust"));
    m_popupTrustLayout = new QVBoxLayout(m_popupTrustBox);
    m_popupTrustLayout->setContentsMargins(0, 2, 0, 0);
    m_popupTrustLayout->setSpacing(2);
    popupLayout->addWidget(m_popupTrustBox);
    QWidgetAction *popupAction = new QWidgetAction(m_menu);
    popupAction->setDefaultWidget(m_popup);
    m_menu->addAction(popupAction);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    connect(m_menu, &QMenu::aboutToShow,
            this, &SitePadlockButton::rebuildPopup);

    // The probe lands asynchronously — a fresh https commit shows
    // Pending (yellow) until statusChanged re-evaluates to the real
    // verdict.  The signal carries the host; refreshing unconditionally
    // is cheaper than comparing (the icon path is a cached lookup).
    connect(TlsVerifier::instance(), &TlsVerifier::statusChanged,
            this, [this](const QString &) { refresh(); });

    // A refreshed threat/Tranco table can change the verdict for the
    // host already on screen — drop the shared level cache (the panel
    // and the toolbar shield clear it on the same signals; a second
    // clear is a no-op) and repaint.
    connect(DomainBlocklist::instance(), &DomainBlocklist::updateFinished,
            this, [this](bool) {
        SiteShieldButton::clearTrustCache();
        refresh();
    });
    connect(TrancoList::instance(), &TrancoList::updateFinished,
            this, [this](bool) {
        SiteShieldButton::clearTrustCache();
        refresh();
    });

    // The tier colors are theme-aware — re-bake on a light<->dark
    // flip.  Queued: the signal fires mid-flip, before the palette
    // propagation finishes.
    connect(BrowserTheme::themeNotifier(),
            &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
            this, &SitePadlockButton::refresh, Qt::QueuedConnection);

    refresh();
    setVisible(m_webView != nullptr);
}

void SitePadlockButton::setWebView(WebView *webView)
{
    if (m_webView == webView)
        return;
    if (m_webView)
        disconnect(m_webView, nullptr, this, nullptr);
    m_webView = webView;
    if (webView) {
        // urlChanged is the navigation commit — the state tracks the
        // page on it; loadStarted/loadFinished re-bracket the in-flight
        // transitions (a same-host navigation can keep url() stable
        // while the verdict behind it is being re-probed).
        connect(webView, &QWebEngineView::urlChanged,
                this, [this](const QUrl &) { refresh(); });
        connect(webView, &QWebEngineView::loadStarted,
                this, [this]() { refresh(); });
        connect(webView, &QWebEngineView::loadFinished,
                this, [this](bool) { refresh(); });
    }
    setVisible(webView != nullptr);
    refresh();
}

SitePadlockButton::State SitePadlockButton::stateFor(const QUrl &url)
{
    const QString scheme = url.scheme();
    const QString host = url.host();
    const bool https = scheme == QLatin1String("https");
    const bool web = (https || scheme == QLatin1String("http"))
        && !host.isEmpty();

    // TRUST03 gate: trust off means NO verdict — but the padlock stays
    // up as a plain chrome lock so the https/http distinction is not
    // lost with the trust surface.
    if (!BrowserApplication::siteTrustEnabled())
        return https && !host.isEmpty() ? State::PlainLocked
                                        : State::PlainUnlocked;
    // Non-web pages (internal schemes, no host) have nothing to
    // evaluate — the honest glyph is the unclaimed open lock.
    if (!web)
        return State::PlainUnlocked;

    // The composite site-trust verdict — cached per host+https, so this
    // repaint path never blocks on the lookup.
    const QString level =
        SiteShieldButton::trustLevelFor(host, https);
    if (level == QLatin1String("red"))
        return https ? State::LockedRed : State::UnlockedRed;

    if (https) {
        const quint16 port = quint16(url.port(443));
        const TlsVerifier::Status tls =
            TlsVerifier::instance()->statusFor(host, port);
        // A chain the second opinion evaluated and rejected is a red
        // verdict regardless of what the lists say.
        if (tls == TlsVerifier::Status::Warning)
            return State::LockedRed;
        // Green needs BOTH signals positive: the site provably known
        // (the verdict's green — Tranco rank or equivalent) AND the
        // chain independently verified.  Pending/Unverified/Refused/
        // None — the latter covering tor and proxied pages, which are
        // never probed — can only ever claim neutral, so a tor page
        // can never wear a misleading green.
        if (tls == TlsVerifier::Status::Verified
            && level == QLatin1String("green"))
            return State::LockedGreen;
        return State::LockedYellow;
    }

    // Plaintext http: never locked, and the verdict itself ambers any
    // host under http — "green" is unreachable today but mapped anyway
    // so the table stays total if the trust stack ever asserts it.
    return level == QLatin1String("green") ? State::UnlockedGreen
                                           : State::UnlockedYellow;
}

QIcon SitePadlockButton::makeIcon(State state) const
{
    const bool locked = state == State::LockedGreen
        || state == State::LockedYellow || state == State::LockedRed
        || state == State::PlainLocked;

    QColor color;
    switch (state) {
    case State::LockedGreen:
    case State::UnlockedGreen:
        color = SitePanel::trustLevelColor(
            QLatin1String("green"),
            palette().color(QPalette::Window).lightness() < 128);
        break;
    case State::LockedYellow:
    case State::UnlockedYellow:
        color = SitePanel::trustLevelColor(
            QLatin1String("amber"),
            palette().color(QPalette::Window).lightness() < 128);
        break;
    case State::LockedRed:
    case State::UnlockedRed:
        color = SitePanel::trustLevelColor(
            QLatin1String("red"),
            palette().color(QPalette::Window).lightness() < 128);
        break;
    case State::PlainLocked:
    case State::PlainUnlocked:
        // The trust surface is off or there is no site — chrome text
        // color, no claim either way.
        color = palette().color(QPalette::ButtonText);
        break;
    }

    // The glyph is painted, not themed — the bundled icon sets carry no
    // padlock, and a painted glyph follows the verdict color directly
    // (same convention as ReaderButton).
    const int px = qMax(16, iconSize().width());
    QPixmap pixmap(px, px);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal s = qreal(px) / 16.0;
    painter.scale(s, s);

    // Shackle: a 5px-radius arc closing over the body for the locked
    // state; for the open state the same arc stops short on the right
    // and swings clear of the body — the classic open-lock read.
    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (locked) {
        painter.drawArc(QRectF(5.5, 3.0, 5.0, 5.0), 0, 180 * 16);
        painter.drawLine(QPointF(5.5, 5.5), QPointF(5.5, 8.5));
        painter.drawLine(QPointF(10.5, 5.5), QPointF(10.5, 8.5));
    } else {
        // Open: the arc swings right and up — its right end no longer
        // reaches the body, leaving a visible gap.
        painter.drawArc(QRectF(7.0, 3.0, 5.0, 5.0), -20 * 16, 200 * 16);
        painter.drawLine(QPointF(7.0, 5.5), QPointF(7.0, 8.5));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(4.0, 8.0, 8.0, 6.0), 1.2, 1.2);
    // Keyhole punched in the field color so the body reads as a lock,
    // not a blob — only meaningful while the shackle is closed.
    if (locked) {
        painter.setBrush(palette().color(QPalette::Base));
        painter.drawEllipse(QPointF(8.0, 10.2), 1.0, 1.0);
        painter.drawRect(QRectF(7.5, 10.2, 1.0, 2.4));
    }
    painter.end();
    return QIcon(pixmap);
}

void SitePadlockButton::refresh()
{
    const QUrl url = m_webView ? m_webView->url() : QUrl();
    m_state = stateFor(url);
    setIcon(makeIcon(m_state));

    QString tip;
    switch (m_state) {
    case State::LockedGreen:
        tip = tr("Connection is secure — verified and trusted site");
        break;
    case State::LockedYellow:
        tip = tr("Connection is secure (HTTPS)");
        break;
    case State::LockedRed:
        tip = tr("Connection is secure but this site is flagged as "
                 "untrusted");
        break;
    case State::UnlockedGreen:
        tip = tr("Trusted site over an insecure connection");
        break;
    case State::UnlockedYellow:
        tip = tr("Connection is not secure (HTTP)");
        break;
    case State::UnlockedRed:
        tip = tr("Not secure — this site is flagged as untrusted");
        break;
    case State::PlainLocked:
        tip = tr("Connection is encrypted (HTTPS) — site trust checks "
                 "are off");
        break;
    case State::PlainUnlocked:
        tip = url.scheme() == QLatin1String("http")
            ? tr("Connection is not secure (HTTP) — site trust checks "
                 "are off")
            : tr("Local or internal page");
        break;
    }
    setToolTip(tip);
    // The popup is label rows only — rebuild eagerly so it is accurate
    // even if opened between verdict changes, and so tests can inspect
    // it without popping the menu.
    rebuildPopup();
}

void SitePadlockButton::rebuildPopup()
{
    const QUrl url = m_webView ? m_webView->url() : QUrl();
    const QString host = url.host();
    const QString scheme = url.scheme();
    const bool https = scheme == QLatin1String("https");
    const bool web = !host.isEmpty()
        && (https || scheme == QLatin1String("http"));

    m_popupHost->setText(host.isEmpty() ? tr("This page") : host);
    if (scheme == QLatin1String("arora-cert-error"))
        m_popupSecurity->setText(
            tr("Certificate error — the connection could not be "
               "verified"));
    else if (scheme == QLatin1String("arora-http-warning"))
        m_popupSecurity->setText(
            tr("Blocked by HTTPS-Only mode — the site is not secure"));
    else if (scheme == QLatin1String("arora-site-block"))
        m_popupSecurity->setText(
            tr("Blocked — the domain is on the phishing/malware list"));
    else if (https)
        m_popupSecurity->setText(tr("Connection is secure (HTTPS)"));
    else if (scheme == QLatin1String("http"))
        m_popupSecurity->setText(tr("Connection is not secure (HTTP)"));
    else
        m_popupSecurity->setText(tr("Local content"));

    // The TLS chip mirrors the site panel's: the second-opinion verdict
    // for the current host, hidden when nothing was ever probed.
    TlsVerifier::Status tls = TlsVerifier::Status::None;
    quint16 tlsPort = 443;
    if (https && !host.isEmpty()) {
        tlsPort = quint16(url.port(443));
        tls = TlsVerifier::instance()->statusFor(host, tlsPort);
    }
    switch (tls) {
    case TlsVerifier::Status::Verified: {
        const QString version =
            TlsVerifier::instance()->tlsVersion(host, tlsPort);
        m_popupTls->setText(version.isEmpty()
            ? tr("TLS verified by Arora")
            : tr("TLS verified by Arora (%1)")
                  .arg(version == QLatin1String("TLSv1_3")
                           ? QStringLiteral("TLS 1.3")
                       : version == QLatin1String("TLSv1_2")
                           ? QStringLiteral("TLS 1.2") : version));
        m_popupTls->setPalette(palette());
        m_popupTls->setVisible(true);
        break;
    }
    case TlsVerifier::Status::Warning: {
        const QString errorClass =
            TlsVerifier::instance()->errorClass(host, tlsPort);
        m_popupTls->setText(tr("TLS WARNING: %1")
            .arg(errorClass.isEmpty() ? tr("unknown") : errorClass));
        m_popupTls->setToolTip(
            TlsVerifier::instance()->detail(host, tlsPort));
        QPalette warning = palette();
        warning.setColor(QPalette::WindowText,
                         warning.color(QPalette::BrightText));
        m_popupTls->setPalette(warning);
        m_popupTls->setVisible(true);
        break;
    }
    case TlsVerifier::Status::Pending:
        m_popupTls->setText(tr("Verifying TLS…"));
        m_popupTls->setToolTip(QString());
        m_popupTls->setPalette(palette());
        m_popupTls->setVisible(true);
        break;
    case TlsVerifier::Status::Unverified:
        // Honest reporting: the chain was never evaluated — that is
        // not a certificate problem and must not render like one.
        m_popupTls->setText(tr("TLS not independently verified"));
        m_popupTls->setToolTip(
            TlsVerifier::instance()->detail(host, tlsPort));
        m_popupTls->setPalette(palette());
        m_popupTls->setVisible(true);
        break;
    case TlsVerifier::Status::Refused:
    case TlsVerifier::Status::None:
        m_popupTls->setVisible(false);
        break;
    }

    // The trust reason lines — the same verdict signals the shield
    // panel's Trust section lists, read-only in this popup.
    while (QLayoutItem *item = m_popupTrustLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QStringList lines = web
        ? SitePanel::trustSignalLines(host, https) : QStringList();
    if (!BrowserApplication::siteTrustEnabled()) {
        m_popupTrustLayout->addWidget(padlockLabel(
            tr("Site trust checks are off — enable them in Settings > "
               "Privacy"),
            m_popupTrustBox, "padlockPopupTrustOff"));
        m_popupTrustBox->setVisible(true);
    } else if (!lines.isEmpty()) {
        for (const QString &line : lines) {
            QLabel *label = padlockLabel(line, m_popupTrustBox,
                                         "padlockPopupTrustLine");
            m_popupTrustLayout->addWidget(label);
        }
        m_popupTrustBox->setVisible(true);
    } else {
        m_popupTrustBox->setVisible(false);
    }
}
