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

#include "sitepanel.h"

#include "adblockmanager.h"
#include "arorastyle.h"
#include "adblockrequestinterceptor.h"
#include "browserapplication.h"
#include "browsertheme.h"
#include "cookiejar.h"
#include "domainblocklist.h"
#include "engineinterface.h"
#include "fingerprintprotector.h"
#include "popupblocker.h"
#include "privacyrequestinterceptor.h"
#include "scriptcontrolmanager.h"
#include "siteshield.h"
#include "tlsverifier.h"
#include "trancolist.h"
#include "webpermissionmanager.h"
#include "webview.h"

#include <qcheckbox.h>
#include <qcombobox.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qpalette.h>
#include <qpushbutton.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#if defined(ARORA_RUSTCORE)
#include "rustcorebridge.h"
#include <rustcore.h>
#endif

static QLabel *plainLabel(const QString &text, QWidget *parent)
{
    // Host names and permission strings are derived from web content —
    // a QLabel's default AutoText would render markup in them.
    QLabel *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    return label;
}

SitePanel::SitePanel(QWidget *parent)
    : QFrame(parent)
    , m_webView(nullptr)
    , m_blockedBaseline(0)
    , m_refreshing(false)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
    setMinimumWidth(280);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(AroraStyle::Spacing::Medium,
                               AroraStyle::Spacing::Medium,
                               AroraStyle::Spacing::Medium,
                               AroraStyle::Spacing::Medium);
    layout->setSpacing(AroraStyle::Spacing::Medium);

    m_hostLabel = plainLabel(QString(), this);
    m_hostLabel->setObjectName(QLatin1String("sitePanelHost"));
    QFont bold = m_hostLabel->font();
    bold.setBold(true);
    m_hostLabel->setFont(bold);
    layout->addWidget(m_hostLabel);

    m_securityLabel = plainLabel(QString(), this);
    m_securityLabel->setObjectName(QLatin1String("sitePanelSecurity"));
    layout->addWidget(m_securityLabel);

    // SEC22: the second-opinion TLS chip — hidden until rustcore's
    // probe reports on the current host.  Warning gets the palette's
    // warning-role color (BrightText); every other verdict keeps the
    // normal text color.
    m_tlsLabel = plainLabel(QString(), this);
    m_tlsLabel->setObjectName(QLatin1String("sitePanelTls"));
    m_tlsLabel->setVisible(false);
    layout->addWidget(m_tlsLabel);
    connect(TlsVerifier::instance(), &TlsVerifier::statusChanged,
            this, [this](const QString &) { refresh(); });

    // TRUST08: the composite site-trust section — one line per real
    // signal from rustcore's verdict (threat-list hit, lookalike
    // resemblance, Tranco rank, first visit).  The box stays hidden
    // whenever the verdict has nothing informative to say.
    // TRUST11: it opens with a separator and a bold header colored by
    // the same verdict the lock icon carries.
    m_trustBox = new QWidget(this);
    m_trustBox->setObjectName(QLatin1String("siteTrust"));
    m_trustLayout = new QVBoxLayout(m_trustBox);
    m_trustLayout->setContentsMargins(0, 0, 0, 0);
    m_trustLayout->setSpacing(2);
    QFrame *trustSeparator = new QFrame(m_trustBox);
    trustSeparator->setFrameShape(QFrame::HLine);
    trustSeparator->setFrameShadow(QFrame::Sunken);
    m_trustLayout->addWidget(trustSeparator);
    m_trustHeader = plainLabel(tr("Trust"), m_trustBox);
    m_trustHeader->setObjectName(QLatin1String("siteTrustHeader"));
    QFont trustFont = m_trustHeader->font();
    trustFont.setBold(true);
    m_trustHeader->setFont(trustFont);
    m_trustLayout->addWidget(m_trustHeader);
    m_trustBox->setVisible(false);
    layout->addWidget(m_trustBox);

    // TRUST11: a refreshed threat-list or Tranco table changes the
    // verdict for already-seen hosts — the shield clears its level
    // cache on the same signals but slot order across objects is not
    // guaranteed, so the panel clears it too and repaints; an open
    // popup picks the new color up immediately.
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

    QFrame *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    layout->addWidget(line);

    QHBoxLayout *cookieRow = new QHBoxLayout();
    cookieRow->addWidget(plainLabel(tr("Cookies"), this));
    m_cookieRule = new QComboBox(this);
    m_cookieRule->setObjectName(QLatin1String("siteCookieRule"));
    m_cookieRule->addItem(tr("Site default"), -1);
    m_cookieRule->addItem(tr("Always allow"), CookieJar::Allow);
    m_cookieRule->addItem(tr("Always block"), CookieJar::Block);
    m_cookieRule->addItem(tr("Allow for this session"),
                          CookieJar::AllowForSession);
    m_cookieRule->setToolTip(
        tr("Per-site cookie exception — overrides the global policy."));
    connect(m_cookieRule, &QComboBox::activated,
            this, [this](int index) { applyCookieRule(index); });
    cookieRow->addWidget(m_cookieRule, 1);
    layout->addLayout(cookieRow);

    m_cookieCount = plainLabel(QString(), this);
    m_cookieCount->setObjectName(QLatin1String("siteCookieCount"));
    layout->addWidget(m_cookieCount);

    // SHLD02: the merged content-blocking section — the global switch
    // sits above the per-site toggle so the two scopes stay distinct,
    // and the count line reports what the blocker stopped on this
    // page load.
    m_contentBlocking = new QCheckBox(tr("Content blocking"), this);
    m_contentBlocking->setObjectName(QLatin1String("siteContentBlocking"));
    m_contentBlocking->setToolTip(
        tr("Blocks ads and trackers on every site — the master switch "
           "the Ad Block settings also expose."));
    connect(m_contentBlocking, &QCheckBox::toggled,
            this, &SitePanel::toggleGlobalBlocking);
    layout->addWidget(m_contentBlocking);

    m_blockContent = new QCheckBox(
        tr("Block ads and trackers on this site"), this);
    m_blockContent->setObjectName(QLatin1String("siteBlockContent"));
    connect(m_blockContent, &QCheckBox::toggled,
            this, &SitePanel::toggleContentBlocking);
    layout->addWidget(m_blockContent);

    m_blockedCount = plainLabel(QString(), this);
    m_blockedCount->setObjectName(QLatin1String("siteBlockedCount"));
    layout->addWidget(m_blockedCount);

    // JSCTL: per-site JavaScript rule — the explicit grant beats the
    // security tier in both directions (see ScriptControlManager).
    QHBoxLayout *jsRow = new QHBoxLayout();
    jsRow->addWidget(plainLabel(tr("JavaScript"), this));
    m_javaScriptRule = new QComboBox(this);
    m_javaScriptRule->setObjectName(QLatin1String("siteJavaScriptRule"));
    m_javaScriptRule->addItem(tr("Site default"),
                              int(ScriptControlManager::SiteDefault));
    m_javaScriptRule->addItem(tr("Always allow"),
                              int(ScriptControlManager::Allow));
    m_javaScriptRule->addItem(tr("Always block"),
                              int(ScriptControlManager::Block));
    m_javaScriptRule->setToolTip(
        tr("Per-site JavaScript override — an explicit choice beats "
           "the security tier in both directions."));
    connect(m_javaScriptRule, &QComboBox::activated,
            this, [this](int index) { applyJavaScriptRule(index); });
    jsRow->addWidget(m_javaScriptRule, 1);
    layout->addLayout(jsRow);

    m_javaScriptState = plainLabel(QString(), this);
    m_javaScriptState->setObjectName(QLatin1String("siteJavaScriptState"));
    layout->addWidget(m_javaScriptState);

    // POPUP01: per-site pop-up exception — the opener-side allowlist
    // PopupBlocker consults in createWindow.
    m_allowPopups = new QCheckBox(
        tr("Allow pop-ups on this site"), this);
    m_allowPopups->setObjectName(QLatin1String("siteAllowPopups"));
    connect(m_allowPopups, &QCheckBox::toggled,
            this, &SitePanel::togglePopups);
    layout->addWidget(m_allowPopups);

    // SAFE01: the host exception the HTTPS-Only warning interstitial
    // consults — the persistent half of its "always allow" link.
    m_allowHttp = new QCheckBox(
        tr("Always allow insecure HTTP on this site"), this);
    m_allowHttp->setObjectName(QLatin1String("siteAllowHttp"));
    m_allowHttp->setToolTip(
        tr("Skips the HTTPS-Only warning page for this host — its "
           "pages load over plain HTTP without asking."));
    connect(m_allowHttp, &QCheckBox::toggled,
            this, &SitePanel::toggleHttpAllowance);
    layout->addWidget(m_allowHttp);

    // SAFE06: per-site exemption from the injected fingerprint
    // countermeasures — canvas-heavy pages can break under the
    // readout noise.  The exception list is baked into the script
    // source, so toggling re-installs it and reloads the page.
    m_spoofFingerprint = new QCheckBox(
        tr("Spoof fingerprints on this site"), this);
    m_spoofFingerprint->setObjectName(QLatin1String("siteFingerprintSpoof"));
    connect(m_spoofFingerprint, &QCheckBox::toggled,
            this, &SitePanel::toggleFingerprintSpoof);
    layout->addWidget(m_spoofFingerprint);

    QFrame *line2 = new QFrame(this);
    line2->setFrameShape(QFrame::HLine);
    line2->setFrameShadow(QFrame::Plain);
    layout->addWidget(line2);

    layout->addWidget(plainLabel(tr("Permissions"), this));
    m_permissionsBox = new QWidget(this);
    m_permissionsBox->setObjectName(QLatin1String("sitePermissions"));
    m_permissionsLayout = new QVBoxLayout(m_permissionsBox);
    m_permissionsLayout->setContentsMargins(0, 0, 0, 0);
    m_permissionsLayout->setSpacing(AroraStyle::Spacing::Small);
    layout->addWidget(m_permissionsBox);

    QFrame *line3 = new QFrame(this);
    line3->setFrameShape(QFrame::HLine);
    line3->setFrameShadow(QFrame::Plain);
    layout->addWidget(line3);

    QHBoxLayout *buttons = new QHBoxLayout();
    m_adBlockSettings = new QPushButton(tr("Ad Block Settings..."), this);
    m_adBlockSettings->setObjectName(QLatin1String("siteAdBlockSettings"));
    connect(m_adBlockSettings, &QPushButton::clicked,
            this, []() { AdBlockManager::instance()->showDialog(); });
    buttons->addWidget(m_adBlockSettings);

    m_clearData = new QPushButton(tr("Clear site data"), this);
    m_clearData->setObjectName(QLatin1String("siteClearData"));
    m_clearData->setToolTip(
        tr("Deletes this site's cookies and local storage."));
    connect(m_clearData, &QPushButton::clicked,
            this, &SitePanel::clearSiteData);
    buttons->addWidget(m_clearData);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    // TRUST17: keep the panel synced while its popup is closed — the
    // menu's pre-popup sizeHint flush measures the last render before
    // aboutToShow runs, so unsignalled state that shifts while closed
    // (a remembered-permission grant from a page prompt, a whitelist
    // edit in the settings dialog) is exactly the first-click
    // wrong-size popup.
    connect(WebPermissionManager::instance(),
            &WebPermissionManager::changed, this, &SitePanel::refresh);
    connect(AdBlockManager::instance(), &AdBlockManager::rulesChanged,
            this, &SitePanel::refresh);
    refresh();
}

