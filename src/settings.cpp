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
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserprofile.h"
#include "containermanager.h"
#include "cookiedialog.h"
#include "cookieexceptionsdialog.h"
#include "cookiejar.h"
#include "extensionmanager.h"
#include "extensionreviewdialog.h"
#include "historymanager.h"
#include "networkaccessmanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "popupblocker.h"
#include "privacyrequestinterceptor.h"
#include "safetext.h"
#include "scopeshortcuts.h"
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
#include <qscreen.h>
#include <qscrollarea.h>
#include <qsettings.h>
#include <qstackedwidget.h>
#include <qstandardpaths.h>
#include <qfiledialog.h>
#include <qheaderview.h>
#include <qpixmap.h>
#include <qpushbutton.h>
#include <qregularexpression.h>
#include <qtreewidget.h>
#include <qboxlayout.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QWidget(parent)
{
    setupUi(this);

    // PREFS01: the page is tab-hosted, so the button box connects by
    // hand — the .ui can't target accept()/reject() on a QWidget root.
    connect(buttonBox, &QDialogButtonBox::accepted,
            this, &SettingsDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected,
            this, &SettingsDialog::reject);

    // UIP03: Vivaldi-style sidebar navigation — pagesList rows mirror
    // the order the QStackedWidget pages had as QTabWidget tabs, and
    // the two stay in sync both ways so the persisted "currentTab"
    // index keeps its old meaning.
    connect(pagesList, &QListWidget::currentRowChanged,
            tabWidget, &QStackedWidget::setCurrentIndex);
    connect(tabWidget, &QStackedWidget::currentChanged,
            pagesList, QOverload<int>::of(&QListWidget::setCurrentRow));
    static const char *const pageIcons[] = {
        "go-home",          // General
        "edit-find",        // Search
        "zoom-original",    // Appearance
        "security-high",    // Privacy
        "tab-new",          // Tab Settings
        "go-next",          // Proxy
        "user-bookmarks",   // AutoFill
        "system-run",       // Advanced
        "list-add",         // Extensions
        "folder-new",       // Containers
    };
    for (int i = 0;
         i < pagesList->count()
         && i < int(sizeof(pageIcons) / sizeof(pageIcons[0])); ++i) {
        if (QListWidgetItem *item = pagesList->item(i))
            item->setIcon(AroraIcon::get(QLatin1String(pageIcons[i])));
    }
    // Fixed-width nav column sized to the longest translated label.
    pagesList->setFixedWidth(pagesList->sizeHintForColumn(0)
                             + 2 * pagesList->frameWidth() + 8);

    // UIP06: the stack's sizeHint is the tallest page, which can
    // exceed the screen and push the button box out of reach.
    // Wrap each page in a scroll area (scroll areas live on the page
    // content, not the dialog shell) so pages scroll instead — the
    // sidebar keeps its fixed height and the stack keeps its indices.
    for (int i = 0; i < tabWidget->count(); ++i) {
        QWidget *page = tabWidget->widget(i);
        tabWidget->removeWidget(page);
        QScrollArea *area = new QScrollArea;
        area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true);
        area->setWidget(page);
        tabWidget->insertWidget(i, area);
    }

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
    connect(extensionUpdateButton, &QPushButton::clicked, this, &SettingsDialog::checkExtensionUpdates);
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
    // EXT03: the opt-in background check may finish while the dialog
    // is open — only the button-triggered check pops the summary.
    connect(extensions, &ExtensionManager::updateCheckStarted, this,
            [this]() { extensionUpdateButton->setEnabled(false); });
    connect(extensions, &ExtensionManager::updateCheckFinished, this,
            &SettingsDialog::extensionUpdateCheckFinished);
    connect(extensions, &ExtensionManager::updateAvailable, this,
            [this](const ExtensionManager::UpdateResult &) {
        extensionSelectionChanged();
    });
    if (BrowserApplication::isTorMode()) {
        // EXT04: a tor process never attaches a profile to the
        // extension manager — extensions are a deanonymization
        // surface there.  State the hard refusal rather than showing
        // controls that could only fail.
        extensionsTree->setEnabled(false);
        extensionLoadButton->setEnabled(false);
        extensionInstallButton->setEnabled(false);
        extensionUpdateButton->setEnabled(false);
        extensionAutoUpdateCheck->setEnabled(false);
        extensionRemoveButton->setEnabled(false);
        extensionsHintLabel->setText(
            tr("Extensions never run in a Tor window."));
    } else if (!ExtensionManager::isSupported()) {
        extensionsTree->setEnabled(false);
        extensionLoadButton->setEnabled(false);
        extensionInstallButton->setEnabled(false);
        extensionUpdateButton->setEnabled(false);
        extensionAutoUpdateCheck->setEnabled(false);
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
    // POPUP01: pop-up exceptions share the table ("popup|" keys).
    connect(PopupBlocker::instance(), &PopupBlocker::changed,
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
    if (BrowserApplication::isTorMode()) {
        // SEC20: a tor process never resolves names locally — the
        // stored DoH mode is ignored here regardless of the controls.
        // Disabled rather than blanked so the choice still shows and
        // keeps applying to normal windows.
        secureDnsMode->setEnabled(false);
        secureDnsServer->setEnabled(false);
        secureDnsMode->setToolTip(tr(
            "Tor windows resolve names remotely through the Tor proxy "
            "— local DNS-over-HTTPS would bypass the tunnel."));
    }

    // SEC13: master-passphrase controls for the credential store.
    connect(credentialPassphraseButton, &QPushButton::clicked,
            this, &SettingsDialog::credentialPassphraseChange);
    connect(credentialRemoveButton, &QPushButton::clicked,
            this, &SettingsDialog::credentialPassphraseRemove);
    connect(credentialLockButton, &QPushButton::clicked,
            this, &SettingsDialog::credentialStoreLock);
    refreshCredentialUi();

    // CONT03: the Containers page mirrors the ContainerManager
    // registry.  Container names are user-chosen but still render
    // literal — a markup-looking name must not mark up the list.
    containersList->setItemDelegate(new PlainTextItemDelegate(containersList));
    connect(containersList, &QListWidget::itemSelectionChanged,
            this, &SettingsDialog::containerSelectionChanged);
    connect(containerNewButton, &QPushButton::clicked,
            this, &SettingsDialog::containerNew);
    connect(containerEditButton, &QPushButton::clicked,
            this, &SettingsDialog::containerEdit);
    connect(containerRemoveButton, &QPushButton::clicked,
            this, &SettingsDialog::containerRemove);
    // CONT04: the same mirror for the per-container site rules.
    containerSitesList->setItemDelegate(
        new PlainTextItemDelegate(containerSitesList));
    connect(containerSitesList, &QListWidget::itemSelectionChanged,
            this, [this]() {
        containerSiteRemoveButton->setEnabled(
            containerSitesList->currentItem() != nullptr);
    });
    connect(containerSiteRemoveButton, &QPushButton::clicked,
            this, &SettingsDialog::containerSiteRemove);
    ContainerManager *containers = ContainerManager::instance();
    connect(containers, &ContainerManager::containersChanged,
            this, &SettingsDialog::refreshContainers);
    connect(containers, &ContainerManager::siteRulesChanged,
            this, &SettingsDialog::refreshContainerSites);
    refreshContainers();

    // SRCH02: the Search tab mirrors the shared OpenSearchManager —
    // engine add/remove (the Manage dialog edits the same manager) and
    // external default-engine switches refresh the combo.  The
    // suggestions checkbox rebinds to whichever engine the combo
    // shows; QComboBox::activated marks user picks so a manager change
    // does not clobber an unsaved selection.
    OpenSearchManager *searchManager = ToolbarSearch::openSearchManager();
    connect(defaultEngineCombo, &QComboBox::currentTextChanged,
            this, &SettingsDialog::refreshSearchSuggestions);
    connect(defaultEngineCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int) { m_engineComboDirty = true; });
    connect(searchManager, &OpenSearchManager::changed,
            this, &SettingsDialog::refreshSearchEngines);
    connect(searchManager, &OpenSearchManager::currentEngineChanged,
            this, &SettingsDialog::refreshSearchEngines);

    // SRCH04: the private/image pickers get the same unsaved-pick
    // protection as the default combo, and the field/button radios
    // share the showSearchBox key with the General page's checkbox.
    connect(privateEngineCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int) { m_privateEngineComboDirty = true; });
    connect(imageEngineCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int) { m_imageEngineComboDirty = true; });
    connect(searchFieldRadio, &QRadioButton::toggled, this,
            [this](bool on) { showSearchBox->setChecked(on); });
    connect(showSearchBox, &QCheckBox::toggled, this, [this](bool on) {
        (on ? static_cast<QRadioButton*>(searchFieldRadio)
            : static_cast<QRadioButton*>(searchButtonRadio))
                ->setChecked(true);
    });

    // SRCH05: the inline engine editor — engine names/keywords come
    // from (possibly remote) OpenSearch XML so the list renders them
    // as plain text.  Edits apply to the shared manager immediately,
    // like the old Manage dialog did; typed field edits are staged in
    // m_engineFieldsDirty until editingFinished or the next action
    // commits them.
    engineTree->setItemDelegate(new PlainTextItemDelegate(engineTree));
    engineTree->header()->setStretchLastSection(false);
    engineTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(engineTree, &QTreeWidget::itemSelectionChanged,
            this, &SettingsDialog::engineSelectionChanged);
    connect(engineAddButton, &QPushButton::clicked,
            this, &SettingsDialog::engineAdd);
    connect(engineRemoveButton, &QPushButton::clicked,
            this, &SettingsDialog::engineRemove);
    connect(engineDefaultsButton, &QPushButton::clicked,
            this, &SettingsDialog::engineRestoreDefaults);
    connect(engineUpButton, &QPushButton::clicked,
            this, [this]() { engineMove(-1); });
    connect(engineDownButton, &QPushButton::clicked,
            this, [this]() { engineMove(1); });
    const auto markEngineField = [this](QLineEdit *edit, const QString &field) {
        connect(edit, &QLineEdit::textChanged, this,
                [this, field](const QString &) {
            if (!m_populatingEngineForm)
                m_engineFieldsDirty.insert(field);
        });
        connect(edit, &QLineEdit::editingFinished,
                this, &SettingsDialog::commitEngineEdits);
    };
    markEngineField(engineNameEdit, QLatin1String("name"));
    markEngineField(engineNicknameEdit, QLatin1String("nickname"));
    markEngineField(engineUrlEdit, QLatin1String("url"));
    markEngineField(engineSuggestUrlEdit, QLatin1String("suggest"));
    markEngineField(engineImageUrlEdit, QLatin1String("image"));
    markEngineField(enginePostParamsEdit, QLatin1String("post"));
    markEngineField(engineImagePostParamsEdit, QLatin1String("imagepost"));
    connect(engineDefaultCheck, &QCheckBox::toggled,
            this, &SettingsDialog::engineAssignmentChanged);
    connect(enginePrivateCheck, &QCheckBox::toggled,
            this, &SettingsDialog::engineAssignmentChanged);

    // SRCH06: scoped-completion nickname rows — the token field only
    // edits while its scope is enabled; the reset button restores
    // every Search-page-owned preference.
    connect(shortcutBookmarksCheck, &QCheckBox::toggled,
            shortcutBookmarksToken, &QWidget::setEnabled);
    connect(shortcutHistoryCheck, &QCheckBox::toggled,
            shortcutHistoryToken, &QWidget::setEnabled);
    connect(shortcutTabsCheck, &QCheckBox::toggled,
            shortcutTabsToken, &QWidget::setEnabled);
    connect(resetSearchButton, &QPushButton::clicked,
            this, &SettingsDialog::resetSearchSettings);

    loadDefaults();
    loadFromSettings();
}

