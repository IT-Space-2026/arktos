/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include "settings.h"

#include "acceptlanguagedialog.h"
#include "aroraicon.h"
#include "autofilldialog.h"
#include "autofillmanager.h"
#include "browsermainwindow.h"
#include "browserprofile.h"
#include "cookiedialog.h"
#include "cookieexceptionsdialog.h"
#include "cookiejar.h"
#include "extensionmanager.h"
#include "historymanager.h"
#include "networkaccessmanager.h"
#include "opensearchdialog.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "privacyrequestinterceptor.h"
#include "safetext.h"
#include "scriptcontrolmanager.h"
#include "securestore.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webpermissionmanager.h"
#include "webview.h"

#include <qapplication.h>
#include <qcombobox.h>
#include <qdesktopservices.h>
#include <qdialogbuttonbox.h>
#include <qdir.h>
#include <qfile.h>
#include <qfontdialog.h>
#include <qformlayout.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qlistwidget.h>
#include <qmessagebox.h>
#include <qmetaobject.h>
#include <qsettings.h>
#include <qstandardpaths.h>
#include <qfiledialog.h>
#include <qtreewidget.h>
#include <qboxlayout.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi(this);
    connect(exceptionsButton, &QPushButton::clicked, this, &SettingsDialog::showExceptions);
    connect(setHomeToCurrentPageButton, &QPushButton::clicked, this, &SettingsDialog::setHomeToCurrentPage);

    // ICONS01: one entry per AroraIcon::themeIds() — itemData carries
    // the id, each option previews its own view-refresh glyph.
    for (const QString &id : AroraIcon::themeIds()) {
        const int row = iconThemeCombo->count();
        iconThemeCombo->addItem(AroraIcon::themeDisplayName(id), id);
        iconThemeCombo->setItemIcon(
            row, AroraIcon::iconForTheme(id, QLatin1String("view-refresh")));
    }
    connect(cookiesButton, &QPushButton::clicked, this, &SettingsDialog::showCookies);
    connect(standardFontButton, &QPushButton::clicked, this, &SettingsDialog::chooseFont);
    connect(fixedFontButton, &QPushButton::clicked, this, &SettingsDialog::chooseFixedFont);
    connect(languageButton, &QPushButton::clicked, this, &SettingsDialog::chooseAcceptLanguage);
    connect(downloadDirectoryButton, &QPushButton::clicked, this, &SettingsDialog::chooseDownloadDirectory);
    connect(externalDownloadBrowse, &QPushButton::clicked, this, &SettingsDialog::chooseDownloadProgram);
    connect(styleSheetBrowseButton, &QPushButton::clicked, this, &SettingsDialog::chooseStyleSheet);

    connect(editAutoFillUserButton, &QPushButton::clicked, this, &SettingsDialog::editAutoFillUser);

    connect(extensionLoadButton, &QPushButton::clicked, this, &SettingsDialog::loadExtension);
    connect(extensionInstallButton, &QPushButton::clicked, this, &SettingsDialog::installExtension);
    connect(extensionRemoveButton, &QPushButton::clicked, this, &SettingsDialog::removeExtension);
    connect(extensionsTree, &QTreeWidget::itemSelectionChanged, this, &SettingsDialog::extensionSelectionChanged);
    connect(extensionsTree, &QTreeWidget::itemChanged, this, &SettingsDialog::extensionItemChanged);
    connect(userScriptsOpenButton, &QPushButton::clicked, this, &SettingsDialog::openUserScriptsFolder);
    connect(userScriptsReloadButton, &QPushButton::clicked, this, &SettingsDialog::reloadUserScripts);

    // Extension names come from manifest.json and userscript names
    // from filenames — markup-looking text stays literal.
    extensionsTree->setItemDelegate(new PlainTextItemDelegate(extensionsTree));
    userScriptsList->setItemDelegate(new PlainTextItemDelegate(userScriptsList));

    ExtensionManager *extensions = ExtensionManager::instance();
    connect(extensions, &ExtensionManager::changed, this, &SettingsDialog::refreshExtensions);
    connect(extensions, &ExtensionManager::userScriptsChanged, this, &SettingsDialog::refreshUserScripts);
    connect(extensions, &ExtensionManager::errorOccurred, this, &SettingsDialog::extensionError);
    if (!ExtensionManager::isSupported()) {
        extensionsTree->setEnabled(false);
        extensionLoadButton->setEnabled(false);
        extensionInstallButton->setEnabled(false);
        extensionsHintLabel->setText(
            tr("This build of Qt WebEngine was compiled without extension support."));
    }
    refreshExtensions();
    refreshUserScripts();

    // SEC05: auditable list of remembered site-permission decisions.
    permissionsTree->setItemDelegate(new PlainTextItemDelegate(permissionsTree));
    WebPermissionManager *permissions = WebPermissionManager::instance();
    connect(permissionRemoveButton, &QPushButton::clicked,
            this, &SettingsDialog::removePermission);
    connect(permissionClearAllButton, &QPushButton::clicked,
            this, &SettingsDialog::clearPermissions);
    connect(permissionsTree, &QTreeWidget::itemSelectionChanged,
            this, &SettingsDialog::permissionSelectionChanged);
    connect(permissions, &WebPermissionManager::changed,
            this, &SettingsDialog::refreshPermissions);
    // JSCTL: the per-site JavaScript rules are listed in the same
    // audit table (key prefix "js|" distinguishes them on removal).
    connect(ScriptControlManager::instance(), &ScriptControlManager::changed,
            this, &SettingsDialog::refreshPermissions);
    refreshPermissions();

    // SECLVL: the tier combo's hint text (and the Enable Javascript
    // checkbox's enabled state under Safest) follow the selection.
    connect(securityLevelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateSecurityLevelHint(); });
    updateSecurityLevelHint();

    // DOH01: the DoH endpoint field only matters to the two Custom
    // modes — keep it greyed otherwise.
    connect(secureDnsMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) { secureDnsServer->setEnabled(index >= 2); });

    // SEC13: master-passphrase controls for the credential store.
    connect(credentialPassphraseButton, &QPushButton::clicked,
            this, &SettingsDialog::credentialPassphraseChange);
    connect(credentialRemoveButton, &QPushButton::clicked,
            this, &SettingsDialog::credentialPassphraseRemove);
    connect(credentialLockButton, &QPushButton::clicked,
            this, &SettingsDialog::credentialStoreLock);
    refreshCredentialUi();

    // SRCH02: the Search tab mirrors the shared OpenSearchManager —
    // engine add/remove (the Manage dialog edits the same manager) and
    // external default-engine switches refresh the combo.  The
    // suggestions checkbox rebinds to whichever engine the combo
    // shows; QComboBox::activated marks user picks so a manager change
    // does not clobber an unsaved selection.
    OpenSearchManager *searchManager = ToolbarSearch::openSearchManager();
    connect(manageEnginesButton, &QPushButton::clicked,
            this, &SettingsDialog::manageEngines);
    connect(defaultEngineCombo, &QComboBox::currentTextChanged,
            this, &SettingsDialog::refreshSearchSuggestions);
    connect(defaultEngineCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int) { m_engineComboDirty = true; });
    connect(searchManager, &OpenSearchManager::changed,
            this, &SettingsDialog::refreshSearchEngines);
    connect(searchManager, &OpenSearchManager::currentEngineChanged,
            this, &SettingsDialog::refreshSearchEngines);

    loadDefaults();
    loadFromSettings();
}