void SitePanel::setWebView(WebView *webView)
{
    if (m_webView == webView)
        return;
    if (m_webView)
        disconnect(m_webView, nullptr, this, nullptr);
    m_webView = webView;
    if (webView) {
        // A new load (or a same-tab navigation to another host) starts
        // a fresh tally: the interceptor counter is cumulative, so the
        // baseline is re-captured rather than the shared table cleared.
        connect(webView, &QWebEngineView::loadStarted,
                this, [this]() { m_blockedBaseline = blockedTotal(); });
        connect(webView, &QWebEngineView::urlChanged,
                this, [this](const QUrl &) {
            m_blockedBaseline = blockedTotal();
            refresh();
        });
        connect(webView, &QWebEngineView::loadFinished,
                this, [this](bool) { refresh(); });
        m_blockedBaseline = blockedTotal();
    }
    refresh();
}

WebView *SitePanel::webView() const
{
    return m_webView;
}

QString SitePanel::host() const
{
    return m_webView ? m_webView->url().host() : QString();
}

// The broker keys permissions by bare scheme://host:port — normalize
// the page url the same way so entries match.
QUrl SitePanel::origin() const
{
    QUrl origin = m_webView ? m_webView->url() : QUrl();
    origin.setUserInfo(QString());
    origin.setPath(QString());
    origin.setQuery(QString());
    origin.setFragment(QString());
    return origin;
}