void SettingsDialog::openAtPage(Page page)
{
    tabWidget->setCurrentIndex(int(page));
}

int SettingsDialog::pageCount()
{
    return int(ContainersPage) + 1;
}

QString SettingsDialog::pageTitle(Page page)
{
    static const char *const titles[] = {
        QT_TR_NOOP("General"),
        QT_TR_NOOP("Search"),
        QT_TR_NOOP("Appearance"),
        QT_TR_NOOP("Privacy"),
        QT_TR_NOOP("Tab Settings"),
        QT_TR_NOOP("Proxy"),
        QT_TR_NOOP("AutoFill"),
        QT_TR_NOOP("Advanced"),
        QT_TR_NOOP("Extensions"),
        QT_TR_NOOP("Containers"),
    };
    const int index = int(page);
    if (index < 0 || index >= int(sizeof(titles) / sizeof(titles[0])))
        return QString();
    return tr(titles[index]);
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // UIP06: never open larger than ~90% of the screen — the wrapped
    // pages scroll to cover the overflow so OK/Cancel stay reachable.
    QScreen *screen = this->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QSize cap = screen->availableGeometry().size() * 9 / 10;
    const QSize bounded = size().boundedTo(cap);
    if (bounded != size())
        resize(bounded);
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
    // SRCH03+SRCH04: the same key backs the General checkbox and the
    // Search page's field/button radios — button mode when off.
    const bool showBox = settings.value(QLatin1String("showSearchBox"), false).toBool();
    showSearchBox->setChecked(showBox);
    searchFieldRadio->setChecked(showBox);
    searchButtonRadio->setChecked(!showBox);
    // SIDE01: the sidebar is off by default; its dock side is a
    // separate Left/Right choice applied live to every window.
    showSidebar->setChecked(
        settings.value(QLatin1String("showSidebar"), false).toBool());
    sidebarDockArea->setCurrentIndex(
        settings.value(QLatin1String("sidebarDockArea"),
                       int(Qt::LeftDockWidgetArea)).toInt()
            == int(Qt::RightDockWidgetArea) ? 1 : 0);
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
    // SRCH04: "Search with" context-menu results open unfocused.
    selectionSearchBackgroundCheck->setChecked(
        settings.value(QLatin1String("selectionSearchInBackground"),
                       false).toBool());
    settings.endGroup();

    // SRCH04: dedicated search-field display/behavior switches.
    settings.beginGroup(QLatin1String("toolbarsearch"));
    showEngineNicknameCheck->setChecked(
        settings.value(QLatin1String("showEngineNickname"), true).toBool());
    showEngineIconCheck->setChecked(
        settings.value(QLatin1String("showEngineIcon"), true).toBool());
    alwaysNewTabCheck->setChecked(
        settings.value(QLatin1String("alwaysNewTab"), false).toBool());
    keepTypedTextCheck->setChecked(
        settings.value(QLatin1String("keepTypedText"), true).toBool());
    settings.endGroup();

    // SRCH06: scoped-completion shortcut nicknames.
    shortcutBookmarksCheck->setChecked(
        ScopeShortcuts::enabled(ScopeShortcuts::BookmarksScope));
    shortcutHistoryCheck->setChecked(
        ScopeShortcuts::enabled(ScopeShortcuts::HistoryScope));
    shortcutTabsCheck->setChecked(
        ScopeShortcuts::enabled(ScopeShortcuts::TabsScope));
    shortcutBookmarksToken->setText(
        ScopeShortcuts::token(ScopeShortcuts::BookmarksScope));
    shortcutHistoryToken->setText(
        ScopeShortcuts::token(ScopeShortcuts::HistoryScope));
    shortcutTabsToken->setText(
        ScopeShortcuts::token(ScopeShortcuts::TabsScope));
    // setChecked only fires toggled on a change — sync the token
    // fields for the already-matching case.
    shortcutBookmarksToken->setEnabled(shortcutBookmarksCheck->isChecked());
    shortcutHistoryToken->setEnabled(shortcutHistoryCheck->isChecked());
    shortcutTabsToken->setEnabled(shortcutTabsCheck->isChecked());

    // SRCH04: engine assignments and per-context suggestion toggles
    // live on the OpenSearchManager (persisted in its openSearch
    // group), not in a settings group of their own.
    {
        OpenSearchManager *searchManager = ToolbarSearch::openSearchManager();
        keepFieldEngineCheck->setChecked(searchManager->keepFieldEngine());
        suggestInAddressFieldCheck->setChecked(
            searchManager->suggestionsInAddressField());
        suggestInSearchFieldCheck->setChecked(
            searchManager->suggestionsInSearchField());
        suggestOnlyWithKeywordCheck->setChecked(
            searchManager->suggestionsOnlyWithKeyword());
    }

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
    forceDarkMode->setChecked(settings.value(QLatin1String("forceDarkMode"), false).toBool());
    // POL03: Chromium's native autoscroll — on everywhere except
    // macOS, where middle-button scroll is not a platform convention.
#if defined(Q_OS_MACOS)
    const bool autoscrollDefault = false;
#else
    const bool autoscrollDefault = true;
#endif
    middleClickAutoscroll->setChecked(settings.value(
        QLatin1String("middleClickAutoscroll"),
        autoscrollDefault).toBool());
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
    // SAFE01: HTTPS-Only warning gate — defaults on alongside
    // httpsFirst (the upgrade still runs first; only the http: that
    // remains warns).
    httpsOnly->setChecked(settings.value(QLatin1String("httpsOnly"), true).toBool());
    // REF01: the PRIV01-era bool folds into the refererPolicy selector
    // — an old "trimReferer = false" maps to "Chromium default" (index
    // 0), anything else to "Trimmed" (index 1).
    const QVariant storedRefererPolicy =
        settings.value(QLatin1String("refererPolicy"));
    const int refererIndex = storedRefererPolicy.isValid()
        ? storedRefererPolicy.toInt()
        : (settings.value(QLatin1String("trimReferer"), true).toBool()
               ? 1 : 0);
    refererPolicy->setCurrentIndex(
        qBound(0, refererIndex, refererPolicy->count() - 1));
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
    strictTlsCiphers->setChecked(settings.value(QLatin1String("tlsStrictCiphers"), true).toBool());
    blockPings->setChecked(settings.value(QLatin1String("blockPings"), true).toBool());
    blockPrefetch->setChecked(settings.value(QLatin1String("blockPrefetch"), true).toBool());
    // TELEM02: DNS prefetch leaks every linked hostname to the
    // resolver — opt-in, off by default.
    dnsPrefetch->setChecked(settings.value(QLatin1String("dnsPrefetch"), false).toBool());
    blockRemoteFonts->setChecked(settings.value(QLatin1String("blockRemoteFonts"), false).toBool());
    blockThirdPartyWebSockets->setChecked(settings.value(QLatin1String("blockThirdPartyWebSockets"), false).toBool());
    clearOnExit->setChecked(settings.value(QLatin1String("clearOnExit"), false).toBool());
    // PRIV02 fingerprint normalization.
    reportUtcTimezone->setChecked(settings.value(QLatin1String("reportUtcTimezone"), false).toBool());
    normalizeAcceptLanguage->setChecked(settings.value(QLatin1String("normalizeAcceptLanguage"), false).toBool());
    // SAFE06: opt-in injection — off by default because the spoofing
    // is detectable and canvas-heavy sites can break.
    fingerprintProtection->setChecked(settings.value(QLatin1String("fingerprintProtection"), false).toBool());
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
    suspendTabs->setChecked(settings.value(QLatin1String("suspendTabs"), false).toBool());
    suspendTabsMinutes->setValue(settings.value(QLatin1String("suspendTabsMinutes"), 30).toInt());
    suspendTabsMinutes->setEnabled(suspendTabs->isChecked());
    openTargetBlankLinksIn->setCurrentIndex(settings.value(QLatin1String("openTargetBlankLinksIn"), TabWidget::NewSelectedTab).toInt());
    openLinksFromAppsIn->setCurrentIndex(settings.value(QLatin1String("openLinksFromAppsIn"), TabWidget::NewSelectedTab).toInt());
    settings.endGroup();

    settings.beginGroup(QLatin1String("autofill"));
    autoFillPasswordFormsCheckBox->setChecked(settings.value(QLatin1String("passwordForms"), true).toBool());
    settings.endGroup();

    // EXT03: the opt-in background update check (default off).
    extensionAutoUpdateCheck->setChecked(
        ExtensionManager::updateCheckEnabled());
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
    settings.setValue(QLatin1String("showSidebar"), showSidebar->isChecked());
    settings.setValue(QLatin1String("sidebarDockArea"),
                      sidebarDockArea->currentIndex() == 1
                      ? int(Qt::RightDockWidgetArea)
                      : int(Qt::LeftDockWidgetArea));
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
    settings.setValue(QLatin1String("selectionSearchInBackground"),
                      selectionSearchBackgroundCheck->isChecked());
    settings.endGroup();

    // SRCH04: dedicated search-field display/behavior switches.
    settings.beginGroup(QLatin1String("toolbarsearch"));
    settings.setValue(QLatin1String("showEngineNickname"),
                      showEngineNicknameCheck->isChecked());
    settings.setValue(QLatin1String("showEngineIcon"),
                      showEngineIconCheck->isChecked());
    settings.setValue(QLatin1String("alwaysNewTab"),
                      alwaysNewTabCheck->isChecked());
    settings.setValue(QLatin1String("keepTypedText"),
                      keepTypedTextCheck->isChecked());
    settings.endGroup();

    // SRCH06: scoped-completion shortcut nicknames — tokens cannot
    // contain whitespace (the first space splits the token from the
    // search term); an empty field restores the scope's default.
    const auto saveShortcut = [](ScopeShortcuts::Scope scope,
                                 QCheckBox *check, QLineEdit *edit) {
        ScopeShortcuts::setEnabled(scope, check->isChecked());
        QString token = ScopeShortcuts::sanitizeToken(edit->text());
        if (token.isEmpty())
            token = ScopeShortcuts::defaultToken(scope);
        ScopeShortcuts::setToken(scope, token);
    };
    saveShortcut(ScopeShortcuts::BookmarksScope,
                 shortcutBookmarksCheck, shortcutBookmarksToken);
    saveShortcut(ScopeShortcuts::HistoryScope,
                 shortcutHistoryCheck, shortcutHistoryToken);
    saveShortcut(ScopeShortcuts::TabsScope,
                 shortcutTabsCheck, shortcutTabsToken);

    // Appearance
    settings.beginGroup(QLatin1String("websettings"));
    settings.setValue(QLatin1String("fixedFont"), m_fixedFont);
    settings.setValue(QLatin1String("standardFont"), m_standardFont);

    settings.setValue(QLatin1String("blockPopupWindows"), blockPopupWindows->isChecked());
    settings.setValue(QLatin1String("enableJavascript"), enableJavascript->isChecked());
    settings.setValue(QLatin1String("enablePlugins"), enablePlugins->isChecked());
    settings.setValue(QLatin1String("enableImages"), enableImages->isChecked());
    settings.setValue(QLatin1String("enableLocalStorage"), enableLocalStorage->isChecked());
    settings.setValue(QLatin1String("forceDarkMode"), forceDarkMode->isChecked());
    // Latched by applyChromiumFlags() at engine startup — the tooltip
    // on the checkbox already tells the user it needs a restart.
    settings.setValue(QLatin1String("middleClickAutoscroll"), middleClickAutoscroll->isChecked());
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
    settings.setValue(QLatin1String("httpsOnly"), httpsOnly->isChecked());
    settings.setValue(QLatin1String("refererPolicy"), refererPolicy->currentIndex());
    // The PRIV01 bool stays in sync so an older build maps "Chromium
    // default" to off and everything else to its trimmed behavior.
    settings.setValue(QLatin1String("trimReferer"), refererPolicy->currentIndex() != 0);
    settings.setValue(QLatin1String("webrtcIpProtection"), webrtcProtection->isChecked());
    settings.setValue(QLatin1String("secureDnsMode"), secureDnsMode->currentIndex());
    settings.setValue(QLatin1String("secureDnsServer"), secureDnsServer->text().trimmed());
    // The PRIV01 bool is kept in sync so an older build reading it
    // lands on "automatic" rather than "off".
    settings.setValue(QLatin1String("secureDns"), secureDnsMode->currentIndex() != 0);
    settings.setValue(QLatin1String("tlsStrictCiphers"), strictTlsCiphers->isChecked());
    settings.setValue(QLatin1String("blockPings"), blockPings->isChecked());
    settings.setValue(QLatin1String("blockPrefetch"), blockPrefetch->isChecked());
    settings.setValue(QLatin1String("dnsPrefetch"), dnsPrefetch->isChecked());
    settings.setValue(QLatin1String("blockRemoteFonts"), blockRemoteFonts->isChecked());
    settings.setValue(QLatin1String("blockThirdPartyWebSockets"), blockThirdPartyWebSockets->isChecked());
    settings.setValue(QLatin1String("clearOnExit"), clearOnExit->isChecked());
    settings.setValue(QLatin1String("reportUtcTimezone"), reportUtcTimezone->isChecked());
    settings.setValue(QLatin1String("normalizeAcceptLanguage"), normalizeAcceptLanguage->isChecked());
    settings.setValue(QLatin1String("fingerprintProtection"), fingerprintProtection->isChecked());
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
        // Capture every combo pick up front — the manager writes below
        // emit signals that synchronously refreshSearchEngines(), so
        // reading the combos late loses the unsaved selection.
        const QString engineName = defaultEngineCombo->currentText();
        const QString privatePick =
            privateEngineCombo->currentData().toString();
        const QString imagePick =
            imageEngineCombo->currentData().toString();
        stashSearchSuggestions();
        for (auto it = m_pendingSuggestions.cbegin();
             it != m_pendingSuggestions.cend(); ++it) {
            if (searchManager->engineExists(it.key()))
                searchManager->setSuggestionsEnabledForEngine(it.key(), it.value());
        }
        m_pendingSuggestions.clear();
        if (searchManager->engineExists(engineName))
            searchManager->setCurrentEngineName(engineName);
        m_engineComboDirty = false;

        // SRCH04: context engine assignments — the combos' first row
        // ("Same as Default") carries empty data, clearing the pick.
        // setKeepFieldEngine runs first: turning it off drops the
        // stored field engine before the other writes land.
        searchManager->setKeepFieldEngine(keepFieldEngineCheck->isChecked());
        searchManager->setPrivateEngineName(privatePick);
        searchManager->setImageEngineName(imagePick);
        searchManager->setSuggestionsInAddressField(
            suggestInAddressFieldCheck->isChecked());
        searchManager->setSuggestionsInSearchField(
            suggestInSearchFieldCheck->isChecked());
        searchManager->setSuggestionsOnlyWithKeyword(
            suggestOnlyWithKeywordCheck->isChecked());
        m_privateEngineComboDirty = false;
        m_imageEngineComboDirty = false;
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
    settings.setValue(QLatin1String("suspendTabs"), suspendTabs->isChecked());
    settings.setValue(QLatin1String("suspendTabsMinutes"), suspendTabsMinutes->value());
    settings.setValue(QLatin1String("openTargetBlankLinksIn"), openTargetBlankLinksIn->currentIndex());
    settings.setValue(QLatin1String("openLinksFromAppsIn"), openLinksFromAppsIn->currentIndex());
    settings.endGroup();

    settings.beginGroup(QLatin1String("autofill"));
    settings.setValue(QLatin1String("passwordForms"), autoFillPasswordFormsCheckBox->isChecked());
    settings.endGroup();

    // EXT03: opt-in background update check — applies immediately;
    // the next scheduled slot reads the stored value.
    ExtensionManager::setUpdateCheckEnabled(
        extensionAutoUpdateCheck->isChecked());

    // Re-apply: the profile-level settings (was
    // BrowserApplication::loadSettings()), then each live manager.
    // The off-the-record profile gets the same treatment when it
    // exists — private browsing keeps user preferences.
    BrowserProfile::applySettings(BrowserProfile::normalProfile());
    if (QWebEngineProfile *otrProfile = BrowserProfile::privateProfileIfCreated())
        BrowserProfile::applySettings(otrProfile);
    // CONT01: materialized container profiles too — a settings change
    // reaches them without a restart just like the normal profile.
    ContainerManager::instance()->reapplySettings();
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
        else if (BrowserMainWindow *window = qobject_cast<BrowserMainWindow*>(widget)) {
            window->applySearchBoxVisibility();
            window->applySidebarSettings();
        }
    }
}