void SettingsDialog::loadDefaults()
{
    // The profile's QWebEngineSettings replaces QWebSettings::
    // globalSettings() — pages inherit their settings from the profile.
    QWebEngineSettings *defaultSettings = BrowserProfile::normalProfile()->settings();
    QString standardFontFamily = defaultSettings->fontFamily(QWebEngineSettings::StandardFont);
    int standardFontSize = defaultSettings->fontSize(QWebEngineSettings::DefaultFontSize);
    m_standardFont = QFont(standardFontFamily, standardFontSize);
    standardLabel->setText(QString(QLatin1String("%1 %2")).arg(m_standardFont.family()).arg(m_standardFont.pointSize()));

    QString fixedFontFamily = defaultSettings->fontFamily(QWebEngineSettings::FixedFont);
    int fixedFontSize = defaultSettings->fontSize(QWebEngineSettings::DefaultFixedFontSize);
    m_fixedFont = QFont(fixedFontFamily, fixedFontSize);
    fixedLabel->setText(QString(QLatin1String("%1 %2")).arg(m_fixedFont.family()).arg(m_fixedFont.pointSize()));

    QString downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloadDir.isEmpty())
        downloadDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    downloadsLocation->setText(downloadDir);

    blockPopupWindows->setChecked(!defaultSettings->testAttribute(QWebEngineSettings::JavascriptCanOpenWindows));
    enableJavascript->setChecked(defaultSettings->testAttribute(QWebEngineSettings::JavascriptEnabled));
    enablePlugins->setChecked(defaultSettings->testAttribute(QWebEngineSettings::PluginsEnabled));
    enableImages->setChecked(defaultSettings->testAttribute(QWebEngineSettings::AutoLoadImages));
    enableLocalStorage->setChecked(defaultSettings->testAttribute(QWebEngineSettings::LocalStorageEnabled));
    cookieSessionCombo->setCurrentIndex(0);
    filterTrackingCookiesCheckbox->setChecked(false);

    autoFillPasswordFormsCheckBox->setChecked(false);
    minimFontSizeCheckBox->setChecked(false);
    minimumFontSizeSpinBox->setValue(9);
}