int SitePanel::blockedTotal() const
{
    const QString site = host();
    if (site.isEmpty())
        return 0;
    return AdBlockRequestInterceptor::blockedRequestCount(site);
}

int SitePanel::blockedSinceLoad() const
{
    return qMax(0, blockedTotal() - m_blockedBaseline);
}

CookieJar *SitePanel::siteCookieJar() const
{
    if (!m_webView || !m_webView->page())
        return nullptr;
    return CookieJar::instance(m_webView->page()->profile());
}

void SitePanel::refresh()
{
    m_refreshing = true;

    const QString site = host();
    const QUrl url = m_webView ? m_webView->url() : QUrl();
    const QString scheme = url.scheme();
    const bool hasSite = !site.isEmpty();

    m_hostLabel->setText(hasSite ? site : tr("This page"));
    if (scheme == QLatin1String("arora-cert-error"))
        m_securityLabel->setText(
            tr("Certificate error — the connection could not be verified"));
    else if (scheme == QLatin1String("arora-http-warning"))
        m_securityLabel->setText(
            tr("Blocked by HTTPS-Only mode — the site is not secure"));
    else if (scheme == QLatin1String("arora-site-block"))
        m_securityLabel->setText(
            tr("Blocked — the domain is on the phishing/malware list"));
    else if (scheme == QLatin1String("https"))
        m_securityLabel->setText(tr("Connection is secure (HTTPS)"));
    else if (scheme == QLatin1String("http"))
        m_securityLabel->setText(tr("Connection is not secure (HTTP)"));
    else
        m_securityLabel->setText(tr("Local content"));

    // SEC22 chip: only https sites are probed.  The verdict is a
    // second opinion — a mismatch surfaces the error class, a network
    // failure just drops the chip back to "not verified", and
    // non-webengine/tor/proxy/private pages simply hide it.
    TlsVerifier::Status tls = TlsVerifier::Status::None;
    quint16 tlsPort = 443;
    if (scheme == QLatin1String("https") && hasSite) {
        tlsPort = quint16(url.port(443));
        tls = TlsVerifier::instance()->statusFor(site, tlsPort);
    }
    switch (tls) {
    case TlsVerifier::Status::Verified: {
        const QString version =
            TlsVerifier::instance()->tlsVersion(site, tlsPort);
        m_tlsLabel->setText(version.isEmpty()
            ? tr("TLS verified by Arora")
            : tr("TLS verified by Arora (%1)")
                  .arg(version == QLatin1String("TLSv1_3")
                           ? QStringLiteral("TLS 1.3")
                       : version == QLatin1String("TLSv1_2")
                           ? QStringLiteral("TLS 1.2") : version));
        m_tlsLabel->setToolTip(
            tr("The certificate chain was re-verified independently of "
               "the web engine."));
        m_tlsLabel->setPalette(palette());
        m_tlsLabel->setVisible(true);
        break;
    }
    case TlsVerifier::Status::Warning: {
        const QString errorClass =
            TlsVerifier::instance()->errorClass(site, tlsPort);
        m_tlsLabel->setText(tr("TLS WARNING: %1")
            .arg(errorClass.isEmpty() ? tr("unknown") : errorClass));
        m_tlsLabel->setToolTip(
            TlsVerifier::instance()->detail(site, tlsPort));
        QPalette warning = palette();
        warning.setColor(QPalette::WindowText,
                         warning.color(QPalette::BrightText));
        m_tlsLabel->setPalette(warning);
        m_tlsLabel->setVisible(true);
        break;
    }
    case TlsVerifier::Status::Pending:
        m_tlsLabel->setText(tr("Verifying TLS…"));
        m_tlsLabel->setToolTip(QString());
        m_tlsLabel->setPalette(palette());
        m_tlsLabel->setVisible(true);
        break;
    case TlsVerifier::Status::Unverified:
        // Honest reporting: the chain was never evaluated — that is
        // not a certificate problem and must not render like one.
        m_tlsLabel->setText(tr("TLS not independently verified"));
        m_tlsLabel->setToolTip(
            TlsVerifier::instance()->detail(site, tlsPort));
        m_tlsLabel->setPalette(palette());
        m_tlsLabel->setVisible(true);
        break;
    case TlsVerifier::Status::Refused:
    case TlsVerifier::Status::None:
        m_tlsLabel->setVisible(false);
        break;
    }

    rebuildTrustRows();

    CookieJar *jar = siteCookieJar();
    m_cookieRule->setEnabled(hasSite && jar);
    if (jar && hasSite) {
        CookieJar::CookieRule rule = CookieJar::Allow;
        const int comboIndex = jar->ruleForHost(site, &rule)
            ? m_cookieRule->findData(static_cast<int>(rule))
            : 0;
        m_cookieRule->setCurrentIndex(comboIndex >= 0 ? comboIndex : 0);
        m_cookieCount->setText(
            tr("%1 cookies stored for this site")
                .arg(jar->cookiesForUrl(url).count()));
    } else {
        m_cookieRule->setCurrentIndex(0);
        m_cookieCount->setText(
            hasSite ? QString() : tr("No site to scope cookies to"));
    }

    AdBlockManager *adblock = AdBlockManager::instance();
    const bool blockingOn = adblock->isEnabled();
    m_contentBlocking->setChecked(blockingOn);
    if (!blockingOn)
        m_blockedCount->setText(tr("Content blocking is disabled"));
    else if (!hasSite)
        m_blockedCount->setText(tr("No web page to count"));
    else
        m_blockedCount->setText(
            tr("%1 requests blocked on this page")
                .arg(blockedSinceLoad()));

    // The per-site row greys out while the master switch is off —
    // whitelisting a site is meaningless when nothing is blocked.
    m_blockContent->setEnabled(hasSite && blockingOn
        && !adblock->siteWhitelistFilter(site).isEmpty());
    m_blockContent->setChecked(
        blockingOn && !adblock->isSiteWhitelisted(site));
    m_blockContent->setToolTip(
        !blockingOn
            ? tr("Content blocking is disabled globally")
            : tr("Unchecked, this site's pages are exempted from "
                 "content blocking (an @@||host^$document rule in "
                 "your custom filters)."));

    m_clearData->setEnabled(hasSite);

    ScriptControlManager *scripts = ScriptControlManager::instance();
    const bool webSite = hasSite
        && (scheme == QLatin1String("http")
            || scheme == QLatin1String("https"));
    m_javaScriptRule->setEnabled(webSite);
    if (webSite) {
        const int comboIndex = m_javaScriptRule->findData(
            int(scripts->ruleForHost(site)));
        m_javaScriptRule->setCurrentIndex(comboIndex >= 0 ? comboIndex : 0);
        m_javaScriptState->setText(
            scripts->isJavaScriptEnabledFor(url)
                ? tr("JavaScript runs on this site")
                : tr("JavaScript is blocked on this site"));
    } else {
        m_javaScriptRule->setCurrentIndex(0);
        m_javaScriptState->setText(
            hasSite ? tr("JavaScript rules apply to web sites")
                    : tr("No site to scope JavaScript to"));
    }

    // POPUP01: the exception is host-keyed like the JavaScript one —
    // only real web sites get the toggle.
    m_allowPopups->setEnabled(webSite);
    m_allowPopups->setChecked(
        webSite && PopupBlocker::instance()->isAllowedHost(site));

    // SAFE01: the HTTPS-Only exception is host-keyed like the others.
    // While the warning page itself is shown the url is the internal
    // interstitial (no host) — the row becomes meaningful once the
    // user has proceeded onto the http: site.
    m_allowHttp->setEnabled(webSite);
    m_allowHttp->setChecked(
        webSite && PrivacyRequestInterceptor::isHttpAllowedHost(site));

    // SAFE06: the row is only meaningful while protection is active
    // on this page's profile — greyed with an explanation otherwise.
    FingerprintProtector *protector = FingerprintProtector::instance();
    const bool protectionOn = webSite
        && m_webView && m_webView->page()
        && protector->isActiveForProfile(m_webView->page()->profile());
    m_spoofFingerprint->setEnabled(protectionOn);
    m_spoofFingerprint->setChecked(protectionOn
                                   && !protector->isExempt(site));
    m_spoofFingerprint->setToolTip(
        !webSite
            ? tr("Fingerprint spoofing applies to web sites")
            : !protectionOn
                ? tr("Fingerprint spoofing is off — enable it in "
                     "Settings > Privacy")
                : tr("Unchecked, this host is exempted from the "
                     "injected fingerprint countermeasures (takes "
                     "effect on reload)."));

    rebuildPermissionRows();
    m_refreshing = false;
    emit refreshed();
}