void SettingsDialog::accept()
{
    // SRCH05: flush a still-focused engine field before persisting.
    commitEngineEdits();
    saveToSettings();
    emit closeRequested();
}

void SettingsDialog::reject()
{
    // Discard: pending widget state dies when the host closes the tab
    // (or standalone window) in response — same as the dialog's Cancel.
    emit closeRequested();
}

// static
void SettingsDialog::openPage(QWidget *context, Page page)
{
    if (BrowserMainWindow *window = BrowserMainWindow::parentWindow(context)) {
        window->showSettingsPage(int(page));
        return;
    }
    // No window to host the tab — show the page as a standalone
    // modeless window (autotests, widgets with no browser ancestor).
    SettingsDialog *dialog = new SettingsDialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &SettingsDialog::closeRequested,
                     dialog, &QWidget::close);
    dialog->openAtPage(page);
    dialog->show();
}

void SettingsDialog::resetSearchSettings()
{
    if (QMessageBox::question(this, tr("Reset Search Settings"),
            tr("Reset the search engine choices, suggestion switches, "
               "shortcut nicknames and search display options to their "
               "defaults?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;

    // SRCH06: clear every key the Search page owns — nothing on the
    // other pages is touched.
    commitEngineEdits();
    m_pendingSuggestions.clear();
    m_suggestionsEngine.clear();
    m_engineComboDirty = false;
    m_privateEngineComboDirty = false;
    m_imageEngineComboDirty = false;

    QSettings settings;
    settings.beginGroup(QLatin1String("urlloading"));
    settings.remove(QLatin1String("searchEngineFallback"));
    settings.remove(QLatin1String("selectionSearchInBackground"));
    settings.endGroup();
    settings.beginGroup(QLatin1String("toolbarsearch"));
    settings.remove(QLatin1String("showEngineNickname"));
    settings.remove(QLatin1String("showEngineIcon"));
    settings.remove(QLatin1String("alwaysNewTab"));
    settings.remove(QLatin1String("keepTypedText"));
    settings.endGroup();
    settings.beginGroup(QLatin1String("MainWindow"));
    settings.remove(QLatin1String("showSearchBox"));
    settings.endGroup();

    ScopeShortcuts::reset();
    ToolbarSearch::openSearchManager()->resetSearchPreferences();

    // Widgets re-read the (now absent) keys as their compiled
    // defaults; saving immediately makes the reset take effect, like
    // the engine editor's own Restore Defaults.
    loadFromSettings();
    saveToSettings();
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

// Serializes an OpenSearchEngine::Parameters list into the editor's
// "name=value&name2=value2" text form.
static QString engineParametersText(const OpenSearchEngine::Parameters &parameters)
{
    QStringList parts;
    parts.reserve(parameters.size());
    for (const OpenSearchEngine::Parameter &param : parameters)
        parts << param.first + QLatin1Char('=') + param.second;
    return parts.join(QLatin1Char('&'));
}

// Parses the "name=value&name2=value2" text form back into Parameters.
// A pair without '=' or with an empty name is rejected.
static bool parseEngineParameters(const QString &text,
                                  OpenSearchEngine::Parameters *out)
{
    out->clear();
    const QStringList pairs = text.split(QLatin1Char('&'), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq <= 0)
            return false;
        out->append(OpenSearchEngine::Parameter(pair.left(eq).trimmed(),
                                                pair.mid(eq + 1).trimmed()));
    }
    return true;
}

// SRCH05: rebuild the inline editor's engine list from the manager.
// The selected row survives refreshes (manager edits emit changed());
// when the row vanished — a remove — the first row takes over.  The
// badges column shows the engine's [IMAGE] capability and the
// [PRIVATE] assignment.
void SettingsDialog::refreshEngineList()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    QString wanted = engineTree->currentItem()
        ? engineTree->currentItem()->data(0, Qt::UserRole).toString()
        : QString();
    if (wanted.isEmpty() || !manager->engineExists(wanted))
        wanted = m_editEngineName;

    const QSignalBlocker blocker(engineTree);
    engineTree->clear();
    const QStringList names = manager->allEnginesNames();
    for (const QString &name : names) {
        OpenSearchEngine *engine = manager->engine(name);
        if (!engine)
            continue;
        QTreeWidgetItem *item = new QTreeWidgetItem(engineTree);
        item->setText(0, name);
        item->setData(0, Qt::UserRole, name);
        const QImage image = engine->image();
        item->setIcon(0, image.isNull()
            ? HistoryManager::instance()->icon(QUrl(engine->imageUrl()))
            : QIcon(QPixmap::fromImage(image)));
        item->setText(1, manager->keywordsForEngine(engine)
                         .join(QLatin1String(", ")));
        QStringList badges;
        if (engine->providesImageSearch())
            badges << QLatin1String("IMAGE");
        if (name == manager->privateEngineName())
            badges << QLatin1String("PRIVATE");
        item->setText(2, badges.join(QLatin1Char(' ')));
        if (name == wanted)
            engineTree->setCurrentItem(item);
    }
    if (!engineTree->currentItem() && engineTree->topLevelItemCount() > 0)
        engineTree->setCurrentItem(engineTree->topLevelItem(0));
    // Signals were blocked during the rebuild — sync the form side.
    engineSelectionChanged();
}

void SettingsDialog::engineSelectionChanged()
{
    // A click on another row commits the form's pending edits to the
    // engine it was showing (the commit itself may trigger a refresh).
    commitEngineEdits();
    QTreeWidgetItem *item = engineTree->currentItem();
    m_editEngineName = item
        ? item->data(0, Qt::UserRole).toString() : QString();
    populateEngineForm();
    updateEngineButtonStates();
}

void SettingsDialog::populateEngineForm()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = manager->engine(m_editEngineName);
    engineEditPane->setEnabled(engine != nullptr);
    m_populatingEngineForm = true;
    if (!engine) {
        engineNameEdit->clear();
        engineNicknameEdit->clear();
        engineUrlEdit->clear();
        engineSuggestUrlEdit->clear();
        engineImageUrlEdit->clear();
        enginePostParamsEdit->clear();
        engineImagePostParamsEdit->clear();
        engineDefaultCheck->setChecked(false);
        enginePrivateCheck->setChecked(false);
    } else {
        engineNameEdit->setText(engine->name());
        engineNicknameEdit->setText(manager->keywordsForEngine(engine)
                                    .join(QLatin1String(", ")));
        engineUrlEdit->setText(engine->searchUrlTemplate());
        engineSuggestUrlEdit->setText(engine->suggestionsUrlTemplate());
        engineImageUrlEdit->setText(engine->imageSearchUrlTemplate());
        enginePostParamsEdit->setText(
            engineParametersText(engine->searchParameters()));
        engineImagePostParamsEdit->setText(
            engineParametersText(engine->imageSearchParameters()));
        engineDefaultCheck->setChecked(
            manager->currentEngineName() == engine->name());
        enginePrivateCheck->setChecked(
            manager->privateEngineName() == engine->name());
    }
    m_engineFieldsDirty.clear();
    m_populatingEngineForm = false;
}

void SettingsDialog::updateEngineButtonStates()
{
    const int row = engineTree->indexOfTopLevelItem(engineTree->currentItem());
    const int count = engineTree->topLevelItemCount();
    engineUpButton->setEnabled(row > 0);
    engineDownButton->setEnabled(row >= 0 && row < count - 1);
    engineRemoveButton->setEnabled(row >= 0 && count > 1);
}

void SettingsDialog::engineAdd()
{
    commitEngineEdits();
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    QString name = tr("New Engine");
    for (int i = 2; manager->engineExists(name); ++i)
        name = tr("New Engine %1").arg(i);
    OpenSearchEngine *engine = new OpenSearchEngine();
    engine->setName(name);
    // Placeholder endpoint — the URL field is focused so the user
    // replaces it; the commit validation demands %s/{searchTerms}.
    engine->setSearchUrlTemplate(QLatin1String("https://"));
    if (!manager->addEngine(engine)) {
        delete engine;
        return;
    }
    // addEngine emitted changed() and the list refreshed; select the
    // new row explicitly and put focus where editing starts.
    for (int i = 0; i < engineTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = engineTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == name) {
            engineTree->setCurrentItem(item);
            break;
        }
    }
    engineNameEdit->setFocus();
    engineNameEdit->selectAll();
}