void SettingsDialog::loadFromSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("Settings"));
    tabWidget->setCurrentIndex(settings.value(QLatin1String("currentTab"), 0).toInt());
    settings.endGroup();

    settings.beginGroup(QLatin1String("MainWindow"));
    QString defaultHome = QLatin1String("about:home");
    homeLineEdit->setText(settings.value(QLatin1String("home"), defaultHome).toString());
    startupBehavior->setCurrentIndex(settings.value(QLatin1String("startupBehavior"), 0).toInt());
    // SRCH03: opt-in — hidden by default now that the omnibox
    // location bar covers searching.
    showSearchBox->setChecked(settings.value(QLatin1String("showSearchBox"), false).toBool());
    const QString iconTheme = AroraIcon::theme();
    const int iconThemeIndex = iconThemeCombo->findData(iconTheme);
    iconThemeCombo->setCurrentIndex(iconThemeIndex < 0 ? 0 : iconThemeIndex);
    settings.endGroup();

    settings.beginGroup(QLatin1String("history"));
    int historyExpire = settings.value(QLatin1String("historyLimit")).toInt();
    int idx = 0;
    switch (historyExpire) {
    case 1: idx = 0; break;
    case 7: idx = 1; break;
    case 14: idx = 2; break;
    case 30: idx = 3; break;
    case 365: idx = 4; break;
    case -1: idx = 5; break;
    case -2: idx = 6; break;
    default:
        idx = 5;
    }
    expireHistory->setCurrentIndex(idx);
    settings.endGroup();

    settings.beginGroup(QLatin1String("urlloading"));
    // SRCH01: the omnibox searches by default — the stored value only
    // exists once the user touches the checkbox.
    bool search = settings.value(QLatin1String("searchEngineFallback"), true).toBool();
    searchEngineFallback->setChecked(search);
    settings.endGroup();

    settings.beginGroup(QLatin1String("downloadmanager"));
    bool alwaysPromptForFileName = settings.value(QLatin1String("alwaysPromptForFileName"), false).toBool();
    downloadAsk->setChecked(alwaysPromptForFileName);
    QString downloadDirectory = settings.value(QLatin1String("downloadDirectory"), downloadsLocation->text()).toString();
    downloadsLocation->setText(downloadDirectory);
    externalDownloadButton->setChecked(settings.value(QLatin1String("external"), false).toBool());
    externalDownloadPath->setText(settings.value(QLatin1String("externalPath")).toString());
    settings.endGroup();

    // Appearance
    settings.beginGroup(QLatin1String("websettings"));
    m_fixedFont = settings.value(QLatin1String("fixedFont"), m_fixedFont).value<QFont>();
    m_standardFont = settings.value(QLatin1String("standardFont"), m_standardFont).value<QFont>();

    standardLabel->setText(QString(QLatin1String("%1 %2")).arg(m_standardFont.family()).arg(m_standardFont.pointSize()));
    fixedLabel->setText(QString(QLatin1String("%1 %2")).arg(m_fixedFont.family()).arg(m_fixedFont.pointSize()));

    blockPopupWindows->setChecked(settings.value(QLatin1String("blockPopupWindows"), blockPopupWindows->isChecked()).toBool());
    enableJavascript->setChecked(settings.value(QLatin1String("enableJavascript"), enableJavascript->isChecked()).toBool());
    enablePlugins->setChecked(settings.value(QLatin1String("enablePlugins"), enablePlugins->isChecked()).toBool());
    enableImages->setChecked(settings.value(QLatin1String("enableImages"), enableImages->isChecked()).toBool());
    enableLocalStorage->setChecked(settings.value(QLatin1String("enableLocalStorage"), enableLocalStorage->isChecked()).toBool());
    userStyleSheet->setText(QString::fromUtf8(settings.value(QLatin1String("userStyleSheet")).toUrl().toEncoded()));
    int minimumFontSize = settings.value(QLatin1String("minimumFontSize"), 0).toInt();
    minimFontSizeCheckBox->setChecked(minimumFontSize != 0);
    if (minimumFontSize != 0)
        minimumFontSizeSpinBox->setValue(minimumFontSize);
    settings.endGroup();

    // Privacy
    settings.beginGroup(QLatin1String("cookies"));

    QByteArray value = settings.value(QLatin1String("acceptCookies"), QLatin1String("AcceptOnlyFromSitesNavigatedTo")).toByteArray();
    QMetaEnum acceptPolicyEnum = CookieJar::staticMetaObject.enumerator(CookieJar::staticMetaObject.indexOfEnumerator("AcceptPolicy"));
    CookieJar::AcceptPolicy acceptCookies = acceptPolicyEnum.keyToValue(value) == -1 ?
                        CookieJar::AcceptOnlyFromSitesNavigatedTo :
                        static_cast<CookieJar::AcceptPolicy>(acceptPolicyEnum.keyToValue(value));
    switch (acceptCookies) {
    case CookieJar::AcceptAlways:
        acceptCombo->setCurrentIndex(0);
        break;
    case CookieJar::AcceptNever:
        acceptCombo->setCurrentIndex(1);
        break;
    case CookieJar::AcceptOnlyFromSitesNavigatedTo:
        acceptCombo->setCurrentIndex(2);
        break;
    }

    value = settings.value(QLatin1String("keepCookiesUntil"), QLatin1String("Expire")).toByteArray();
    QMetaEnum keepPolicyEnum = CookieJar::staticMetaObject.enumerator(CookieJar::staticMetaObject.indexOfEnumerator("KeepPolicy"));
    CookieJar::KeepPolicy keepCookies = keepPolicyEnum.keyToValue(value) == -1 ?
                        CookieJar::KeepUntilExpire :
                        static_cast<CookieJar::KeepPolicy>(keepPolicyEnum.keyToValue(value));
    switch (keepCookies) {
    case CookieJar::KeepUntilExpire:
        keepUntilCombo->setCurrentIndex(0);
        break;
    case CookieJar::KeepUntilExit:
        keepUntilCombo->setCurrentIndex(1);
        break;
    case CookieJar::KeepUntilTimeLimit:
        keepUntilCombo->setCurrentIndex(2);
        break;
    }
    int sessionLength = settings.value(QLatin1String("sessionLength"), -1).toInt();
    switch (sessionLength) {
    case 1: cookieSessionCombo->setCurrentIndex(1); break;
    case 2: cookieSessionCombo->setCurrentIndex(2); break;
    case 3: cookieSessionCombo->setCurrentIndex(3); break;
    case 7: cookieSessionCombo->setCurrentIndex(4); break;
    case 30: cookieSessionCombo->setCurrentIndex(5); break;
    default:
    case 0: cookieSessionCombo->setCurrentIndex(0); break;
    }
    filterTrackingCookiesCheckbox->setChecked(settings.value(QLatin1String("filterTrackingCookies"), false).toBool());
    blockThirdPartyCookies->setChecked(settings.value(QLatin1String("blockThirdPartyCookies"), true).toBool());
    settings.endGroup();

    // Connections & Storage hardening (PRIV01).  The interceptor
    // toggles are applied live through applySettings() ->
    // PrivacyRequestInterceptor::loadSettings() below; the WebRTC/DoH
    // Chromium flags and the clear-on-exit hook read these keys too.
    settings.beginGroup(QLatin1String("privacy"));
    httpsFirst->setChecked(settings.value(QLatin1String("httpsFirst"), true).toBool());
    trimReferer->setChecked(settings.value(QLatin1String("trimReferer"), true).toBool());
    webrtcProtection->setChecked(settings.value(QLatin1String("webrtcIpProtection"), true).toBool());
    // DOH01: the PRIV01-era bool folds into "automatic" (mode 1) when
    // the newer mode key was never written.
    const QVariant storedDnsMode =
        settings.value(QLatin1String("secureDnsMode"));
    const int secureDnsIndex = storedDnsMode.isValid()
        ? storedDnsMode.toInt()
        : (settings.value(QLatin1String("secureDns"), false).toBool()
               ? 1 : 0);
    secureDnsMode->setCurrentIndex(
        qBound(0, secureDnsIndex, secureDnsMode->count() - 1));
    secureDnsServer->setText(settings.value(
        QLatin1String("secureDnsServer"),
        QLatin1String("https://cloudflare-dns.com/dns-query")).toString());
    secureDnsServer->setEnabled(secureDnsMode->currentIndex() >= 2);
    clearOnExit->setChecked(settings.value(QLatin1String("clearOnExit"), false).toBool());
    // PRIV02 fingerprint normalization.
    reportUtcTimezone->setChecked(settings.value(QLatin1String("reportUtcTimezone"), false).toBool());
    normalizeAcceptLanguage->setChecked(settings.value(QLatin1String("normalizeAcceptLanguage"), false).toBool());
    securityLevelCombo->setCurrentIndex(qBound(
        int(PrivacyRequestInterceptor::Standard),
        settings.value(QLatin1String("securityLevel"),
                       int(PrivacyRequestInterceptor::Standard)).toInt(),
        int(PrivacyRequestInterceptor::Safest)));
    settings.endGroup();
    updateSecurityLevelHint();

    // The Search tab mirrors OpenSearchManager: engine combo (default
    // engine selection) plus the per-engine suggestions opt-in (SEC11)
    // bound to whichever engine the combo shows.
    refreshSearchEngines();

    // Network — also drives the profile's http cache through
    // BrowserProfile::applySettings().
    settings.beginGroup(QLatin1String("network"));
    networkCache->setChecked(settings.value(QLatin1String("cacheEnabled"), true).toBool());
    networkCacheMaximumSizeSpinBox->setValue(settings.value(QLatin1String("maximumCacheSize"), 50).toInt());
    settings.endGroup();

    // Proxy
    settings.beginGroup(QLatin1String("proxy"));
    proxySupport->setChecked(settings.value(QLatin1String("enabled"), false).toBool());
    proxyType->setCurrentIndex(settings.value(QLatin1String("type"), 0).toInt());
    proxyHostName->setText(settings.value(QLatin1String("hostName")).toString());
    proxyPort->setValue(settings.value(QLatin1String("port"), 1080).toInt());
    proxyUserName->setText(settings.value(QLatin1String("userName")).toString());
    proxyPassword->setText(SecureStore::openString(settings.value(QLatin1String("password")).toString()));
    settings.endGroup();

    // Tabs
    settings.beginGroup(QLatin1String("tabs"));
    const int tabBarPositionIndex = settings.value(QLatin1String("tabBarPosition"), 0).toInt();
    tabBarPosition->setCurrentIndex(qBound(0, tabBarPositionIndex, tabBarPosition->count() - 1));
    selectTabsWhenCreated->setChecked(settings.value(QLatin1String("selectNewTabs"), false).toBool());
    confirmClosingMultipleTabs->setChecked(settings.value(QLatin1String("confirmClosingMultipleTabs"), true).toBool());
    oneCloseButton->setChecked(settings.value(QLatin1String("oneCloseButton"),false).toBool());
    quitAsLastTabClosed->setChecked(settings.value(QLatin1String("quitAsLastTabClosed"), true).toBool());
    openTargetBlankLinksIn->setCurrentIndex(settings.value(QLatin1String("openTargetBlankLinksIn"), TabWidget::NewSelectedTab).toInt());
    openLinksFromAppsIn->setCurrentIndex(settings.value(QLatin1String("openLinksFromAppsIn"), TabWidget::NewSelectedTab).toInt());
    settings.endGroup();

    settings.beginGroup(QLatin1String("autofill"));
    autoFillPasswordFormsCheckBox->setChecked(settings.value(QLatin1String("passwordForms"), true).toBool());
    settings.endGroup();
}