void SitePanel::applyCookieRule(int index)
{
    if (m_refreshing)
        return;
    CookieJar *jar = siteCookieJar();
    const QString site = host();
    if (!jar || site.isEmpty())
        return;
    const int data = m_cookieRule->itemData(index).toInt();
    if (data < 0)
        jar->clearRuleForHost(site);
    else
        jar->setRuleForHost(site, static_cast<CookieJar::CookieRule>(data));
    refresh();
}

void SitePanel::toggleContentBlocking(bool checked)
{
    if (m_refreshing)
        return;
    const QString site = host();
    if (site.isEmpty())
        return;
    AdBlockManager::instance()->setSiteWhitelisted(site, !checked);
    refresh();
}

void SitePanel::toggleGlobalBlocking(bool checked)
{
    if (m_refreshing)
        return;
    AdBlockManager::instance()->setEnabled(checked);
    refresh();
}

void SitePanel::applyJavaScriptRule(int index)
{
    if (m_refreshing)
        return;
    const QString site = host();
    if (site.isEmpty() || !m_webView || !m_webView->page())
        return;
    const int data = m_javaScriptRule->itemData(index).toInt();
    // Off-the-record pages never write the persistent store.
    const bool persistent =
        !m_webView->page()->profile()->isOffTheRecord();
    ScriptControlManager::instance()->setRuleForHost(
        site, static_cast<ScriptControlManager::Rule>(data), persistent);
    // The policy is applied pre-navigation; reload so the change takes
    // effect on the page that is open right now.
    m_webView->reload();
    refresh();
}