void SettingsDialog::engineRemove()
{
    QTreeWidgetItem *item = engineTree->currentItem();
    if (!item)
        return;
    ToolbarSearch::openSearchManager()->removeEngine(
        item->data(0, Qt::UserRole).toString());
    // removeEngine emits changed() — the list refresh selects the
    // first remaining row.
}

void SettingsDialog::engineMove(int offset)
{
    commitEngineEdits();
    QTreeWidgetItem *item = engineTree->currentItem();
    if (!item)
        return;
    ToolbarSearch::openSearchManager()->moveEngine(
        item->data(0, Qt::UserRole).toString(), offset);
    // changed() -> refreshEngineList keeps the moved row selected.
}

void SettingsDialog::engineRestoreDefaults()
{
    commitEngineEdits();
    ToolbarSearch::openSearchManager()->restoreDefaults();
}

// The default/private checkboxes write straight through to the
// manager — checking "Set as Default" makes that engine the default
// immediately (unchecking the current default is impossible, there is
// always one).
void SettingsDialog::engineAssignmentChanged()
{
    if (m_populatingEngineForm)
        return;
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = manager->engine(m_editEngineName);
    if (!engine)
        return;
    if (engineDefaultCheck->isChecked()
            && manager->currentEngineName() != engine->name()) {
        manager->setCurrentEngineName(engine->name());
    } else if (!engineDefaultCheck->isChecked()
               && manager->currentEngineName() == engine->name()) {
        engineDefaultCheck->setChecked(true);
    }
    if (enginePrivateCheck->isChecked()
            && manager->privateEngineName() != engine->name()) {
        manager->setPrivateEngineName(engine->name());
    } else if (!enginePrivateCheck->isChecked()
               && manager->privateEngineName() == engine->name()) {
        manager->setPrivateEngineName(QString());
    }
}