void SettingsDialog::saveToSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("Settings"));
    settings.setValue(QLatin1String("currentTab"), tabWidget->currentIndex());
    settings.endGroup();

    settings.beginGroup(QLatin1String("MainWindow"));
    settings.setValue(QLatin1String("home"), homeLineEdit->text());
    settings.setValue(QLatin1String("startupBehavior"), startupBehavior->currentIndex());
    settings.setValue(QLatin1String("showSearchBox"), showSearchBox->isChecked());
    const QString iconTheme = iconThemeCombo->currentData().toString();
    settings.setValue(QLatin1String("iconTheme"), iconTheme);
    settings.endGroup();
    // Applies to existing icons live — QIcon theme lookups re-resolve
    // on setThemeName, no widget rewiring needed.
    AroraIcon::setTheme(iconTheme);

    settings.beginGroup(QLatin1String("downloadmanager"));
    settings.setValue(QLatin1String("alwaysPromptForFileName"), downloadAsk->isChecked());
    settings.setValue(QLatin1String("downloadDirectory"), downloadsLocation->text());
    settings.setValue(QLatin1String("external"), externalDownloadButton->isChecked());
    settings.setValue(QLatin1String("externalPath"), externalDownloadPath->text());
    settings.endGroup();

    settings.beginGroup(QLatin1String("history"));
    int historyExpire = expireHistory->currentIndex();
    int idx = -1;
    switch (historyExpire) {
    case 0: idx = 1; break;
    case 1: idx = 7; break;
    case 2: idx = 14; break;
    case 3: idx = 30; break;
    case 4: idx = 365; break;
    case 5: idx = -1; break;
    case 6: idx = -2; break;
    }
    settings.setValue(QLatin1String("historyLimit"), idx);
    settings.endGroup();

    settings.beginGroup(QLatin1String("urlloading"));
    settings.setValue(QLatin1String("searchEngineFallback"), searchEngineFallback->isChecked());
    settings.endGroup();

    // Appearance
    settings.beginGroup(QLatin1String("websettings"));
    settings.setValue(QLatin1String("fixedFont"), m_fixedFont);
    settings.setValue(QLatin1String("standardFont"), m_standardFont);

    settings.setValue(QLatin1String("blockPopupWindows"), blockPopupWindows->isChecked());
    settings.setValue(QLatin1String("enableJavascript"), enableJavascript->isChecked());
    settings.setValue(QLatin1String("enablePlugins"), enablePlugins->isChecked());
    settings.setValue(QLatin1String("enableImages"), enableImages->isChecked());
    settings.setValue(QLatin1String("enableLocalStorage"), enableLocalStorage->isChecked());
    QString userStyleSheetString = userStyleSheet->text();
    if (QFile::exists(userStyleSheetString))
        settings.setValue(QLatin1String("userStyleSheet"), QUrl::fromLocalFile(userStyleSheetString));
    else
        settings.setValue(QLatin1String("userStyleSheet"), QUrl::fromEncoded(userStyleSheetString.toUtf8()));

    if (minimFontSizeCheckBox->isChecked())
        settings.setValue(QLatin1String("minimumFontSize"), minimumFontSizeSpinBox->value());
    else
        settings.setValue(QLatin1String("minimumFontSize"), 0);
    settings.endGroup();

    // Privacy
    settings.beginGroup(QLatin1String("cookies"));
    CookieJar::AcceptPolicy acceptCookies;
    switch (acceptCombo->currentIndex()) {
    default:
    case 0:
        acceptCookies = CookieJar::AcceptAlways;
        break;
    case 1:
        acceptCookies = CookieJar::AcceptNever;
        break;
    case 2:
        acceptCookies = CookieJar::AcceptOnlyFromSitesNavigatedTo;
        break;
    }

    QMetaEnum acceptPolicyEnum = CookieJar::staticMetaObject.enumerator(CookieJar::staticMetaObject.indexOfEnumerator("AcceptPolicy"));
    settings.setValue(QLatin1String("acceptCookies"), QLatin1String(acceptPolicyEnum.valueToKey(acceptCookies)));

    CookieJar::KeepPolicy keepPolicy;
    switch (keepUntilCombo->currentIndex()) {
    default:
    case 0:
        keepPolicy = CookieJar::KeepUntilExpire;
        break;
    case 1:
        keepPolicy = CookieJar::KeepUntilExit;
        break;
    case 2:
        keepPolicy = CookieJar::KeepUntilTimeLimit;
        break;
    }

    QMetaEnum keepPolicyEnum = CookieJar::staticMetaObject.enumerator(CookieJar::staticMetaObject.indexOfEnumerator("KeepPolicy"));
    settings.setValue(QLatin1String("keepCookiesUntil"), QLatin1String(keepPolicyEnum.valueToKey(keepPolicy)));
    int sessionLength = cookieSessionCombo->currentIndex();
    switch (sessionLength) {
    case 1: sessionLength = 1; break;
    case 2: sessionLength = 2; break;
    case 3: sessionLength = 3; break;
    case 4: sessionLength = 7; break;
    case 5: sessionLength = 30; break;
    default:
    case 0: sessionLength = -1; break;
    }
    settings.setValue(QLatin1String("sessionLength"), sessionLength);
    settings.setValue(QLatin1String("filterTrackingCookies"), filterTrackingCookiesCheckbox->isChecked());
    settings.setValue(QLatin1String("blockThirdPartyCookies"), blockThirdPartyCookies->isChecked());
    settings.endGroup();

    // Connections & Storage hardening (PRIV01).
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("httpsFirst"), httpsFirst->isChecked());
    settings.setValue(QLatin1String("trimReferer"), trimReferer->isChecked());
    settings.setValue(QLatin1String("webrtcIpProtection"), webrtcProtection->isChecked());
    settings.setValue(QLatin1String("secureDnsMode"), secureDnsMode->currentIndex());
    settings.setValue(QLatin1String("secureDnsServer"), secureDnsServer->text().trimmed());
    // The PRIV01 bool is kept in sync so an older build reading it
    // lands on "automatic" rather than "off".
    settings.setValue(QLatin1String("secureDns"), secureDnsMode->currentIndex() != 0);
    settings.setValue(QLatin1String("clearOnExit"), clearOnExit->isChecked());
    settings.setValue(QLatin1String("reportUtcTimezone"), reportUtcTimezone->isChecked());
    settings.setValue(QLatin1String("normalizeAcceptLanguage"), normalizeAcceptLanguage->isChecked());
    settings.setValue(QLatin1String("securityLevel"), securityLevelCombo->currentIndex());
    settings.endGroup();

    // PRIV02: TZ is process environment — applying it now reaches
    // engine processes spawned from here on; the ones already running
    // keep their zone until restart.
    BrowserProfile::applyFingerprintEnvironment();

    // Search engines: flush the per-engine suggestion opt-ins the user
    // edited in the combo session (skipped for engines that vanished
    // meanwhile), then the displayed engine becomes the default.
    {
        OpenSearchManager *searchManager = ToolbarSearch::openSearchManager();
        stashSearchSuggestions();
        for (auto it = m_pendingSuggestions.cbegin();
             it != m_pendingSuggestions.cend(); ++it) {
            if (searchManager->engineExists(it.key()))
                searchManager->setSuggestionsEnabledForEngine(it.key(), it.value());
        }
        m_pendingSuggestions.clear();
        const QString engineName = defaultEngineCombo->currentText();
        if (searchManager->engineExists(engineName))
            searchManager->setCurrentEngineName(engineName);
        m_engineComboDirty = false;
    }

    // Network
    settings.beginGroup(QLatin1String("network"));
    settings.setValue(QLatin1String("cacheEnabled"), networkCache->isChecked());
    settings.setValue(QLatin1String("maximumCacheSize"), networkCacheMaximumSizeSpinBox->value());
    settings.endGroup();

    // proxy
    settings.beginGroup(QLatin1String("proxy"));
    settings.setValue(QLatin1String("enabled"), proxySupport->isChecked());
    settings.setValue(QLatin1String("type"), proxyType->currentIndex());
    settings.setValue(QLatin1String("hostName"), proxyHostName->text());
    settings.setValue(QLatin1String("port"), proxyPort->text());
    settings.setValue(QLatin1String("userName"), proxyUserName->text());
    settings.setValue(QLatin1String("password"), SecureStore::sealString(proxyPassword->text()));
    settings.endGroup();

    // Tabs
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("tabBarPosition"), tabBarPosition->currentIndex());
    settings.setValue(QLatin1String("selectNewTabs"), selectTabsWhenCreated->isChecked());
    settings.setValue(QLatin1String("confirmClosingMultipleTabs"), confirmClosingMultipleTabs->isChecked());
    settings.setValue(QLatin1String("oneCloseButton"), oneCloseButton->isChecked());
    settings.setValue(QLatin1String("quitAsLastTabClosed"), quitAsLastTabClosed->isChecked());
    settings.setValue(QLatin1String("openTargetBlankLinksIn"), openTargetBlankLinksIn->currentIndex());
    settings.setValue(QLatin1String("openLinksFromAppsIn"), openLinksFromAppsIn->currentIndex());
    settings.endGroup();

    settings.beginGroup(QLatin1String("autofill"));
    settings.setValue(QLatin1String("passwordForms"), autoFillPasswordFormsCheckBox->isChecked());
    settings.endGroup();

    // Re-apply: the profile-level settings (was
    // BrowserApplication::loadSettings()), then each live manager.
    // The off-the-record profile gets the same treatment when it
    // exists — private browsing keeps user preferences.
    BrowserProfile::applySettings(BrowserProfile::normalProfile());
    if (QWebEngineProfile *otrProfile = BrowserProfile::privateProfileIfCreated())
        BrowserProfile::applySettings(otrProfile);
    NetworkAccessManager::instance()->loadSettings();
    CookieJar::instance()->loadSettings();
    HistoryManager::instance()->loadSettings();
    AutoFillManager::instance()->loadSettings();

    // Per-page settings (user agent, open-links-in preference) — was a
    // BrowserApplication::mainWindows() loop over each window's
    // TabWidget.  Every live WebView re-reads its page settings
    // instead; the widgets are reachable without BrowserMainWindow.
    // Live TabWidgets also re-read the tabs group so the tab bar
    // position and corner buttons change without a restart.
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        if (WebView *view = qobject_cast<WebView*>(widget))
            view->loadSettings();
        else if (TabWidget *tabs = qobject_cast<TabWidget*>(widget))
            tabs->loadSettings();
        else if (BrowserMainWindow *window = qobject_cast<BrowserMainWindow*>(widget))
            window->applySearchBoxVisibility();
    }
}