void SitePanel::togglePopups(bool checked)
{
    if (m_refreshing)
        return;
    const QString site = host();
    if (site.isEmpty() || !m_webView || !m_webView->page())
        return;
    // Off-the-record pages get a session rule — nothing private is
    // ever written to the persistent store.
    const bool persistent =
        !m_webView->page()->profile()->isOffTheRecord();
    PopupBlocker *blocker = PopupBlocker::instance();
    if (checked)
        blocker->allowHost(site, persistent);
    else
        blocker->removeAllowedHost(site);
    refresh();
}

void SitePanel::toggleHttpAllowance(bool checked)
{
    if (m_refreshing)
        return;
    const QString site = host();
    if (site.isEmpty() || !m_webView || !m_webView->page())
        return;
    // Off-the-record pages never write the persistent store — the
    // exception lives only for the session, like the interstitial's
    // session-scoped "proceed".
    const bool persistent =
        !m_webView->page()->profile()->isOffTheRecord();
    if (checked)
        PrivacyRequestInterceptor::allowHttpForHost(site, persistent);
    else
        PrivacyRequestInterceptor::clearHttpAllowance(site);
    refresh();
}

void SitePanel::toggleFingerprintSpoof(bool checked)
{
    if (m_refreshing)
        return;
    const QString site = host();
    if (site.isEmpty() || !m_webView || !m_webView->page())
        return;
    // Off-the-record pages never write the persistent store — the
    // exemption lives only for the session.
    const bool persistent =
        !m_webView->page()->profile()->isOffTheRecord();
    FingerprintProtector *protector = FingerprintProtector::instance();
    if (checked)
        protector->clearException(site);
    else
        protector->addException(site, persistent);
    // The exemption list is baked into the injected script — re-push
    // it onto the live profiles, then reload so this page's document
    // runs under the new source.
    protector->reinstallOnProfiles();
    m_webView->reload();
    refresh();
}

