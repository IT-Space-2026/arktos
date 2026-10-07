/*
 * Copyright (c) 2026, The Arora Authors
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

// COV04: SettingsDialog — every widget group is poked to a non-default
// value so saveToSettings() runs each branch, then a second dialog
// instance exercises loadFromSettings() reading them back.  The buttons
// that spawn sub-dialogs are clicked under acceptModal()/rejectModal()
// timers.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qwebengineprofile.h>

#include "settings.h"
#include "browserapplication.h"
#include "browserprofile.h"
#include "cookiejar.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "toolbarsearch.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_SettingsDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void constructDefaults();
    void saveAndReload();
    void searchTab();
    void searchContextControls();
    void suggestionsCheckbox();
    void engineEditor();
    void sidebarNavigation();
    void subDialogButtons();
    void setHomeToCurrentPage();
};

void tst_SettingsDialog::initTestCase()
{
    QCoreApplication::setApplicationName("tst_settingsdialog");
    QSettings settings;
    settings.clear();
}

void tst_SettingsDialog::constructDefaults()
{
    SettingsDialog dialog;
    QCOMPARE(dialog.homeLineEdit->text(), QLatin1String("about:home"));
    QVERIFY(dialog.buttonBox);
    QVERIFY(dialog.extensionsTree);
}

// Flip every group off its default and accept() so saveToSettings()
// serializes each branch.
void tst_SettingsDialog::saveAndReload()
{
    {
        SettingsDialog dialog;
        dialog.tabWidget->setCurrentIndex(2);
        dialog.homeLineEdit->setText(QLatin1String("http://home.example.com/"));
        dialog.startupBehavior->setCurrentIndex(1);
        dialog.showSearchBox->setChecked(true);
        // ICONS01: pick a non-native bundled set so the round-trip
        // proves the persisted id lands on the right combo row.
        const int tablerRow = dialog.iconThemeCombo->findData(
            QLatin1String("tabler"));
        QVERIFY(tablerRow >= 0);
        dialog.iconThemeCombo->setCurrentIndex(tablerRow);
        dialog.expireHistory->setCurrentIndex(2);       // 14 days
        dialog.searchEngineFallback->setChecked(true);
        dialog.downloadAsk->setChecked(true);
        dialog.downloadsLocation->setText(QDir::tempPath());
        dialog.externalDownloadButton->setChecked(true);
        dialog.externalDownloadPath->setText(QLatin1String("/bin/cat %1"));
        dialog.minimFontSizeCheckBox->setChecked(true);
        dialog.minimumFontSizeSpinBox->setValue(12);
        dialog.blockPopupWindows->setChecked(true);
        dialog.enableJavascript->setChecked(false);
        dialog.enableImages->setChecked(false);
        dialog.enableLocalStorage->setChecked(false);
        dialog.enablePlugins->setChecked(true);
        dialog.userStyleSheet->setText(
            QString::fromUtf8(QUrl::fromLocalFile(QLatin1String("/tmp/x.css")).toEncoded()));
        dialog.acceptCombo->setCurrentIndex(1);          // AcceptNever
        dialog.keepUntilCombo->setCurrentIndex(2);       // KeepUntilTimeLimit
        dialog.cookieSessionCombo->setCurrentIndex(3);   // 3 days
        dialog.filterTrackingCookiesCheckbox->setChecked(true);
        dialog.proxySupport->setChecked(true);
        dialog.proxyType->setCurrentIndex(1);
        dialog.proxyHostName->setText(QLatin1String("localhost"));
        dialog.proxyPort->setValue(8080);
        dialog.proxyUserName->setText(QLatin1String("u"));
        dialog.proxyPassword->setText(QLatin1String("p"));
        dialog.tabBarPosition->setCurrentIndex(2);   // Left
        dialog.selectTabsWhenCreated->setChecked(true);
        dialog.confirmClosingMultipleTabs->setChecked(false);
        dialog.oneCloseButton->setChecked(true);
        dialog.quitAsLastTabClosed->setChecked(false);
        dialog.openTargetBlankLinksIn->setCurrentIndex(0);
        dialog.openLinksFromAppsIn->setCurrentIndex(0);
        dialog.autoFillPasswordFormsCheckBox->setChecked(true);
        dialog.networkCache->setChecked(true);
        dialog.networkCacheMaximumSizeSpinBox->setValue(64);
        dialog.accept();
    }

    QSettings settings;
    QCOMPARE(settings.value(QLatin1String("MainWindow/home")).toString(),
             QLatin1String("http://home.example.com/"));
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSearchBox")).toBool(), true);
    QCOMPARE(settings.value(QLatin1String("MainWindow/iconTheme")).toString(),
             QLatin1String("tabler"));
    QCOMPARE(settings.value(QLatin1String("proxy/enabled")).toBool(), true);

    // A fresh dialog must read every value back through loadFromSettings.
    {
        SettingsDialog reloaded;
        QCOMPARE(reloaded.homeLineEdit->text(),
                 QLatin1String("http://home.example.com/"));
        QVERIFY(reloaded.showSearchBox->isChecked());
        QCOMPARE(reloaded.iconThemeCombo->currentData().toString(),
                 QLatin1String("tabler"));
        QCOMPARE(reloaded.expireHistory->currentIndex(), 2);
        QVERIFY(reloaded.searchEngineFallback->isChecked());
        QVERIFY(reloaded.proxySupport->isChecked());
        QCOMPARE(reloaded.proxyHostName->text(), QLatin1String("localhost"));
        QCOMPARE(reloaded.acceptCombo->currentIndex(), 1);
        QCOMPARE(reloaded.keepUntilCombo->currentIndex(), 2);
        QCOMPARE(reloaded.tabBarPosition->currentIndex(), 2);
        QVERIFY(reloaded.selectTabsWhenCreated->isChecked());
        QVERIFY(!reloaded.confirmClosingMultipleTabs->isChecked());
        QVERIFY(!reloaded.quitAsLastTabClosed->isChecked());
        QCOMPARE(reloaded.openTargetBlankLinksIn->currentIndex(), 0);
        QCOMPARE(reloaded.openLinksFromAppsIn->currentIndex(), 0);
        QVERIFY(reloaded.minimFontSizeCheckBox->isChecked());
        QCOMPARE(reloaded.minimumFontSizeSpinBox->value(), 12);
    }
}

// SRCH02: the Search tab's engine combo mirrors the OpenSearchManager
// list; the displayed engine becomes the default on accept().
void tst_SettingsDialog::searchTab()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    manager->restoreDefaults();
    const QString original = manager->currentEngineName();
    const QStringList engines = manager->allEnginesNames();
    QVERIFY(engines.count() >= 2);

    // The combo holds exactly the manager's engine list and starts on
    // the current engine.
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.defaultEngineCombo->count(), engines.count());
        QSet<QString> comboNames;
        for (int i = 0; i < dialog.defaultEngineCombo->count(); ++i)
            comboNames.insert(dialog.defaultEngineCombo->itemText(i));
        QCOMPARE(comboNames, QSet<QString>(engines.begin(), engines.end()));
        QCOMPARE(dialog.defaultEngineCombo->currentText(), original);
    }

    // Picking a different engine applies on accept() and a reopened
    // dialog shows it.
    const QString other = engines.first() == original
        ? engines.at(1) : engines.first();
    {
        SettingsDialog dialog;
        dialog.defaultEngineCombo->setCurrentText(other);
        dialog.accept();
        QCOMPARE(manager->currentEngineName(), other);
    }
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.defaultEngineCombo->currentText(), other);
    }
    manager->setCurrentEngineName(original);
}

// SRCH04: the Search page's context pickers — private window and
// image search — mirror the OpenSearchManager assignments, lead with
// "Same as Default", and the image list only offers engines that
// advertise an image-search endpoint.  The grouped checkboxes persist
// through save/reload.
void tst_SettingsDialog::searchContextControls()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    manager->restoreDefaults();

    const QStringList engines = manager->allEnginesNames();
    QString imageCapable;
    bool hasImageless = false;
    for (const QString &name : engines) {
        if (manager->engine(name)->providesImageSearch()
            && imageCapable.isEmpty())
            imageCapable = name;
        if (!manager->engine(name)->providesImageSearch())
            hasImageless = true;
    }
    QVERIFY(!imageCapable.isEmpty());
    QVERIFY(hasImageless);

    // Combos: private lists every engine; image lists only capable
    // ones; both start on "Same as Default" when nothing is set.
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.privateEngineCombo->itemText(0),
                 QStringLiteral("Same as Default"));
        QCOMPARE(dialog.privateEngineCombo->count(), engines.count() + 1);
        QVERIFY(dialog.imageEngineCombo->count() < engines.count() + 1);
        for (int i = 1; i < dialog.imageEngineCombo->count(); ++i) {
            const QString name = dialog.imageEngineCombo->itemData(i).toString();
            QVERIFY(manager->engine(name)->providesImageSearch());
        }

        dialog.privateEngineCombo->setCurrentIndex(
            dialog.privateEngineCombo->findData(imageCapable));
        dialog.imageEngineCombo->setCurrentIndex(
            dialog.imageEngineCombo->findData(imageCapable));

        // Flip the grouped toggles to non-default states.
        dialog.searchButtonRadio->setChecked(true);
        QVERIFY(!dialog.showSearchBox->isChecked());
        dialog.showEngineNicknameCheck->setChecked(false);
        dialog.alwaysNewTabCheck->setChecked(true);
        dialog.keepFieldEngineCheck->setChecked(false);
        dialog.keepTypedTextCheck->setChecked(false);
        dialog.selectionSearchBackgroundCheck->setChecked(true);
        dialog.suggestInAddressFieldCheck->setChecked(false);
        dialog.suggestOnlyWithKeywordCheck->setChecked(true);
        dialog.accept();
    }

    QCOMPARE(manager->privateEngineName(), imageCapable);
    QCOMPARE(manager->imageEngineName(), imageCapable);
    QCOMPARE(manager->imageSearchEngine(), manager->engine(imageCapable));
    QVERIFY(!manager->keepFieldEngine());
    QVERIFY(!manager->suggestionsInAddressField());
    QVERIFY(manager->suggestionsOnlyWithKeyword());

    QSettings settings;
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSearchBox")).toBool(),
             false);
    QCOMPARE(settings.value(QLatin1String("toolbarsearch/alwaysNewTab")).toBool(),
             true);
    QCOMPARE(settings.value(QLatin1String("toolbarsearch/keepTypedText")).toBool(),
             false);
    QCOMPARE(settings.value(
                 QLatin1String("urlloading/selectionSearchInBackground"))
                 .toBool(),
             true);

    // A reopened dialog reflects the stored state everywhere.
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.privateEngineCombo->currentData().toString(),
                 imageCapable);
        QCOMPARE(dialog.imageEngineCombo->currentData().toString(),
                 imageCapable);
        QVERIFY(dialog.searchButtonRadio->isChecked());
        QVERIFY(!dialog.showEngineNicknameCheck->isChecked());
        QVERIFY(dialog.alwaysNewTabCheck->isChecked());
        QVERIFY(!dialog.keepFieldEngineCheck->isChecked());
        QVERIFY(dialog.selectionSearchBackgroundCheck->isChecked());
        QVERIFY(!dialog.suggestInAddressFieldCheck->isChecked());
        QVERIFY(dialog.suggestOnlyWithKeywordCheck->isChecked());

        // Back to defaults for the rest of the suite.
        dialog.searchFieldRadio->setChecked(true);
        dialog.privateEngineCombo->setCurrentIndex(0);
        dialog.imageEngineCombo->setCurrentIndex(0);
        dialog.showEngineNicknameCheck->setChecked(true);
        dialog.alwaysNewTabCheck->setChecked(false);
        dialog.keepFieldEngineCheck->setChecked(true);
        dialog.keepTypedTextCheck->setChecked(true);
        dialog.selectionSearchBackgroundCheck->setChecked(false);
        dialog.suggestInAddressFieldCheck->setChecked(true);
        dialog.suggestOnlyWithKeywordCheck->setChecked(false);
        dialog.accept();
    }
    QCOMPARE(manager->privateEngineName(), QString());
    QCOMPARE(manager->imageEngineName(), QString());
    QVERIFY(manager->keepFieldEngine());
    QVERIFY(manager->suggestionsInAddressField());
    QVERIFY(!manager->suggestionsOnlyWithKeyword());
}

// SEC11/SRCH02: the Search tab's Search Suggestions checkbox is bound
// to the per-engine opt-in of whichever engine the combo shows.
void tst_SettingsDialog::suggestionsCheckbox()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    manager->restoreDefaults();

    // The checkbox only makes sense for an engine with a suggest
    // endpoint — pick one (bundled Google/Wikipedia have them).
    OpenSearchEngine *engine = manager->currentEngine();
    if (!engine || !engine->providesSuggestions()) {
        for (const QString &name : manager->allEnginesNames()) {
            if (manager->engine(name)->providesSuggestions()) {
                manager->setCurrentEngineName(name);
                break;
            }
        }
        engine = manager->currentEngine();
    }
    QVERIFY(engine);
    QVERIFY(engine->providesSuggestions());
    QVERIFY(!manager->suggestionsEnabledForEngine(engine->name()));

    {
        SettingsDialog dialog;
        QVERIFY(dialog.searchSuggestionsCheckBox->isEnabled());
        QVERIFY(!dialog.searchSuggestionsCheckBox->isChecked());
        dialog.searchSuggestionsCheckBox->setChecked(true);
        dialog.accept();
    }
    QVERIFY(manager->suggestionsEnabledForEngine(engine->name()));

    {
        SettingsDialog dialog;
        QVERIFY(dialog.searchSuggestionsCheckBox->isChecked());
        dialog.searchSuggestionsCheckBox->setChecked(false);
        dialog.accept();
    }
    QVERIFY(!manager->suggestionsEnabledForEngine(engine->name()));

    // The checkbox follows the combo: toggling while a different
    // capable engine is displayed writes THAT engine's opt-in on
    // accept, not the current engine's.
    QString otherCapable;
    for (const QString &name : manager->allEnginesNames()) {
        if (name != engine->name()
            && manager->engine(name)->providesSuggestions()) {
            otherCapable = name;
            break;
        }
    }
    if (!otherCapable.isEmpty()) {
        SettingsDialog dialog;
        dialog.defaultEngineCombo->setCurrentText(otherCapable);
        QVERIFY(dialog.searchSuggestionsCheckBox->isEnabled());
        QVERIFY(!dialog.searchSuggestionsCheckBox->isChecked());
        dialog.searchSuggestionsCheckBox->setChecked(true);
        dialog.accept();
        QVERIFY(manager->suggestionsEnabledForEngine(otherCapable));
        QVERIFY(!manager->suggestionsEnabledForEngine(engine->name()));
        manager->setSuggestionsEnabledForEngine(otherCapable, false);
        manager->setCurrentEngineName(engine->name());
    }
}

// SRCH05: the Search page's inline engine editor — the tree mirrors
// the manager's engine order with [IMAGE]/[PRIVATE] badges, the form
// commits field edits on editingFinished, the checkboxes bind the
// default/private assignments, and the buttons add/rename/reorder/
// remove engines straight on the shared manager.
void tst_SettingsDialog::engineEditor()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    manager->restoreDefaults();
    const QStringList engines = manager->allEnginesNames();
    QVERIFY(engines.count() >= 3);

    QString imageCapable;
    for (const QString &name : engines) {
        if (manager->engine(name)->providesImageSearch()) {
            imageCapable = name;
            break;
        }
    }
    QVERIFY(!imageCapable.isEmpty());

    {
        SettingsDialog dialog;
        QCOMPARE(dialog.engineTree->topLevelItemCount(), engines.count());
        for (int i = 0; i < engines.count(); ++i)
            QCOMPARE(dialog.engineTree->topLevelItem(i)
                         ->data(0, Qt::UserRole).toString(), engines.at(i));

        // Badges: [IMAGE] on capable engines, [PRIVATE] on the
        // private pick — assigning through the manager updates them.
        manager->setPrivateEngineName(imageCapable);
        for (int i = 0; i < dialog.engineTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = dialog.engineTree->topLevelItem(i);
            const QString name = item->data(0, Qt::UserRole).toString();
            QCOMPARE(item->text(2).contains(QLatin1String("IMAGE")),
                     manager->engine(name)->providesImageSearch());
            QCOMPARE(item->text(2).contains(QLatin1String("PRIVATE")),
                     name == imageCapable);
        }
        manager->setPrivateEngineName(QString());

        // Selecting a row populates the form with that engine.
        QTreeWidgetItem *item = dialog.engineTree->topLevelItem(1);
        dialog.engineTree->setCurrentItem(item);
        const QString name = item->data(0, Qt::UserRole).toString();
        OpenSearchEngine *engine = manager->engine(name);
        QCOMPARE(dialog.engineNameEdit->text(), name);
        QCOMPARE(dialog.engineUrlEdit->text(), engine->searchUrlTemplate());
        QCOMPARE(dialog.engineDefaultCheck->isChecked(),
                 manager->currentEngineName() == name);
        QVERIFY(!dialog.engineDefaultCheck->isChecked()
                || manager->currentEngineName() == name);

        // Field edits commit on editingFinished; %s normalizes to
        // {searchTerms}.
        dialog.engineUrlEdit->setText(QLatin1String("https://e.org/s?q=%s"));
        QTest::keyClick(dialog.engineUrlEdit, Qt::Key_Return);
        QCOMPARE(engine->searchUrlTemplate(),
                 QLatin1String("https://e.org/s?q={searchTerms}"));

        // Nicknames write keyword bindings on the manager.
        dialog.engineNicknameEdit->setText(QLatin1String("zzq"));
        QTest::keyClick(dialog.engineNicknameEdit, Qt::Key_Return);
        QCOMPARE(manager->keywordsForEngine(engine),
                 QStringList() << QLatin1String("zzq"));
        QCOMPARE(manager->engineForKeyword(QLatin1String("zzq")), engine);
        manager->setKeywordsForEngine(engine, QStringList());

        // The checkboxes write the default/private assignments through.
        if (manager->currentEngineName() != name) {
            dialog.engineDefaultCheck->setChecked(true);
            QCOMPARE(manager->currentEngineName(), name);
        }
        dialog.enginePrivateCheck->setChecked(true);
        QCOMPARE(manager->privateEngineName(), name);
        dialog.enginePrivateCheck->setChecked(false);
        QCOMPARE(manager->privateEngineName(), QString());
    }
    manager->setCurrentEngineName(engines.at(0));

    // Add produces a row and selects it; edits to it (incl. a rename
    // and POST parameters) apply, up/down reorder, remove drops it.
    const int count = manager->enginesCount();
    {
        SettingsDialog dialog;
        dialog.engineAddButton->click();
        QCOMPARE(manager->enginesCount(), count + 1);
        const QString added = dialog.engineTree->currentItem()
            ->data(0, Qt::UserRole).toString();
        QVERIFY(added.startsWith(QLatin1String("New Engine")));
        QCOMPARE(manager->allEnginesNames().last(), added);
        QVERIFY(dialog.engineNameEdit->hasFocus()
                || dialog.engineNameEdit->text() == added);

        // POST parameters parse into the engine's parameter list and
        // flip its method.
        OpenSearchEngine *addedEngine = manager->engine(added);
        dialog.enginePostParamsEdit->setText(
            QLatin1String("q={searchTerms}&hl=en"));
        QTest::keyClick(dialog.enginePostParamsEdit, Qt::Key_Return);
        QCOMPARE(addedEngine->searchMethod(), QLatin1String("post"));
        QCOMPARE(addedEngine->searchParameters().count(), 2);
        QCOMPARE(addedEngine->searchParameters().at(0).first,
                 QLatin1String("q"));

        // Renaming through the Name field re-keys the manager.
        dialog.engineNameEdit->setText(QLatin1String("Renamed Engine"));
        QTest::keyClick(dialog.engineNameEdit, Qt::Key_Return);
        QVERIFY(!manager->engineExists(added));
        QVERIFY(manager->engineExists(QLatin1String("Renamed Engine")));
        QCOMPARE(manager->allEnginesNames().last(),
                 QLatin1String("Renamed Engine"));

        // Up/down reorder the manager's list.
        const QString before = manager->allEnginesNames().at(count - 1);
        dialog.engineUpButton->click();
        QCOMPARE(manager->allEnginesNames().at(count - 1),
                 QLatin1String("Renamed Engine"));
        QCOMPARE(manager->allEnginesNames().last(), before);
        dialog.engineDownButton->click();
        QCOMPARE(manager->allEnginesNames().last(),
                 QLatin1String("Renamed Engine"));

        dialog.engineRemoveButton->click();
        QCOMPARE(manager->enginesCount(), count);
        QVERIFY(!manager->engineExists(QLatin1String("Renamed Engine")));
    }
    manager->restoreDefaults();
}

// UIP03: the sidebar list and the page stack stay in sync both ways,
// every page is reachable, and the persisted currentTab index keeps
// the old tab order across a save/reload.
void tst_SettingsDialog::sidebarNavigation()
{
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.pagesList->count(), dialog.tabWidget->count());
        QCOMPARE(dialog.pagesList->count(), 9);
        for (int row = 0; row < dialog.pagesList->count(); ++row) {
            dialog.pagesList->setCurrentRow(row);
            QCOMPARE(dialog.tabWidget->currentIndex(), row);
            QCOMPARE(dialog.tabWidget->currentWidget(),
                     dialog.tabWidget->widget(row));
        }
        // Stack -> list direction: a programmatic page change moves
        // the sidebar highlight with it.
        dialog.tabWidget->setCurrentIndex(0);
        QCOMPARE(dialog.pagesList->currentRow(), 0);
        // Row labels carried over from the old tab captions.
        QCOMPARE(dialog.pagesList->item(0)->text(),
                 QStringLiteral("General"));
        QCOMPARE(dialog.pagesList->item(8)->text(),
                 QStringLiteral("Extensions"));
    }

    // The persisted currentTab round-trips through the sidebar.
    {
        SettingsDialog dialog;
        dialog.pagesList->setCurrentRow(4);
        dialog.accept();
    }
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.tabWidget->currentIndex(), 4);
        QCOMPARE(dialog.pagesList->currentRow(), 4);
        dialog.pagesList->setCurrentRow(0);
        dialog.accept();
    }
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.tabWidget->currentIndex(), 0);
        QCOMPARE(dialog.pagesList->currentRow(), 0);
    }
}

// Each button that pops a nested dialog runs it under a timer that
// dismisses the modal — the coverage is in the slot wiring.
void tst_SettingsDialog::subDialogButtons()
{
    SettingsDialog dialog;

    acceptModal();
    dialog.cookiesButton->click();

    acceptModal();
    dialog.exceptionsButton->click();

    acceptModal();
    dialog.languageButton->click();

    rejectModal(); // QFontDialog::getFont — cancel keeps m_standardFont
    dialog.standardFontButton->click();
    rejectModal();
    dialog.fixedFontButton->click();

    rejectModal(); // QFileDialog::getExistingDirectory
    dialog.downloadDirectoryButton->click();
    rejectModal();
    dialog.externalDownloadBrowse->click();
    rejectModal();
    dialog.styleSheetBrowseButton->click();

    acceptModal(); // AutoFillDialog
    dialog.editAutoFillUserButton->click();

    if (dialog.extensionLoadButton->isEnabled()) {
        rejectModal(); // QFileDialog::getExistingDirectory
        dialog.extensionLoadButton->click();
    }

    dialog.userScriptsReloadButton->click();
    dialog.userScriptsOpenButton->click(); // QDesktopServices — no-op offscreen

    // Selection-changed on an empty extension list clears details.
    dialog.extensionsTree->clear();
    dialog.extensionsTree->clearSelection();
}

// setHomeToCurrentPage finds the visible WebView under the parent
// window; without one it is an early return.
void tst_SettingsDialog::setHomeToCurrentPage()
{
    {
        SettingsDialog orphan;
        const QString before = orphan.homeLineEdit->text();
        orphan.setHomeToCurrentPageButton->click(); // no parent: returns
        QCOMPARE(orphan.homeLineEdit->text(), before);
    }

    QWidget window;
    WebView *view = new WebView(BrowserApplication::webEngineProfile(), &window);
    view->loadUrl(QUrl(QLatin1String("data:text/html,<p>home</p>")));
    QTRY_VERIFY_WITH_TIMEOUT(
        !QString::fromUtf8(view->url().toEncoded()).isEmpty(), 15000);

    SettingsDialog dialog(&window);
    dialog.setHomeToCurrentPageButton->click();
    QVERIFY(dialog.homeLineEdit->text().startsWith(QLatin1String("data:text/html")));
}

QTEST_MAIN(tst_SettingsDialog)
#include "tst_settingsdialog.moc"