void SettingsDialog::accept()
{
    saveToSettings();
    QDialog::accept();
}

void SettingsDialog::showCookies()
{
    CookieDialog dialog(CookieJar::instance(), this);
    dialog.exec();
}

void SettingsDialog::showExceptions()
{
    CookieExceptionsDialog dialog(CookieJar::instance(), this);
    dialog.exec();
}

void SettingsDialog::chooseDownloadDirectory()
{
    QString fileName = QFileDialog::getExistingDirectory(this, tr("Choose Directory"), downloadsLocation->text());
    downloadsLocation->setText(fileName);
}

void SettingsDialog::chooseDownloadProgram()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Choose Program"), externalDownloadPath->text());
    if (fileName.contains(QLatin1Char(' ')))
        fileName = QString(QLatin1String("\"%1\"")).arg(fileName);
    externalDownloadPath->setText(fileName);
}

void SettingsDialog::chooseFont()
{
    bool ok;
    QFont font = QFontDialog::getFont(&ok, m_standardFont, this);
    if (ok) {
        m_standardFont = font;
        standardLabel->setText(QString(QLatin1String("%1 %2")).arg(font.family()).arg(font.pointSize()));
    }
}

void SettingsDialog::chooseFixedFont()
{
    bool ok;
    QFont font = QFontDialog::getFont(&ok, m_fixedFont, this);
    if (ok) {
        m_fixedFont = font;
        fixedLabel->setText(QString(QLatin1String("%1 %2")).arg(font.family()).arg(font.pointSize()));
    }
}

void SettingsDialog::setHomeToCurrentPage()
{
    // TODO(MIG14): this used to go through BrowserMainWindow::
    // currentTab(); until windows exist again, use the visible WebView
    // under the parent window — a QTabWidget hides all non-current
    // pages, so the unhidden view is the current tab.
    QWidget *window = parentWidget();
    if (!window)
        return;
    const QList<WebView*> views = window->findChildren<WebView*>();
    for (WebView *view : views) {
        if (!view->isHidden()) {
            homeLineEdit->setText(QString::fromUtf8(view->url().toEncoded()));
            return;
        }
    }
}

