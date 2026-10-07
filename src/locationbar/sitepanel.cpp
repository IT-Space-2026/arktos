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
#include "cookiejar.h"
#include "webpermissionmanager.h"
#include "webview.h"

#include <qcheckbox.h>
#include <qcombobox.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qpushbutton.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

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
    , m_refreshing(false)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    setMinimumWidth(280);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    m_hostLabel = plainLabel(QString(), this);
    m_hostLabel->setObjectName(QLatin1String("sitePanelHost"));
    QFont bold = m_hostLabel->font();
    bold.setBold(true);
    m_hostLabel->setFont(bold);
    layout->addWidget(m_hostLabel);

    m_securityLabel = plainLabel(QString(), this);
    m_securityLabel->setObjectName(QLatin1String("sitePanelSecurity"));
    layout->addWidget(m_securityLabel);

    QFrame *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
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

    m_blockContent = new QCheckBox(
        tr("Block ads and trackers on this site"), this);
    m_blockContent->setObjectName(QLatin1String("siteBlockContent"));
    connect(m_blockContent, &QCheckBox::toggled,
            this, &SitePanel::toggleContentBlocking);
    layout->addWidget(m_blockContent);

    QFrame *line2 = new QFrame(this);
    line2->setFrameShape(QFrame::HLine);
    line2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line2);

    layout->addWidget(plainLabel(tr("Permissions"), this));
    m_permissionsBox = new QWidget(this);
    m_permissionsBox->setObjectName(QLatin1String("sitePermissions"));
    m_permissionsLayout = new QVBoxLayout(m_permissionsBox);
    m_permissionsLayout->setContentsMargins(0, 0, 0, 0);
    m_permissionsLayout->setSpacing(2);
    layout->addWidget(m_permissionsBox);

    QFrame *line3 = new QFrame(this);
    line3->setFrameShape(QFrame::HLine);
    line3->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line3);

    m_clearData = new QPushButton(tr("Clear site data"), this);
    m_clearData->setObjectName(QLatin1String("siteClearData"));
    m_clearData->setToolTip(
        tr("Deletes this site's cookies and local storage."));
    connect(m_clearData, &QPushButton::clicked,
            this, &SitePanel::clearSiteData);
    layout->addWidget(m_clearData, 0, Qt::AlignLeft);
}

void SitePanel::setWebView(WebView *webView)
{
    if (m_webView == webView)
        return;
    if (m_webView)
        disconnect(m_webView, nullptr, this, nullptr);
    m_webView = webView;
    if (webView) {
        connect(webView, &QWebEngineView::urlChanged,
                this, [this](const QUrl &) { refresh(); });
        connect(webView, &QWebEngineView::loadFinished,
                this, [this](bool) { refresh(); });
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
    else if (scheme == QLatin1String("https"))
        m_securityLabel->setText(tr("Connection is secure (HTTPS)"));
    else if (scheme == QLatin1String("http"))
        m_securityLabel->setText(tr("Connection is not secure (HTTP)"));
    else
        m_securityLabel->setText(tr("Local content"));

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
    m_blockContent->setEnabled(hasSite && blockingOn
        && !adblock->siteWhitelistFilter(site).isEmpty());
    m_blockContent->setChecked(
        blockingOn && !adblock->isSiteWhitelisted(site));
    m_blockContent->setToolTip(
        !blockingOn
            ? tr("Content blocking is disabled in Settings")
            : tr("Unchecked, this site's pages are exempted from "
                 "content blocking (an @@||host^$document rule in "
                 "your custom filters)."));

    m_clearData->setEnabled(hasSite);

    rebuildPermissionRows();
    m_refreshing = false;
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

void SitePanel::rebuildPermissionRows()
{
    while (QLayoutItem *item = m_permissionsLayout->takeAt(0)) {
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
    // clearSiteStorage); the in-memory wipe keeps them empty.
    m_webView->page()->runJavaScript(QStringLiteral(
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