void SitePanel::rebuildPermissionRows()
{
    // Rows are nested layouts — a layout item's widget() is null, so
    // deleting the item alone orphan-parents the row's widgets to the
    // box.  Strays stay children of the panel and paint over the popup
    // on first show (TRUST17).  Drain each row's widgets first.
    while (QLayoutItem *item = m_permissionsLayout->takeAt(0)) {
        if (QLayout *row = item->layout()) {
            while (QLayoutItem *child = row->takeAt(0)) {
                delete child->widget();
                delete child;
            }
        }
        delete item->widget();
        delete item;
    }

    const QUrl siteOrigin = origin();
    const QList<WebPermissionManager::Entry> entries =
        WebPermissionManager::instance()->entries();
    int shown = 0;
    for (const WebPermissionManager::Entry &entry : entries) {
        if (entry.origin != siteOrigin)
            continue;
        QHBoxLayout *row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(plainLabel(
            WebPermissionManager::typeName(entry.type)
            + QLatin1String(" — ")
            + (entry.granted ? tr("allowed") : tr("denied")),
            m_permissionsBox), 1);
        QPushButton *revoke = new QPushButton(tr("Revoke"), m_permissionsBox);
        connect(revoke, &QPushButton::clicked, this,
                [this, entry]() {
            WebPermissionManager::instance()->removeEntry(entry.origin,
                                                        entry.type);
            refresh();
        });
        row->addWidget(revoke);
        m_permissionsLayout->addLayout(row);
        ++shown;
    }
    if (!shown) {
        m_permissionsLayout->addWidget(plainLabel(
            tr("No remembered permissions for this site"),
            m_permissionsBox));
    }
    // TRUST17: swapped rows don't reach the parent layout's per-item
    // sizeHint cache on their own — a popup measuring the panel mid-
    // rebuild would read the previous render's size (the first-click
    // squashed popup).  Mark the box so the next query re-measures.
    m_permissionsBox->updateGeometry();
}