void SettingsDialog::chooseAcceptLanguage()
{
    AcceptLanguageDialog dialog(this);
    dialog.exec();
}

void SettingsDialog::chooseStyleSheet()
{
    QUrl url = QUrl::fromEncoded(userStyleSheet->text().toUtf8());
    QString fileName = QFileDialog::getOpenFileName(this, tr("Choose CSS File"), url.toLocalFile());
    userStyleSheet->setText(QString::fromUtf8(QUrl::fromLocalFile(fileName).toEncoded()));
}

void SettingsDialog::editAutoFillUser()
{
    AutoFillDialog dialog(this);
    dialog.exec();
}

void SettingsDialog::manageEngines()
{
    OpenSearchDialog dialog(this);
    dialog.exec();
    // The manager emits changed() for every add/remove; refresh once
    // more so an untouched list still resyncs the combo.
    refreshSearchEngines();
}

// The combo mirrors OpenSearchManager's engine list.  A name the user
// picked in the combo but has not saved yet survives external manager
// changes; otherwise the combo tracks the manager's current engine.
void SettingsDialog::refreshSearchEngines()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    QString wanted = defaultEngineCombo->currentText();
    if (!m_engineComboDirty || !manager->engineExists(wanted))
        wanted = manager->currentEngineName();

    defaultEngineCombo->clear();
    QStringList names = manager->allEnginesNames();
    names.sort(Qt::CaseInsensitive);
    defaultEngineCombo->addItems(names);
    const int index = defaultEngineCombo->findText(wanted);
    if (index != -1)
        defaultEngineCombo->setCurrentIndex(index);

    refreshSearchSuggestions();
}

// The Search Suggestions checkbox describes the engine currently shown
// in the combo.  Rebinding stashes the visible edit into
// m_pendingSuggestions so saveToSettings applies every engine the user
// touched (apply-on-OK, not on combo change).
void SettingsDialog::refreshSearchSuggestions()
{
    stashSearchSuggestions();
    m_suggestionsEngine = defaultEngineCombo->currentText();

    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = manager->engine(m_suggestionsEngine);
    const bool capable = engine && engine->providesSuggestions();
    searchSuggestionsCheckBox->setEnabled(capable);

    bool enabled = capable && manager->suggestionsEnabledForEngine(m_suggestionsEngine);
    if (m_pendingSuggestions.contains(m_suggestionsEngine))
        enabled = capable && m_pendingSuggestions.value(m_suggestionsEngine);
    searchSuggestionsCheckBox->setChecked(enabled);

    if (engine) {
        // The engine name comes from (possibly remote) OpenSearch XML —
        // escaped so a markup-looking name stays literal.
        searchSuggestionsHint->setText(
            tr("Suggestions send each keystroke to %1 before you press "
               "Enter. Off by default; the choice is stored per engine.")
                .arg(SafeText::escaped(engine->name())));
    } else {
        searchSuggestionsHint->setText(
            tr("Suggestions send each keystroke to the selected engine "
               "before you press Enter. Off by default."));
    }
}

// Record the checkbox state for the engine it currently describes — a
// disabled checkbox means that engine has no suggest endpoint, in
// which case its stored choice is left alone.
void SettingsDialog::stashSearchSuggestions()
{
    if (!m_suggestionsEngine.isEmpty() && searchSuggestionsCheckBox->isEnabled())
        m_pendingSuggestions[m_suggestionsEngine] = searchSuggestionsCheckBox->isChecked();
}

// Shared manifest pre-check for load/install: warns about MV2
// packages and chrome.* APIs Qt WebEngine cannot serve, and asks
// whether to proceed anyway.
static bool confirmExtensionLoad(const ExtensionManager::Manifest &manifest,
                                 const QString &title, QWidget *parent)
{
    QStringList warnings;
    if (!manifest.error.isEmpty())
        warnings << manifest.error;
    if (!manifest.unsupported.isEmpty())
        warnings << SettingsDialog::tr("Declares chrome.* APIs unavailable in "
                                       "Qt WebEngine (calls will fail): %1")
                    .arg(manifest.unsupported.join(QLatin1String(", ")));
    if (!manifest.unverified.isEmpty())
        warnings << SettingsDialog::tr("Declares chrome.* APIs with only "
                                       "partial Qt WebEngine support: %1")
                    .arg(manifest.unverified.join(QLatin1String(", ")));
    if (warnings.isEmpty())
        return true;
    return QMessageBox::warning(parent, title,
        warnings.join(QLatin1String("\n\n"))
            + SettingsDialog::tr("\n\nContinue anyway?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes;
}

void SettingsDialog::loadExtension()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, tr("Select Unpacked Extension Folder"), QDir::homePath());
    if (path.isEmpty())
        return;
    if (!confirmExtensionLoad(ExtensionManager::inspectManifest(path),
                              tr("Load Extension"), this))
        return;
    ExtensionManager::instance()->loadExtension(path);
}