// Applies the fields the user touched to the engine the form was
// showing.  Values are snapshotted up front because applying an edit
// emits OpenSearchManager::changed(), whose synchronous refresh
// repopulates (and resets) the form fields.  Invalid input reverts
// that one field and is reported once, at the end.
void SettingsDialog::commitEngineEdits()
{
    if (m_editEngineName.isEmpty())
        return;
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = manager->engine(m_editEngineName);
    if (!engine) {
        m_engineFieldsDirty.clear();
        return;
    }
    const QSet<QString> dirty = m_engineFieldsDirty;
    m_engineFieldsDirty.clear();
    if (dirty.isEmpty())
        return;

    const QString wantedName = engineNameEdit->text().trimmed();
    const QString nicknames = engineNicknameEdit->text();
    QString url = engineUrlEdit->text().trimmed();
    QString suggestUrl = engineSuggestUrlEdit->text().trimmed();
    QString imageUrl = engineImageUrlEdit->text().trimmed();
    const QString postParams = enginePostParamsEdit->text().trimmed();
    const QString imagePostParams = engineImagePostParamsEdit->text().trimmed();

    bool edited = false;
    QStringList problems;

    if (dirty.contains(QLatin1String("nickname"))) {
        const QStringList keys = nicknames.split(
            QRegularExpression(QLatin1String("[ ,]+")), Qt::SkipEmptyParts);
        QString conflictName;
        for (const QString &key : keys) {
            OpenSearchEngine *holder = manager->engineForKeyword(key);
            if (holder && holder != engine) {
                conflictName = tr("The nickname \"%1\" is already used by \"%2\".")
                    .arg(key, holder->name());
                break;
            }
        }
        if (!conflictName.isEmpty()) {
            problems << conflictName;
            engineNicknameEdit->setText(manager->keywordsForEngine(engine)
                                        .join(QLatin1String(", ")));
        } else {
            manager->setKeywordsForEngine(engine, keys);
            edited = true;
        }
    }

    if (dirty.contains(QLatin1String("url"))) {
        url.replace(QLatin1String("%s"), QLatin1String("{searchTerms}"));
        if (url.isEmpty()) {
            problems << tr("The search URL cannot be empty.");
            engineUrlEdit->setText(engine->searchUrlTemplate());
        } else if (!url.contains(QLatin1String("{searchTerms}"))
                   && postParams.isEmpty()
                   && engine->searchParameters().isEmpty()) {
            problems << tr("The search URL must contain %s or "
                           "{searchTerms} where the search text is "
                           "inserted (or POST parameters that carry it).");
            engineUrlEdit->setText(engine->searchUrlTemplate());
        } else if (url != engine->searchUrlTemplate()) {
            engine->setSearchUrlTemplate(url);
            edited = true;
        }
    }

    if (dirty.contains(QLatin1String("suggest"))) {
        suggestUrl.replace(QLatin1String("%s"), QLatin1String("{searchTerms}"));
        if (!suggestUrl.isEmpty()
                && !suggestUrl.contains(QLatin1String("{searchTerms}"))) {
            problems << tr("The suggest URL must contain %s or "
                           "{searchTerms} where the search text is "
                           "inserted.");
            engineSuggestUrlEdit->setText(engine->suggestionsUrlTemplate());
        } else if (suggestUrl != engine->suggestionsUrlTemplate()) {
            engine->setSuggestionsUrlTemplate(suggestUrl);
            edited = true;
        }
    }

    if (dirty.contains(QLatin1String("image"))) {
        imageUrl.replace(QLatin1String("%s"), QLatin1String("{searchTerms}"));
        if (!imageUrl.isEmpty()
                && !imageUrl.contains(QLatin1String("{searchTerms}"))) {
            problems << tr("The image search URL must contain %s or "
                           "{searchTerms} where the search text is "
                           "inserted.");
            engineImageUrlEdit->setText(engine->imageSearchUrlTemplate());
        } else if (imageUrl != engine->imageSearchUrlTemplate()) {
            engine->setImageSearchUrlTemplate(imageUrl);
            edited = true;
        }
    }

    if (dirty.contains(QLatin1String("post"))) {
        OpenSearchEngine::Parameters params;
        if (!postParams.isEmpty()
                && !parseEngineParameters(postParams, &params)) {
            problems << tr("POST parameters must be name=value pairs "
                           "separated by '&'.");
            enginePostParamsEdit->setText(
                engineParametersText(engine->searchParameters()));
        } else {
            engine->setSearchParameters(params);
            engine->setSearchMethod(postParams.isEmpty()
                ? QLatin1String("get") : QLatin1String("post"));
            edited = true;
        }
    }

    if (dirty.contains(QLatin1String("imagepost"))) {
        OpenSearchEngine::Parameters params;
        if (!imagePostParams.isEmpty()
                && !parseEngineParameters(imagePostParams, &params)) {
            problems << tr("Image POST parameters must be name=value "
                           "pairs separated by '&'.");
            engineImagePostParamsEdit->setText(
                engineParametersText(engine->imageSearchParameters()));
        } else {
            engine->setImageSearchParameters(params);
            engine->setImageSearchMethod(imagePostParams.isEmpty()
                ? QLatin1String("get") : QLatin1String("post"));
            edited = true;
        }
    }

    if (edited)
        manager->engineEdited(engine);

    // The name commits last: it re-keys the manager, and
    // m_editEngineName must point at the new name before the emitted
    // refresh repopulates the form.
    if (dirty.contains(QLatin1String("name")) && wantedName != engine->name()) {
        if (wantedName.isEmpty()) {
            problems << tr("The engine name cannot be empty.");
            engineNameEdit->setText(engine->name());
        } else if (manager->engineExists(wantedName)) {
            problems << tr("An engine named \"%1\" already exists.")
                            .arg(wantedName);
            engineNameEdit->setText(engine->name());
        } else {
            const QString oldName = engine->name();
            m_editEngineName = wantedName;
            if (!manager->renameEngine(oldName, wantedName))
                m_editEngineName = oldName;
        }
    }

    if (!problems.isEmpty()) {
        // Resync the form — edits that applied already emitted their
        // own refresh, but a validation-only commit leaves stale text.
        populateEngineForm();
        QMessageBox::warning(this, tr("Search Engines"),
                             problems.join(QLatin1Char('\n')));
    }
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

    // SRCH04: the private and image pickers lead with a
    // "Same as Default" row whose empty itemData clears the
    // assignment.  The image list only offers engines that advertise
    // an image-search endpoint — others could never answer the
    // request anyway.
    const QString wantedPrivate = m_privateEngineComboDirty
        ? privateEngineCombo->currentData().toString()
        : manager->privateEngineName();
    const QString wantedImage = m_imageEngineComboDirty
        ? imageEngineCombo->currentData().toString()
        : manager->imageEngineName();

    privateEngineCombo->clear();
    privateEngineCombo->addItem(tr("Same as Default"), QString());
    for (const QString &name : names)
        privateEngineCombo->addItem(name, name);
    privateEngineCombo->setCurrentIndex(qMax(
        0, privateEngineCombo->findData(wantedPrivate)));

    imageEngineCombo->clear();
    imageEngineCombo->addItem(tr("Same as Default"), QString());
    for (const QString &name : names) {
        OpenSearchEngine *engine = manager->engine(name);
        if (engine && engine->providesImageSearch())
            imageEngineCombo->addItem(name, name);
    }
    imageEngineCombo->setCurrentIndex(qMax(
        0, imageEngineCombo->findData(wantedImage)));

    // SRCH05: the inline editor's list follows the same manager state.
    refreshEngineList();

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

void SettingsDialog::loadExtension()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, tr("Select Unpacked Extension Folder"), QDir::homePath());
    if (path.isEmpty())
        return;
    // EXT02: explicit consent with a permission review — nothing
    // loads silently, warnings or not.
    if (!ExtensionReviewDialog::review(
            ExtensionManager::inspectManifest(path), path,
            ExtensionReviewDialog::Load, this))
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
    if (!ExtensionReviewDialog::review(
            ExtensionManager::inspectManifest(path), path,
            ExtensionReviewDialog::Install, this))
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

