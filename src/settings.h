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

#ifndef SETTINGS_H
#define SETTINGS_H

#include <qwidget.h>
#include <qhash.h>
#include <qlist.h>
#include <qset.h>
#include "extensionmanager.h"
#include "ui_settings.h"

class QTreeWidgetItem;
// PREFS01: despite the historical name this is a QWidget — the
// settings page is hosted inside a browser tab (see
// BrowserMainWindow::showSettingsPage), not exec()ed as a dialog.
// The OK/Cancel box survives inside the page: accept() applies and
// asks the host to close, reject()/tab-close discards — the same
// semantics the modal dialog had.
class SettingsDialog : public QWidget, public Ui_Settings
{
    Q_OBJECT

public:
    // Sidebar/stack page order — callers can deep-link a page (the
    // toolbar search menu opens the Search page directly).
    enum Page {
        GeneralPage = 0,
        SearchPage,
        AppearancePage,
        PrivacyPage,
        TabSettingsPage,
        ProxyPage,
        AutoFillPage,
        AdvancedPage,
        ExtensionsPage,
        ContainersPage,
        DownloadsPage
    };

    SettingsDialog(QWidget *parent = nullptr);
    void openAtPage(Page page);
    // Opens the settings page in the ancestor window's Preferences
    // tab; with no BrowserMainWindow ancestor (autotests, detached
    // widgets) it is shown as a standalone modeless window so the
    // deep link still lands somewhere.
    static void openPage(QWidget *context, Page page);
    // CMD01: display names for the sidebar pages, kept in enum order —
    // the command palette deep-links to sections without constructing
    // the dialog.  Must match settings.ui's pagesList order.
    static int pageCount();
    static QString pageTitle(Page page);

signals:
    // Emitted by accept() (after the settings were applied) and by
    // reject() — the host closes the tab (or standalone window) on it.
    // Closing the tab directly discards exactly like the old Cancel.
    void closeRequested();

public slots:
    void accept();
    void reject();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void loadDefaults();
    void loadFromSettings();
    void saveToSettings();

    void setHomeToCurrentPage();
    void showCookies();
    void showExceptions();

    void chooseDownloadDirectory();
    void chooseDownloadProgram();
    void chooseFont();
    void chooseFixedFont();
    void chooseAcceptLanguage();

    void chooseStyleSheet();
    void editAutoFillUser();

    void refreshSearchEngines();
    void refreshSearchSuggestions();
    // SRCH06: Search-page-owned settings back to compiled defaults.
    void resetSearchSettings();

    // SRCH05: inline search-engine editor (replaces the Manage dialog).
    void refreshEngineList();
    void engineSelectionChanged();
    void engineAdd();
    void engineRemove();
    void engineMove(int offset);
    void engineRestoreDefaults();
    void engineAssignmentChanged();
    void commitEngineEdits();

    void loadExtension();
    void installExtension();
    void removeExtension();
    void checkExtensionUpdates();
    void extensionUpdateCheckFinished(
        const QList<ExtensionManager::UpdateResult> &results);
    void extensionSelectionChanged();
    void extensionItemChanged(QTreeWidgetItem *item, int column);
    void extensionError(const QString &message);
    void refreshExtensions();
    void refreshUserScripts();
    void openUserScriptsFolder();
    void reloadUserScripts();

    void refreshPermissions();
    void permissionSelectionChanged();
    void removePermission();
    void clearPermissions();
    void updateSecurityLevelHint();

    void refreshCredentialUi();
    void credentialPassphraseChange();
    void credentialPassphraseRemove();
    void credentialStoreLock();

    // CONT03: the Containers page mirrors the ContainerManager
    // registry — add/edit/delete act on the shared manager
    // immediately, like the Search page's engine editor.
    void refreshContainers();
    void containerNew();
    void containerEdit();
    void containerRemove();
    void containerSelectionChanged();
    // CONT04: the per-container "always open here" site rules —
    // listed per selected container, removable from here or from the
    // tab context menu.
    void refreshContainerSites();
    void containerSiteRemove();

    // PREFUI03: the nav filter narrows the sidebar to pages whose
    // title or control labels match the typed terms.
    void filterPages(const QString &text);

private:
    void buildPageSearchIndex();
    void stashSearchSuggestions();
    void populateEngineForm();
    void updateEngineButtonStates();
    QString selectedContainerId() const;

    QFont m_standardFont;
    QFont m_fixedFont;
    QString m_suggestionsEngine;
    QHash<QString, bool> m_pendingSuggestions;
    bool m_engineComboDirty = false;
    // SRCH04: same unsaved-pick guard for the context combos.
    bool m_privateEngineComboDirty = false;
    bool m_imageEngineComboDirty = false;
    // SRCH05: engine the edit form currently shows + which of its
    // fields the user touched since the last populate.
    QString m_editEngineName;
    QSet<QString> m_engineFieldsDirty;
    bool m_populatingEngineForm = false;
    // EXT03: distinguishes the button-triggered check (summary box)
    // from the opt-in background check firing while the dialog is up.
    bool m_manualExtensionCheck = false;
    // PREFUI03: per-page lowercase search text, built lazily on the
    // first keystroke (after loadFromSettings populated the combos).
    QStringList m_pageSearchTexts;
};

#endif // SETTINGS_H