#if defined(ARORA_RUSTCORE)
// The shared FFI fetch behind the Trust section and the shield
// badge: the composite verdict document for a host, or an empty
// object when the lookup cannot evaluate (empty host, FFI hiccup).
// The whole lookup is local — the blocklist, Tranco table, lookalike
// skeletons and the history rollup all live inside the crate.
static QJsonObject siteTrustVerdict(const QString &host, bool isHttps)
{
    // TRUST03: the master switch kills the whole surface — off means
    // no FFI call at all, so the badge and the Trust section simply
    // see an un-evaluable verdict.  The per-signal switches ride the
    // request as a probe mask: lookalike/threatlist each gate their
    // own input inside the crate.  All three keys are read live, so
    // a Preferences flip applies on the next evaluation.
    if (host.isEmpty() || !BrowserApplication::siteTrustEnabled())
        return QJsonObject();
    unsigned int probes = 0;
    if (BrowserApplication::siteTrustThreatlistEnabled())
        probes |= RC_SITETRUST_PROBE_THREATLIST;
    if (BrowserApplication::siteTrustLookalikeEnabled())
        probes |= RC_SITETRUST_PROBE_LOOKALIKE;
    rustCoreEnsureDataDir();
    const QByteArray hostUtf8 = host.toUtf8();
    char *json = rc_sitetrust_lookup_opts(hostUtf8.constData(),
                                          isHttps ? 1 : 0, probes);
    if (!json)
        return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json));
    rc_string_free(json);
    return doc.object();
}
#endif

// TRUST08: asks rustcore for the composite verdict and maps every
// signal that fired (or is informative) onto a display line — the
// absence of a signal is never rendered, so no neutral noise like
// "not on a threat list" shows up.
QList<SitePanel::TrustLine> SitePanel::trustSignals(const QString &host,
                                                  bool isHttps)
{
    QList<TrustLine> lines;
#if defined(ARORA_RUSTCORE)
    const QJsonObject verdict = siteTrustVerdict(host, isHttps);
    if (verdict.isEmpty())
        return lines;

    const QJsonArray firedSignals =
        verdict.value(QLatin1String("signals")).toArray();
    const auto fired = [&firedSignals](QLatin1String name) {
        for (const QJsonValue &value : firedSignals) {
            if (value.toString() == name)
                return true;
        }
        return false;
    };

    if (fired(QLatin1String("threat")))
        lines.append({ tr("Listed on the phishing/malware threat list"),
                       true });
    if (fired(QLatin1String("lookalike"))) {
        const QString twin =
            verdict.value(QLatin1String("lookalike_of")).toString();
        lines.append({ twin.isEmpty()
            ? tr("Resembles a popular site — check the address")
            : tr("Resembles %1 — check the address").arg(twin),
            true });
    }
    // The rank probe is informative in both directions: listed sites
    // are known, unlisted ones little-known.
    const qint64 rank =
        verdict.value(QLatin1String("rank")).toInteger();
    lines.append({ rank > 0
        ? tr("Known site (Tranco rank %1)").arg(rank)
        : tr("Little-known site"), false });
    if (fired(QLatin1String("first_seen")))
        lines.append({ tr("First visit to this site"), false });
#else
    Q_UNUSED(host);
    Q_UNUSED(isHttps);
#endif
    return lines;
}

QStringList SitePanel::trustSignalLines(const QString &host, bool isHttps)
{
    QStringList out;
    const QList<TrustLine> lines = trustSignals(host, isHttps);
    for (const TrustLine &line : lines)
        out << line.text;
    return out;
}