void SettingsDialog::installExtension()
{
    QMessageBox choice(this);
    choice.setWindowTitle(tr("Install Extension"));
    choice.setText(tr("Install from an unpacked folder or a .zip package?"));
    QAbstractButton *folderButton =
        choice.addButton(tr("Folder..."), QMessageBox::AcceptRole);
    QAbstractButton *zipButton =
        choice.addButton(tr("ZIP Package..."), QMessageBox::AcceptRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();

    QString path;
    if (choice.clickedButton() == folderButton)
        path = QFileDialog::getExistingDirectory(
            this, tr("Select Unpacked Extension Folder"), QDir::homePath());
    else if (choice.clickedButton() == zipButton)
        path = QFileDialog::getOpenFileName(
            this, tr("Select Extension Package"), QDir::homePath(),
            tr("Extension packages (*.zip);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!confirmExtensionLoad(ExtensionManager::inspectManifest(path),
                              tr("Install Extension"), this))
        return;
    ExtensionManager::instance()->installExtension(path);
}

void SettingsDialog::removeExtension()
{
    QTreeWidgetItem *item = extensionsTree->currentItem();
    if (!item)
        return;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty())
        return;
    if (QMessageBox::question(this, tr("Remove Extension"),
            tr("Remove the extension \"%1\"?").arg(item->text(0)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
        return;
    ExtensionManager::instance()->removeExtension(id);
}

void SettingsDialog::extensionSelectionChanged()
{
    QTreeWidgetItem *item = extensionsTree->currentItem();
    extensionRemoveButton->setEnabled(item != nullptr);
    if (!item) {
        extensionDetailsLabel->clear();
        return;
    }
    const QString id = item->data(0, Qt::UserRole).toString();
    const QList<ExtensionManager::ExtensionInfo> list =
        ExtensionManager::instance()->extensions();
    for (const ExtensionManager::ExtensionInfo &info : list) {
        if (info.id != id)
            continue;
        // Built-in component extensions ship with the profile and
        // cannot be uninstalled.
        extensionRemoveButton->setEnabled(!info.builtin);
        QStringList lines;
        if (!info.description.isEmpty())
            lines << info.description;
        if (!info.error.isEmpty())
            lines << tr("Error: %1").arg(info.error);
        if (info.builtin)
            lines << tr("Built-in component extension.");
        if (info.actionPopupUrl.isValid())
            lines << tr("Popup URL: %1").arg(info.actionPopupUrl.toString());
        if (!info.path.isEmpty()) {
            const ExtensionManager::Manifest manifest =
                ExtensionManager::inspectManifest(info.path);
            if (!manifest.version.isEmpty())
                lines << tr("Version %1").arg(manifest.version);
            if (!manifest.permissions.isEmpty())
                lines << tr("Permissions: %1")
                             .arg(manifest.permissions.join(QLatin1String(", ")));
            if (!manifest.unsupported.isEmpty())
                lines << tr("Not available in Qt WebEngine: %1")
                             .arg(manifest.unsupported.join(QLatin1String(", ")));
            if (!manifest.unverified.isEmpty())
                lines << tr("Partially supported in Qt WebEngine: %1")
                             .arg(manifest.unverified.join(QLatin1String(", ")));
        }
        extensionDetailsLabel->setText(lines.join(QLatin1Char('\n')));
        return;
    }
    extensionDetailsLabel->clear();
}

void SettingsDialog::extensionItemChanged(QTreeWidgetItem *item, int column)
{
    if (column != 0 || !item)
        return;
    const QString id = item->data(0, Qt::UserRole).toString();
    if (id.isEmpty())
        return;
    const QList<ExtensionManager::ExtensionInfo> list =
        ExtensionManager::instance()->extensions();
    for (const ExtensionManager::ExtensionInfo &info : list) {
        if (info.id != id)
            continue;
        const bool enabled = item->checkState(0) == Qt::Checked;
        if (enabled != info.enabled)
            ExtensionManager::instance()->setExtensionEnabled(id, enabled);
        return;
    }
}

void SettingsDialog::extensionError(const QString &message)
{
    QMessageBox::warning(this, tr("Extensions"), message);
}

void SettingsDialog::refreshExtensions()
{
    QString selectedId;
    if (QTreeWidgetItem *current = extensionsTree->currentItem())
        selectedId = current->data(0, Qt::UserRole).toString();

    // itemChanged would fire on every checkbox sync while rebuilding.
    const QSignalBlocker blocker(extensionsTree);
    extensionsTree->clear();
    const QList<ExtensionManager::ExtensionInfo> list =
        ExtensionManager::instance()->extensions();
    for (const ExtensionManager::ExtensionInfo &info : list) {
        QTreeWidgetItem *item = new QTreeWidgetItem(extensionsTree);
        item->setText(0, info.name.isEmpty() ? info.id : info.name);
        item->setCheckState(0, info.enabled ? Qt::Checked : Qt::Unchecked);
        QString status;
        if (!info.error.isEmpty())
            status = tr("Error");
        else if (!info.loaded)
            status = tr("Not loaded");
        else
            status = info.enabled ? tr("Enabled") : tr("Disabled");
        item->setText(1, status);
        item->setText(2, info.builtin ? tr("Built-in")
                                    : (info.installed ? tr("Installed")
                                                      : tr("Session")));
        item->setData(0, Qt::UserRole, info.id);
        item->setToolTip(0, SafeText::escaped(info.path));
        if (info.id == selectedId)
            extensionsTree->setCurrentItem(item);
    }
    extensionSelectionChanged();
}

void SettingsDialog::refreshUserScripts()
{
    userScriptsList->clear();
    const QStringList names = ExtensionManager::instance()->userScriptNames();
    if (names.isEmpty()) {
        QListWidgetItem *empty = new QListWidgetItem(
            tr("(empty — drop .js files in the folder)"), userScriptsList);
        empty->setFlags(Qt::NoItemFlags);
        return;
    }
    userScriptsList->addItems(names);
}

void SettingsDialog::openUserScriptsFolder()
{
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(ExtensionManager::userScriptsPath()));
}

void SettingsDialog::reloadUserScripts()
{
    // userScriptsChanged refreshes the list once the profiles re-scan.
    ExtensionManager::instance()->reloadUserScripts();
}

void SettingsDialog::refreshPermissions()
{
    QString selectedKey;
    if (QTreeWidgetItem *current = permissionsTree->currentItem())
        selectedKey = current->data(0, Qt::UserRole).toString();

    permissionsTree->clear();
    const QList<WebPermissionManager::Entry> entries =
        WebPermissionManager::instance()->entries();
    for (const WebPermissionManager::Entry &entry : entries) {
        QTreeWidgetItem *item = new QTreeWidgetItem(permissionsTree);
        item->setText(0, QString::fromUtf8(entry.origin.toEncoded()));
        item->setText(1, WebPermissionManager::typeName(entry.type));
        item->setText(2, entry.granted ? tr("Allowed") : tr("Denied"));
        // Removal needs the origin + type back; both are recoverable
        // from one key.
        item->setData(0, Qt::UserRole,
            QString::fromUtf8(entry.origin.toEncoded()) + QLatin1Char('|')
            + QString::number(static_cast<int>(entry.type)));
        if (item->data(0, Qt::UserRole).toString() == selectedKey)
            permissionsTree->setCurrentItem(item);
    }

    // JSCTL: per-site JavaScript rules share this audit table.  Their
    // key is "js|<host>" so removal can route to the right store.
    ScriptControlManager *scripts = ScriptControlManager::instance();
    const auto addScriptRule = [this, &selectedKey](
            const QString &host, bool allowed) {
        QTreeWidgetItem *item = new QTreeWidgetItem(permissionsTree);
        item->setText(0, host);
        item->setText(1, tr("JavaScript"));
        item->setText(2, allowed ? tr("Allowed") : tr("Blocked"));
        item->setData(0, Qt::UserRole,
                      QLatin1String("js|") + host);
        if (item->data(0, Qt::UserRole).toString() == selectedKey)
            permissionsTree->setCurrentItem(item);
    };
    for (const QString &host : scripts->allowedHosts())
        addScriptRule(host, true);
    for (const QString &host : scripts->blockedHosts())
        addScriptRule(host, false);
    permissionSelectionChanged();
}

void SettingsDialog::permissionSelectionChanged()
{
    permissionRemoveButton->setEnabled(permissionsTree->currentItem() != nullptr);
}

void SettingsDialog::removePermission()
{
    QTreeWidgetItem *item = permissionsTree->currentItem();
    if (!item)
        return;
    const QStringList parts = item->data(0, Qt::UserRole).toString()
        .split(QLatin1Char('|'));
    if (parts.count() != 2)
        return;
    if (parts.at(0) == QLatin1String("js")) {
        // JSCTL row — clear the per-site JavaScript rule.
        ScriptControlManager::instance()->setRuleForHost(
            parts.at(1), ScriptControlManager::SiteDefault);
        return;
    }
    WebPermissionManager::instance()->removeEntry(
        QUrl::fromEncoded(parts.at(0).toUtf8()),
        static_cast<QWebEnginePermission::PermissionType>(parts.at(1).toInt()));
}

void SettingsDialog::clearPermissions()
{
    ScriptControlManager *scripts = ScriptControlManager::instance();
    if (WebPermissionManager::instance()->entries().isEmpty()
        && scripts->allowedHosts().isEmpty()
        && scripts->blockedHosts().isEmpty())
        return;
    if (QMessageBox::question(this, tr("Clear Site Permissions"),
            tr("Remove every remembered site permission?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
        return;
    WebPermissionManager::instance()->clearEntries();
    scripts->clearPersistentRules();
}

// SECLVL: explains the selected tier and greys the Enable Javascript
// checkbox while Safest is picked — the tier overrides it profile-wide
// (the stored value itself is preserved for when the tier comes back
// down).
void SettingsDialog::updateSecurityLevelHint()
{
    QString hint;
    switch (securityLevelCombo->currentIndex()) {
    default:
    case PrivacyRequestInterceptor::Standard:
        hint = tr("Standard: JavaScript runs everywhere and sites may "
                  "autoplay media. This is the default browser behavior.");
        break;
    case PrivacyRequestInterceptor::Safer:
        hint = tr("Safer: pages loaded over plain HTTP cannot load "
                  "external JavaScript (scripts, workers) and media "
                  "only plays after a click. Scripts inlined into the "
                  "page itself can still run; pages on this machine "
                  "(localhost) are exempt.");
        break;
    case PrivacyRequestInterceptor::Safest:
        hint = tr("Safest: JavaScript is disabled on every site and "
                  "media only plays after a click. Many websites will "
                  "break. \"Enable Javascript\" above is ignored while "
                  "this level is selected.");
        break;
    }
    securityLevelHint->setText(hint);
    enableJavascript->setEnabled(
        securityLevelCombo->currentIndex() != PrivacyRequestInterceptor::Safest);
}

// Prompts for a new master passphrase — entered twice — and returns
// Accepted with *passphrase filled only for matching input of at
// least 8 characters.  The dialog keeps the field values out of
// tooltips/accessibility text; SecureStore wipes its own copies.
static int promptNewPassphrase(QWidget *parent, bool change,
                               QString *passphrase)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(change
        ? SettingsDialog::tr("Change Master Passphrase")
        : SettingsDialog::tr("Set Master Passphrase"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);

    QLabel *info = new QLabel(change
        ? SettingsDialog::tr("Choose a new master passphrase (at least 8 characters).\n"
            "Everything sealed under the old one is re-encrypted.")
        : SettingsDialog::tr("Choose a master passphrase (at least 8 characters).\n"
            "Saved passwords will then be decryptable only with it — the\n"
            "key is derived in memory and never written to disk."));
    info->setWordWrap(true);
    layout->addWidget(info);

    QLineEdit *first = new QLineEdit;
    first->setEchoMode(QLineEdit::Password);
    QLineEdit *second = new QLineEdit;
    second->setEchoMode(QLineEdit::Password);
    QFormLayout *form = new QFormLayout;
    form->addRow(SettingsDialog::tr("Passphrase:"), first);
    form->addRow(SettingsDialog::tr("Repeat:"), second);
    layout->addLayout(form);

    QLabel *hint = new QLabel;
    hint->setStyleSheet(QLatin1String("color: #b00000"));
    layout->addWidget(hint);

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);

    for (;;) {
        if (dialog.exec() != QDialog::Accepted)
            return QDialog::Rejected;
        if (first->text().length() < 8) {
            hint->setText(SettingsDialog::tr(
                "The passphrase must be at least 8 characters."));
            continue;
        }
        if (first->text() != second->text()) {
            hint->setText(SettingsDialog::tr(
                "The passphrases do not match."));
            second->clear();
            continue;
        }
        *passphrase = first->text();
        return QDialog::Accepted;
    }
}

void SettingsDialog::refreshCredentialUi()
{
    const bool crypto = SecureStore::isAvailable();
    const bool enabled =
        crypto && SecureStore::passphraseProtectionEnabled();
    const bool unlocked = SecureStore::isUnlocked();

    if (!crypto) {
        credentialStatusLabel->setText(tr(
            "The crypto backend is unavailable — saved passwords"
            " cannot be stored on this system."));
    } else if (enabled && unlocked) {
        credentialStatusLabel->setText(tr(
            "Master passphrase set — the saved-password key is"
            " derived in memory and never written to disk."
            " The store is currently unlocked."));
    } else if (enabled) {
        credentialStatusLabel->setText(tr(
            "Master passphrase set — the store is locked and needs"
            " the passphrase before saved passwords can be read"
            " or written."));
    } else {
        credentialStatusLabel->setText(tr(
            "Saved passwords are encrypted with a key stored next"
            " to the data — that protects them from other users and"
            " offline reads, but not from malware running as you."
            " Set a master passphrase to protect the key itself."));
    }

    credentialPassphraseButton->setEnabled(crypto);
    credentialPassphraseButton->setText(enabled
        ? tr("Change Master Passphrase...")
        : tr("Set Master Passphrase..."));
    credentialRemoveButton->setEnabled(enabled);
    credentialLockButton->setEnabled(enabled && unlocked);
}

void SettingsDialog::credentialPassphraseChange()
{
    const bool enabled = SecureStore::passphraseProtectionEnabled();
    if (enabled && !SecureStore::isUnlocked()
        && !SecureStore::ensureUnlocked(this))
        return;

    QString passphrase;
    if (promptNewPassphrase(this, enabled, &passphrase)
        != QDialog::Accepted)
        return;

    QString error;
    const bool ok = enabled
        ? SecureStore::changePassphrase(passphrase, &error)
        : SecureStore::enablePassphraseProtection(passphrase, &error);
    if (!ok)
        QMessageBox::warning(this, tr("Credential Store"), error);
    refreshCredentialUi();
}

void SettingsDialog::credentialPassphraseRemove()
{
    if (!SecureStore::passphraseProtectionEnabled())
        return;
    if (!SecureStore::isUnlocked() && !SecureStore::ensureUnlocked(this))
        return;
    if (QMessageBox::question(this, tr("Remove Master Passphrase"),
            tr("Saved passwords will go back to being protected by a"
               " key file stored next to the data. Remove the"
               " passphrase?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
        return;

    QString error;
    if (!SecureStore::disablePassphraseProtection(&error))
        QMessageBox::warning(this, tr("Credential Store"), error);
    refreshCredentialUi();
}

void SettingsDialog::credentialStoreLock()
{
    SecureStore::lock();
    refreshCredentialUi();
}