void SettingsDialog::checkExtensionUpdates()
{
    ExtensionManager *extensions = ExtensionManager::instance();
    if (extensions->updateCheckInProgress())
        return;
    m_manualExtensionCheck = true;
    extensions->checkForUpdates(true);
}

void SettingsDialog::extensionUpdateCheckFinished(
    const QList<ExtensionManager::UpdateResult> &results)
{
    if (ExtensionManager::isSupported())
        extensionUpdateButton->setEnabled(true);
    extensionSelectionChanged();
    if (!m_manualExtensionCheck)
        return;
    m_manualExtensionCheck = false;

    QStringList installed;
    QStringList downloaded;
    QStringList failed;
    for (const ExtensionManager::UpdateResult &result : results) {
        if (!result.error.isEmpty()) {
            failed << tr("%1: %2").arg(result.name, result.error);
        } else if (!result.availableVersion.isEmpty()) {
            if (result.installTriggered)
                installed << tr("%1 (version %2)")
                    .arg(result.name, result.availableVersion);
            else
                downloaded << tr("%1 (version %2, saved to %3)")
                    .arg(result.name, result.availableVersion,
                         result.savedTo);
        }
    }

    QString text;
    if (installed.isEmpty() && downloaded.isEmpty() && failed.isEmpty())
        text = tr("All extensions are up to date.");
    else {
        QStringList parts;
        if (!installed.isEmpty())
            parts << tr("Updated: %1").arg(installed.join(QLatin1String(", ")));
        if (!downloaded.isEmpty())
            parts << tr("Update available, install manually: %1")
                         .arg(downloaded.join(QLatin1String(", ")));
        if (!failed.isEmpty())
            parts << tr("Failed: %1").arg(failed.join(QLatin1String(", ")));
        text = parts.join(QLatin1Char('\n'));
    }
    QMessageBox::information(this, tr("Extension Updates"), text);
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
        if (!info.path.isEmpty())
            lines << tr("Source: %1").arg(info.path);
        if (info.actionPopupUrl.isValid())
            lines << tr("Declares a toolbar action — Qt WebEngine has no "
                        "extension toolbar, so its popup will not appear.");
        if (!info.path.isEmpty()) {
            const ExtensionManager::Manifest manifest =
                ExtensionManager::inspectManifest(info.path);
            if (!manifest.version.isEmpty())
                lines << tr("Version %1").arg(manifest.version);
            // EXT03: update source + last check outcome.
            if (manifest.updateUrl.isEmpty())
                lines << tr("No update source — this extension cannot "
                            "check for updates.");
            else
                lines << tr("Update source: %1").arg(manifest.updateUrl);
            const ExtensionManager::UpdateResult update =
                ExtensionManager::instance()->updateResultFor(info.id);
            if (!update.error.isEmpty())
                lines << tr("Update check failed: %1").arg(update.error);
            else if (!update.availableVersion.isEmpty())
                lines << (update.installTriggered
                    ? tr("Update to version %1 was installed.")
                        .arg(update.availableVersion)
                    : tr("Update to version %1 downloaded to %2 — "
                         "install it with the Install button.")
                        .arg(update.availableVersion, update.savedTo));
            else if (update.upToDate)
                lines << tr("Extension is up to date.");
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

    // POPUP01: sites the user allowed pop-ups on share this audit
    // table too — key "popup|<host>".
    for (const QString &host : PopupBlocker::instance()->allowedHosts()) {
        QTreeWidgetItem *item = new QTreeWidgetItem(permissionsTree);
        item->setText(0, host);
        item->setText(1, tr("Pop-ups"));
        item->setText(2, tr("Allowed"));
        item->setData(0, Qt::UserRole,
                      QLatin1String("popup|") + host);
        if (item->data(0, Qt::UserRole).toString() == selectedKey)
            permissionsTree->setCurrentItem(item);
    }
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
    if (parts.at(0) == QLatin1String("popup")) {
        // POPUP01 row — drop the pop-up exception for the host.
        PopupBlocker::instance()->removeAllowedHost(parts.at(1));
        return;
    }
    WebPermissionManager::instance()->removeEntry(
        QUrl::fromEncoded(parts.at(0).toUtf8()),
        static_cast<QWebEnginePermission::PermissionType>(parts.at(1).toInt()));
}

void SettingsDialog::clearPermissions()
{
    ScriptControlManager *scripts = ScriptControlManager::instance();
    PopupBlocker *popups = PopupBlocker::instance();
    if (WebPermissionManager::instance()->entries().isEmpty()
        && scripts->allowedHosts().isEmpty()
        && scripts->blockedHosts().isEmpty()
        && popups->allowedHosts().isEmpty())
        return;
    if (QMessageBox::question(this, tr("Clear Site Permissions"),
            tr("Remove every remembered site permission?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
        return;
    WebPermissionManager::instance()->clearEntries();
    scripts->clearPersistentRules();
    popups->clearAllowedHosts();
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

// CONT03: the shared name + accent-color prompt behind the Containers
// page's New/Edit buttons — the palette is the fixed Firefox-style
// set ContainerManager rotates through.
static bool editContainer(QWidget *parent, const QString &title,
                          QString *name, QColor *color)
{
    static const char *const colorNames[] = {
        QT_TRANSLATE_NOOP("SettingsDialog", "Blue"),
        QT_TRANSLATE_NOOP("SettingsDialog", "Turquoise"),
        QT_TRANSLATE_NOOP("SettingsDialog", "Green"),
        QT_TRANSLATE_NOOP("SettingsDialog", "Yellow"),
        QT_TRANSLATE_NOOP("SettingsDialog", "Orange"),
        QT_TRANSLATE_NOOP("SettingsDialog", "Red"),
        QT_TRANSLATE_NOOP("SettingsDialog", "Pink"),
        QT_TRANSLATE_NOOP("SettingsDialog", "Purple"),
    };

    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    QFormLayout *layout = new QFormLayout(&dialog);

    QLineEdit *nameEdit = new QLineEdit(*name);
    nameEdit->setAccessibleName(SettingsDialog::tr("Container name"));
    layout->addRow(SettingsDialog::tr("Name:"), nameEdit);

    QComboBox *colorCombo = new QComboBox;
    const QList<QColor> palette = ContainerManager::defaultColors();
    const int colorNameCount = int(sizeof(colorNames) / sizeof(colorNames[0]));
    for (int i = 0; i < palette.count(); ++i) {
        colorCombo->addItem(ContainerManager::colorIcon(palette.at(i)),
            SettingsDialog::tr(colorNames[i % colorNameCount]),
            palette.at(i));
    }
    const int currentColor = colorCombo->findData(*color);
    if (currentColor >= 0)
        colorCombo->setCurrentIndex(currentColor);
    layout->addRow(SettingsDialog::tr("Color:"), colorCombo);

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted,
                     &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
                     &dialog, &QDialog::reject);
    // A container always needs a name — keep Ok dead until one exists.
    QPushButton *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(!nameEdit->text().trimmed().isEmpty());
    QObject::connect(nameEdit, &QLineEdit::textChanged, ok,
                     [ok](const QString &text) {
        ok->setEnabled(!text.trimmed().isEmpty());
    });

    if (dialog.exec() != QDialog::Accepted)
        return false;
    *name = nameEdit->text().trimmed();
    if (name->isEmpty())
        return false;
    *color = colorCombo->currentData().value<QColor>();
    return true;
}

QString SettingsDialog::selectedContainerId() const
{
    QListWidgetItem *item = containersList->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void SettingsDialog::refreshContainers()
{
    ContainerManager *manager = ContainerManager::instance();
    const QList<ContainerManager::Container> containers =
        manager->containers();
    // A tor process hands out no containers at all — report that
    // instead of offering controls that could only fail.
    const bool tor = BrowserApplication::isTorMode();

    const QString selectedId = selectedContainerId();
    containersList->clear();
    for (const ContainerManager::Container &container : containers) {
        QListWidgetItem *item = new QListWidgetItem(
            ContainerManager::colorIcon(container.color), container.name);
        item->setData(Qt::UserRole, container.id);
        item->setToolTip(container.name);
        containersList->addItem(item);
    }

    containersEmptyLabel->setText(tor
        ? tr("Containers are not available in a Tor window.")
        : tr("No containers yet. Create one to keep a site's data "
             "isolated from the rest of your browsing."));
    containersStack->setCurrentIndex(containers.isEmpty() || tor ? 0 : 1);
    containerNewButton->setEnabled(!tor);

    if (!selectedId.isEmpty()) {
        for (int i = 0; i < containersList->count(); ++i) {
            if (containersList->item(i)->data(Qt::UserRole).toString()
                == selectedId) {
                containersList->setCurrentRow(i);
                break;
            }
        }
    }
    containerSelectionChanged();
}

void SettingsDialog::containerSelectionChanged()
{
    const bool usable = !selectedContainerId().isEmpty()
        && !BrowserApplication::isTorMode();
    containerEditButton->setEnabled(usable);
    containerRemoveButton->setEnabled(usable);
    refreshContainerSites();
}

void SettingsDialog::refreshContainerSites()
{
    const QString selectedHost =
        containerSitesList->currentItem()
            ? containerSitesList->currentItem()->text() : QString();
    containerSitesList->clear();
    const QString id = selectedContainerId();
    const QStringList sites = id.isEmpty()
        ? QStringList()
        : ContainerManager::instance()->siteRules(id);
    containerSitesList->addItems(sites);
    if (!selectedHost.isEmpty()) {
        const QList<QListWidgetItem*> matches =
            containerSitesList->findItems(selectedHost,
                                          Qt::MatchExactly);
        if (!matches.isEmpty())
            containerSitesList->setCurrentItem(matches.first());
    }
    containerSiteRemoveButton->setEnabled(
        containerSitesList->currentItem() != nullptr);
    // The section explains itself when the container has no rules.
    containerSitesLabel->setText(id.isEmpty()
        ? tr("Sites that always open in the selected container:")
        : tr("Sites that always open in this container — assigned "
             "from a tab's context menu:"));
}

void SettingsDialog::containerSiteRemove()
{
    QListWidgetItem *item = containerSitesList->currentItem();
    if (!item)
        return;
    // siteRulesChanged repopulates the list.
    ContainerManager::instance()->removeSiteRule(item->text());
}

void SettingsDialog::containerNew()
{
    ContainerManager *manager = ContainerManager::instance();
    const QList<QColor> palette = ContainerManager::defaultColors();
    QString name;
    QColor color = palette.value(manager->containers().count()
                                 % palette.count());
    if (!editContainer(this, tr("New Container"), &name, &color))
        return;
    const QString id = manager->createContainer(name, color).id;
    // containersChanged refreshed the list — select the new row.
    for (int i = 0; i < containersList->count(); ++i) {
        if (containersList->item(i)->data(Qt::UserRole).toString() == id) {
            containersList->setCurrentRow(i);
            break;
        }
    }
}

void SettingsDialog::containerEdit()
{
    const QString id = selectedContainerId();
    if (id.isEmpty())
        return;
    ContainerManager *manager = ContainerManager::instance();
    const ContainerManager::Container container =
        manager->containerForId(id);
    QString name = container.name;
    QColor color = container.color;
    if (!editContainer(this, tr("Edit Container"), &name, &color))
        return;
    manager->renameContainer(id, name);
    manager->setContainerColor(id, color);
}

void SettingsDialog::containerRemove()
{
    const QString id = selectedContainerId();
    if (id.isEmpty())
        return;
    ContainerManager *manager = ContainerManager::instance();
    const QString name = manager->containerForId(id).name;

    // deleteContainer() requires every page on the container profile
    // closed first — count the live tabs across all windows so the
    // warning can name the collateral.
    int openTabs = 0;
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        const TabWidget *tabs = qobject_cast<TabWidget*>(widget);
        if (!tabs)
            continue;
        for (int i = 0; i < tabs->count(); ++i) {
            if (tabs->containerIdForTab(i) == id)
                ++openTabs;
        }
    }

    QString text = tr("Delete the container \"%1\"? All site data "
        "stored in it — cookies, logins, cache and site storage — "
        "will be permanently removed.").arg(name);
    if (openTabs > 0)
        text += QLatin1Char('\n') + tr("%1 open tab(s) use this "
            "container and will be closed.").arg(openTabs);
    QMessageBox box(QMessageBox::Warning, tr("Delete Container"), text,
                    QMessageBox::Yes | QMessageBox::No, this);
    // The container name is user text — render the message literally.
    box.setTextFormat(Qt::PlainText);
    box.setDefaultButton(QMessageBox::No);
    if (box.exec() != QMessageBox::Yes)
        return;

    // Close from the back of each tab widget so indices stay valid;
    // the manager's no-live-pages contract is then satisfied.
    for (QWidget *widget : widgets) {
        TabWidget *tabs = qobject_cast<TabWidget*>(widget);
        if (!tabs)
            continue;
        for (int i = tabs->count() - 1; i >= 0; --i) {
            if (tabs->containerIdForTab(i) == id)
                tabs->closeTab(i);
        }
    }
    manager->deleteContainer(id);
}