// TRUST09: the badge only needs the level word — the same verdict
// document, no second FFI signature.
QString SitePanel::trustLevel(const QString &host, bool isHttps)
{
#if defined(ARORA_RUSTCORE)
    return siteTrustVerdict(host, isHttps)
        .value(QLatin1String("level")).toString();
#else
    Q_UNUSED(host);
    Q_UNUSED(isHttps);
    return QString();
#endif
}

// TRUST11: verdict-level tones tuned for text — the badge's icon
// wash would be unreadable on a label, so these run a step darker on
// light themes and a step lighter on dark ones.  Both directions
// keep clear contrast against the theme's own window color.
QColor SitePanel::trustLevelColor(const QString &level, bool darkTheme)
{
    if (level == QLatin1String("green"))
        return darkTheme ? QColor(74, 222, 128) : QColor(21, 128, 61);
    if (level == QLatin1String("amber"))
        return darkTheme ? QColor(251, 191, 36) : QColor(180, 83, 9);
    if (level == QLatin1String("red"))
        return darkTheme ? QColor(248, 113, 113) : QColor(185, 28, 28);
    return QColor();
}

void SitePanel::rebuildTrustRows()
{
    // Rows 0/1 are the separator and the "Trust" header — only the
    // signal lines are rebuilt.
    while (QLayoutItem *item = m_trustLayout->takeAt(2)) {
        delete item->widget();
        delete item;
    }

    const QString site = host();
    const QString scheme = m_webView ? m_webView->url().scheme()
                                     : QString();
    const bool isHttps = scheme == QLatin1String("https");
    const bool webSite = !site.isEmpty()
        && (scheme == QLatin1String("http") || isHttps);
    const QList<TrustLine> lines = webSite
        ? trustSignals(site, isHttps)
        : QList<TrustLine>();

    const QPalette warning = [this]() {
        QPalette p = palette();
        p.setColor(QPalette::WindowText, p.color(QPalette::BrightText));
        return p;
    }();
    for (const TrustLine &line : lines) {
        QLabel *label = plainLabel(line.text, m_trustBox);
        if (line.alert)
            label->setPalette(warning);
        m_trustLayout->addWidget(label);
    }

    // TRUST11: the header wears the same verdict level the lock icon
    // badges — the badge's own cached level, so the two can never
    // disagree.  An un-evaluable verdict keeps the plain text color.
    const QColor levelColor = lines.isEmpty()
        ? QColor()
        : trustLevelColor(
              SiteShieldButton::trustLevelFor(
                  site, scheme == QLatin1String("https")),
              palette().color(QPalette::Window).lightness() < 128);
    if (levelColor.isValid()) {
        QPalette colored = palette();
        colored.setColor(QPalette::WindowText, levelColor);
        m_trustHeader->setPalette(colored);
    } else {
        m_trustHeader->setPalette(palette());
    }

    m_trustBox->setVisible(!lines.isEmpty());
}

void SitePanel::clearSiteData()
{
    const QString site = host();
    if (site.isEmpty() || !m_webView || !m_webView->page())
        return;

    // Cookies scoped to the site (or received by it from a parent
    // domain) come out of the profile's store.
    if (CookieJar *jar = siteCookieJar())
        jar->removeCookiesForHost(site);

    // The DOM-storage half of SEC12 scoped to this origin: the page is
    // the origin, so the sweep empties Chromium's live storage areas.
    // The on-disk leveldb trees are service-coupled — they are not
    // removable under a running browser (see BrowserProfile::
    // clearSiteStorage); the in-memory wipe keeps them empty.  The
    // lifted path reaches pages whose scripts are blocked (JSCTL) —
    // a plain runJavaScript is silently dropped there and the wipe
    // would not happen.
    m_webView->enginePage()->runJavaScriptLifted(QStringLiteral(
        "try{localStorage.clear()}catch(e){}"
        "try{sessionStorage.clear()}catch(e){}"
        "try{if(window.indexedDB&&indexedDB.databases)"
        "indexedDB.databases().then(function(dbs){dbs.forEach("
        "function(db){indexedDB.deleteDatabase(db.name)})})}catch(e){}"
        "try{if(navigator.serviceWorker&&navigator.serviceWorker.getRegistrations)"
        "navigator.serviceWorker.getRegistrations().then(function(rs){"
        "rs.forEach(function(r){r.unregister()})})}catch(e){}"
        "try{if(window.caches&&caches.keys)"
        "caches.keys().then(function(ns){ns.forEach(function(n){caches.delete(n)})})}catch(e){}"));

    refresh();
}
