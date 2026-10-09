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
#include <qscrollarea.h>
#include <qscrollbar.h>
#include <qstyle.h>
#include <qwebengineprofile.h>

#include "settings.h"
#include "aroraicon.h"
#include "browserapplication.h"
#include "extensionreviewdialog.h"
#include "browserprofile.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "popupblocker.h"
#include "scopeshortcuts.h"
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
    void buttonBoxCloseRequest();
    void searchTab();
    void searchContextControls();
    void suggestionsCheckbox();
    void engineEditor();
    void searchShortcutNicknames();
    void resetSearchSettings();
    void sidebarNavigation();
    void sidebarSettings();
    void uniformSidebarIcons();
    void scrollablePages();
    void subDialogButtons();
    void setHomeToCurrentPage();
    void popupExceptions();
    void containersPage();
    void pagePolishSettings();
    void extensionReview();
};

// CONT03: fills the container editor's Name field and accepts — the
// editor exec()s synchronously inside the button click, so the driver
// keeps re-arming until the modal appears.
static void fillContainerEditor(const QString &name, int attempts = 150)
{
    QTimer::singleShot(20, qApp, [name, attempts]() {
        QDialog *dialog = qobject_cast<QDialog *>(
            QApplication::activeModalWidget());
        if (dialog) {
            if (QLineEdit *edit = dialog->findChild<QLineEdit *>())
                edit->setText(name);
            dialog->accept();
        } else if (attempts > 0) {
            fillContainerEditor(name, attempts - 1);
        }
    });
}

// Same re-arming driver for the delete confirmation QMessageBox.
static void answerNextBox(QMessageBox::StandardButton button,
                          int attempts = 150)
{
    QTimer::singleShot(20, qApp, [button, attempts]() {
        QMessageBox *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (box && box->button(button)) {
            box->button(button)->click();
        } else if (attempts > 0) {
            answerNextBox(button, attempts - 1);
        }
    });
}

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

// PREFS01: the in-tab button box keeps the modal dialog's contract —
// OK applies + asks the host to close, Cancel asks to close without
// applying.  closeRequested replaces QDialog::done().
void tst_SettingsDialog::buttonBoxCloseRequest()
{
    const QString original = QSettings().value(
        QLatin1String("MainWindow/home")).toString();

    SettingsDialog dialog;
    QSignalSpy spy(&dialog, &SettingsDialog::closeRequested);

    // Cancel discards the pending edit.
    dialog.homeLineEdit->setText(QLatin1String("http://discarded.example/"));
    dialog.buttonBox->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(QSettings().value(QLatin1String("MainWindow/home")).toString(),
             original);

    // OK persists the edit + closes.
    dialog.homeLineEdit->setText(QLatin1String("http://kept.example/"));
    dialog.buttonBox->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(spy.count(), 2);
    QCOMPARE(QSettings().value(QLatin1String("MainWindow/home")).toString(),
             QLatin1String("http://kept.example/"));

    SettingsDialog restore;
    restore.homeLineEdit->setText(original);
    restore.accept();
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

// SRCH06: the Shortcuts Nickname rows persist each scope's enabled
// flag and token, the token field follows its checkbox, whitespace is
// stripped on save and the values read back on reopen.
void tst_SettingsDialog::searchShortcutNicknames()
{
    ScopeShortcuts::reset();

    {
        SettingsDialog dialog;
        QVERIFY(dialog.shortcutBookmarksCheck->isChecked());
        QVERIFY(dialog.shortcutHistoryCheck->isChecked());
        QVERIFY(dialog.shortcutTabsCheck->isChecked());
        QCOMPARE(dialog.shortcutBookmarksToken->text(),
                 QLatin1String("@bookmarks"));
        QCOMPARE(dialog.shortcutHistoryToken->text(),
                 QLatin1String("@history"));
        QCOMPARE(dialog.shortcutTabsToken->text(),
                 QLatin1String("@tabs"));

        // The token field only edits while its scope is enabled.
        QVERIFY(dialog.shortcutTabsToken->isEnabled());
        dialog.shortcutTabsCheck->setChecked(false);
        QVERIFY(!dialog.shortcutTabsToken->isEnabled());

        dialog.shortcutHistoryToken->setText(QLatin1String("@hist"));
        dialog.shortcutBookmarksToken->setText(
            QLatin1String("@ bm")); // whitespace strips on save
        dialog.accept();
    }
    QCOMPARE(ScopeShortcuts::token(ScopeShortcuts::HistoryScope),
             QLatin1String("@hist"));
    QCOMPARE(ScopeShortcuts::token(ScopeShortcuts::BookmarksScope),
             QLatin1String("@bm"));
    QVERIFY(!ScopeShortcuts::enabled(ScopeShortcuts::TabsScope));

    {
        SettingsDialog dialog;
        QCOMPARE(dialog.shortcutHistoryToken->text(),
                 QLatin1String("@hist"));
        QCOMPARE(dialog.shortcutBookmarksToken->text(),
                 QLatin1String("@bm"));
        QVERIFY(!dialog.shortcutTabsCheck->isChecked());
        QVERIFY(!dialog.shortcutTabsToken->isEnabled());
    }
    ScopeShortcuts::reset();
}

// SRCH06: Reset Search Settings asks for confirmation, restores every
// Search-page-owned preference (engine picks, suggestion switches,
// shortcut nicknames, display options) and leaves the other pages'
// keys alone.
void tst_SettingsDialog::resetSearchSettings()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    manager->restoreDefaults();
    const QStringList engines = manager->allEnginesNames();
    QVERIFY(engines.count() >= 2);
    const QString other = engines.first() == manager->currentEngineName()
        ? engines.at(1) : engines.first();

    // Search-owned non-defaults everywhere the page writes.
    manager->setCurrentEngineName(other);
    manager->setPrivateEngineName(other);
    manager->setKeepFieldEngine(false);
    manager->setSuggestionsInAddressField(false);
    manager->setSuggestionsOnlyWithKeyword(true);
    manager->setSuggestionsEnabledForEngine(other, true);
    ScopeShortcuts::setToken(ScopeShortcuts::HistoryScope,
                             QLatin1String("@hh"));
    ScopeShortcuts::setEnabled(ScopeShortcuts::TabsScope, false);

    QSettings settings;
    settings.setValue(QLatin1String("urlloading/searchEngineFallback"), false);
    settings.setValue(QLatin1String("toolbarsearch/alwaysNewTab"), true);
    settings.setValue(QLatin1String("MainWindow/showSearchBox"), true);

    // Sentinels from other pages that must survive the reset.
    const QString home = QLatin1String("http://kept-home.example/");
    settings.setValue(QLatin1String("MainWindow/home"), home);
    settings.setValue(QLatin1String("tabs/oneCloseButton"), true);

    const auto clickMessageBoxButton = [](QMessageBox::StandardButton b) {
        QTimer::singleShot(50, qApp, [b] {
            if (QMessageBox *box = qobject_cast<QMessageBox *>(
                    QApplication::activeModalWidget())) {
                if (QAbstractButton *button = box->button(b))
                    button->click();
            }
        });
    };

    {
        SettingsDialog dialog;
        QCOMPARE(dialog.defaultEngineCombo->currentText(), other);

        // Rejecting the confirmation changes nothing.
        clickMessageBoxButton(QMessageBox::No);
        dialog.resetSearchButton->click();
        QCOMPARE(manager->currentEngineName(), other);
        QCOMPARE(ScopeShortcuts::token(ScopeShortcuts::HistoryScope),
                 QLatin1String("@hh"));

        // Confirming restores the Search page's defaults.
        clickMessageBoxButton(QMessageBox::Yes);
        dialog.resetSearchButton->click();

        QCOMPARE(manager->currentEngineName(),
                 QLatin1String("DuckDuckGo"));
        QCOMPARE(manager->privateEngineName(), QString());
        QVERIFY(manager->keepFieldEngine());
        QVERIFY(manager->suggestionsInAddressField());
        QVERIFY(!manager->suggestionsOnlyWithKeyword());
        QVERIFY(!manager->suggestionsEnabledForEngine(other));
        QCOMPARE(ScopeShortcuts::token(ScopeShortcuts::HistoryScope),
                 QLatin1String("@history"));
        QVERIFY(ScopeShortcuts::enabled(ScopeShortcuts::TabsScope));

        // The dialog controls show the restored defaults and the
        // persisted keys match.
        QVERIFY(dialog.searchEngineFallback->isChecked());
        QVERIFY(!dialog.alwaysNewTabCheck->isChecked());
        QVERIFY(dialog.suggestInAddressFieldCheck->isChecked());
        QVERIFY(!dialog.showSearchBox->isChecked());
        QCOMPARE(dialog.shortcutHistoryToken->text(),
                 QLatin1String("@history"));
        QCOMPARE(settings.value(
                     QLatin1String("urlloading/searchEngineFallback"))
                     .toBool(),
                 true);
        QCOMPARE(settings.value(
                     QLatin1String("toolbarsearch/alwaysNewTab")).toBool(),
                 false);

        // Sentinels from other pages are untouched.
        QCOMPARE(settings.value(QLatin1String("MainWindow/home"))
                     .toString(),
                 home);
        QCOMPARE(settings.value(QLatin1String("tabs/oneCloseButton"))
                     .toBool(),
                 true);
        QCOMPARE(dialog.homeLineEdit->text(), home);
    }

    // Leave the store tidy for the rest of the suite.
    manager->setCurrentEngineName(engines.first());
    manager->setSuggestionsEnabledForEngine(other, false);
    settings.remove(QLatin1String("MainWindow/home"));
    settings.remove(QLatin1String("tabs/oneCloseButton"));
    ScopeShortcuts::reset();
}

// UIP03: the sidebar list and the page stack stay in sync both ways,
// every page is reachable, and the persisted currentTab index keeps
// the old tab order across a save/reload.
void tst_SettingsDialog::sidebarNavigation()
{
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.pagesList->count(), dialog.tabWidget->count());
        QCOMPARE(dialog.pagesList->count(), 10);
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
        QCOMPARE(dialog.pagesList->item(9)->text(),
                 QStringLiteral("Containers"));
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

// SIDE01: the General page's sidebar controls — off by default,
// Left/Right dock side round-trips through MainWindow/sidebarDockArea.
void tst_SettingsDialog::sidebarSettings()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));

    // Fresh dialog defaults: unchecked, Left.
    {
        SettingsDialog dialog;
        QVERIFY(!dialog.showSidebar->isChecked());
        QCOMPARE(dialog.sidebarDockArea->currentIndex(), 0);
    }

    // Enabling + Right persists and reads back.
    {
        SettingsDialog dialog;
        dialog.showSidebar->setChecked(true);
        dialog.sidebarDockArea->setCurrentIndex(1);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSidebar")).toBool(),
             true);
    QCOMPARE(settings.value(QLatin1String("MainWindow/sidebarDockArea")).toInt(),
             int(Qt::RightDockWidgetArea));
    {
        SettingsDialog dialog;
        QVERIFY(dialog.showSidebar->isChecked());
        QCOMPARE(dialog.sidebarDockArea->currentIndex(), 1);
        dialog.showSidebar->setChecked(false);
        dialog.sidebarDockArea->setCurrentIndex(0);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSidebar")).toBool(),
             false);
    QCOMPARE(settings.value(QLatin1String("MainWindow/sidebarDockArea")).toInt(),
             int(Qt::LeftDockWidgetArea));
}

// ICONS02: the sidebar, the theme-preview combo and the engine list
// all pin iconSize at the style's small-icon metric — without it the
// delegate paints each icon at the resolved asset's own nominal size
// and rows render unevenly.  A runtime theme switch must not regrow
// the pin.
void tst_SettingsDialog::uniformSidebarIcons()
{
    const QString originalTheme = AroraIcon::theme();
    SettingsDialog dialog;

    const int extent =
        dialog.style()->pixelMetric(QStyle::PM_SmallIconSize);
    const QSize iconExtent(extent, extent);

    QCOMPARE(dialog.pagesList->iconSize(), iconExtent);
    QVERIFY(dialog.pagesList->uniformItemSizes());
    QCOMPARE(dialog.iconThemeCombo->iconSize(), iconExtent);
    QCOMPARE(dialog.engineTree->iconSize(), iconExtent);

    const auto checkRows = [&dialog]() {
        const int height = dialog.pagesList->sizeHintForRow(0);
        QVERIFY(height > 0);
        for (int row = 0; row < dialog.pagesList->count(); ++row) {
            QCOMPARE(dialog.pagesList->sizeHintForRow(row), height);
            QVERIFY(!dialog.pagesList->item(row)->icon().isNull());
        }
    };
    checkRows();

    // Re-resolving under each bundled set keeps the pinned extent and
    // uniform row heights.
    for (const QString &id : AroraIcon::themeIds()) {
        if (id == QLatin1String("native"))
            continue;
        AroraIcon::setTheme(id);
        QCOMPARE(dialog.pagesList->iconSize(), iconExtent);
        checkRows();
    }
    AroraIcon::setTheme(originalTheme);
}

// UIP06: every stacked page lives inside a scroll area so a page
// taller than the screen scrolls instead of pushing the button box
// out of reach; the dialog itself can never open larger than ~90% of
// the available screen.
void tst_SettingsDialog::scrollablePages()
{
    SettingsDialog dialog;

    // One resizable, frameless scroll area per page — stack indices
    // unchanged (the sidebar stays unwrapped and fixed-height).
    QCOMPARE(dialog.tabWidget->count(), 10);
    for (int i = 0; i < dialog.tabWidget->count(); ++i) {
        QScrollArea *area = qobject_cast<QScrollArea *>(
            dialog.tabWidget->widget(i));
        QVERIFY(area);
        QVERIFY(area->widgetResizable());
        QVERIFY(area->widget());
        QCOMPARE(area->frameShape(), QFrame::NoFrame);
    }

    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    const QSize avail = dialog.screen()->availableGeometry().size();
    QVERIFY(dialog.height() <= avail.height() * 9 / 10 + 1);
    QVERIFY(dialog.width() <= avail.width() * 9 / 10 + 1);
    QVERIFY(dialog.rect().contains(dialog.buttonBox->geometry()));

    // Re-showing at an oversized geometry snaps back inside the cap.
    dialog.hide();
    dialog.resize(avail.width() + 400, avail.height() + 400);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    qApp->processEvents();
    QVERIFY(dialog.height() <= avail.height() * 9 / 10 + 1);
    QVERIFY(dialog.width() <= avail.width() * 9 / 10 + 1);
    QVERIFY(dialog.rect().contains(dialog.buttonBox->geometry()));

    // Force a small viewport: a tall page must gain a scrollbar whose
    // range reaches the bottom of its content, while the button box
    // stays inside the dialog.
    const int shortHeight = qMax(200, avail.height() / 4);
    dialog.resize(dialog.width(), shortHeight);
    qApp->processEvents();
    QVERIFY(dialog.height() <= shortHeight + 1);
    QVERIFY(dialog.rect().contains(dialog.buttonBox->geometry()));
    bool sawScrollablePage = false;
    for (int i = 0; i < dialog.tabWidget->count(); ++i) {
        dialog.tabWidget->setCurrentIndex(i);
        qApp->processEvents();
        QScrollArea *area = qobject_cast<QScrollArea *>(
            dialog.tabWidget->currentWidget());
        QVERIFY(area);
        QScrollBar *bar = area->verticalScrollBar();
        // Content may compress to its layout minimum but is never
        // squished below it; anything taller scrolls to the bottom.
        QVERIFY(area->widget()->height()
                >= area->widget()->minimumSizeHint().height());
        if (area->widget()->height() > area->viewport()->height()) {
            sawScrollablePage = true;
            QVERIFY2(bar->maximum() > 0,
                     qPrintable(QStringLiteral(
                         "page %1 widget=%2 viewport=%3 max=%4")
                         .arg(i)
                         .arg(area->widget()->height())
                         .arg(area->viewport()->height())
                         .arg(bar->maximum())));
            bar->setValue(bar->maximum());
            QCOMPARE(bar->value(), bar->maximum());
            // The maximum scroll position really is the page bottom.
            QVERIFY(bar->value() + area->viewport()->height()
                    >= area->widget()->height());
        } else {
            QCOMPARE(bar->maximum(), 0);
        }
    }
    QVERIFY(sawScrollablePage);
    dialog.close();
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

// POPUP01: the Privacy page's site-permission audit table lists
// pop-up exceptions ("popup|<host>" keys), and Remove drops the host
// from the PopupBlocker store.
void tst_SettingsDialog::popupExceptions()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    blocker->clearAllowedHosts();
    blocker->clearSessionHosts();
    blocker->allowHost(QLatin1String("popups.example"));

    SettingsDialog dialog;
    QTreeWidgetItem *row = nullptr;
    for (int i = 0; i < dialog.permissionsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = dialog.permissionsTree->topLevelItem(i);
        if (item->text(0) == QLatin1String("popups.example"))
            row = item;
    }
    QVERIFY(row);
    QCOMPARE(row->text(1), QStringLiteral("Pop-ups"));
    QCOMPARE(row->text(2), QStringLiteral("Allowed"));
    QCOMPARE(row->data(0, Qt::UserRole).toString(),
             QLatin1String("popup|popups.example"));

    dialog.permissionsTree->setCurrentItem(row);
    QVERIFY(dialog.permissionRemoveButton->isEnabled());
    dialog.permissionRemoveButton->click();
    QVERIFY(!blocker->isAllowedHost(QLatin1String("popups.example")));

    // PopupBlocker::changed re-runs refreshPermissions synchronously —
    // the row is gone from the rebuilt table.
    for (int i = 0; i < dialog.permissionsTree->topLevelItemCount(); ++i) {
        QVERIFY(dialog.permissionsTree->topLevelItem(i)->text(0)
                != QLatin1String("popups.example"));
    }
}

// CONT03: the Containers page mirrors the ContainerManager registry —
// the empty state swaps the list for a label, New/Edit run through a
// name+color editor, Delete confirms before wiping the profile's
// storage, and open tabs on the dying profile are closed first.
void tst_SettingsDialog::containersPage()
{
    ContainerManager *manager = ContainerManager::instance();
    QVERIFY(manager->containers().isEmpty());

    {
        SettingsDialog dialog;
        dialog.tabWidget->setCurrentIndex(int(SettingsDialog::ContainersPage));

        // Empty state: only New is usable.
        QCOMPARE(dialog.containersStack->currentWidget(),
                 static_cast<QWidget *>(dialog.containersEmptyLabel));
        QVERIFY(!dialog.containersList->isVisibleTo(dialog.containersStack));
        QVERIFY(dialog.containerNewButton->isEnabled());
        QVERIFY(!dialog.containerEditButton->isEnabled());
        QVERIFY(!dialog.containerRemoveButton->isEnabled());

        // Cancelling the editor creates nothing.
        rejectModal();
        dialog.containerNewButton->click();
        QVERIFY(manager->containers().isEmpty());

        // Accepting with a name creates + selects the row.
        fillContainerEditor(QLatin1String("Work"));
        dialog.containerNewButton->click();
        QCOMPARE(manager->containers().count(), 1);
        QCOMPARE(dialog.containersList->count(), 1);
        QCOMPARE(dialog.containersList->item(0)->text(),
                 QLatin1String("Work"));
        QCOMPARE(dialog.containersStack->currentWidget(),
                 static_cast<QWidget *>(dialog.containersList));
        QCOMPARE(dialog.containersList->currentRow(), 0);
        QVERIFY(dialog.containerEditButton->isEnabled());
        QVERIFY(dialog.containerRemoveButton->isEnabled());

        const QString id = dialog.containersList->item(0)
            ->data(Qt::UserRole).toString();
        QVERIFY(manager->isContainerId(id));

        // Edit renames and recolors through the shared manager.
        fillContainerEditor(QLatin1String("Work Renamed"));
        dialog.containerEditButton->click();
        QCOMPARE(manager->containerForId(id).name,
                 QLatin1String("Work Renamed"));
        QCOMPARE(manager->containerForId(id).color,
                 ContainerManager::defaultColors().first());
        QCOMPARE(dialog.containersList->item(0)->text(),
                 QLatin1String("Work Renamed"));

        // A registry write from outside re-syncs the open page.
        manager->renameContainer(id, QLatin1String("Outside"));
        QCOMPARE(dialog.containersList->item(0)->text(),
                 QLatin1String("Outside"));

        // Declining the confirmation keeps the container.
        answerNextBox(QMessageBox::No);
        dialog.containerRemoveButton->click();
        QVERIFY(manager->isContainerId(id));

        // Confirming deletes it and the empty state comes back.
        answerNextBox(QMessageBox::Yes);
        dialog.containerRemoveButton->click();
        QVERIFY(!manager->isContainerId(id));
        QVERIFY(manager->containers().isEmpty());
        QCOMPARE(dialog.containersList->count(), 0);
        QCOMPARE(dialog.containersStack->currentWidget(),
                 static_cast<QWidget *>(dialog.containersEmptyLabel));
    }

    // The registry is empty for a fresh manager too.
    ContainerManager fresh;
    QVERIFY(fresh.containers().isEmpty());

    // Delete also drops the materialized profile's on-disk storage.
    const QString id = manager->createContainer(
        QLatin1String("Storage"), QColor(Qt::blue)).id;
    QWebEngineProfile *profile = manager->profileFor(id);
    QVERIFY(profile);
    const QString storagePath = profile->persistentStoragePath();
    QVERIFY(QDir(storagePath).exists());
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.containersList->count(), 1);
        dialog.containersList->setCurrentRow(0);
        answerNextBox(QMessageBox::Yes);
        dialog.containerRemoveButton->click();
        QVERIFY(!manager->isContainerId(id));
        QVERIFY(manager->profileIfCreated(id) == nullptr);
        QVERIFY(!QDir(storagePath).exists());
    }
}

// POL03: Appearance's dark-mode checkbox (off by default) and
// General's middle-click autoscroll (on by default everywhere but
// macOS) round-trip through the websettings group.
void tst_SettingsDialog::pagePolishSettings()
{
    QSettings settings;
    settings.remove(QLatin1String("websettings/forceDarkMode"));
    settings.remove(QLatin1String("websettings/middleClickAutoscroll"));

#if defined(Q_OS_MACOS)
    const bool autoscrollDefault = false;
#else
    const bool autoscrollDefault = true;
#endif
    {
        SettingsDialog dialog;
        QVERIFY(!dialog.forceDarkMode->isChecked());
        QCOMPARE(dialog.middleClickAutoscroll->isChecked(),
                 autoscrollDefault);
    }

    {
        SettingsDialog dialog;
        dialog.forceDarkMode->setChecked(true);
        dialog.middleClickAutoscroll->setChecked(!autoscrollDefault);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("websettings/forceDarkMode"))
                 .toBool(), true);
    QCOMPARE(settings.value(QLatin1String("websettings/middleClickAutoscroll"))
                 .toBool(), !autoscrollDefault);

    {
        SettingsDialog dialog;
        QVERIFY(dialog.forceDarkMode->isChecked());
        QCOMPARE(dialog.middleClickAutoscroll->isChecked(),
                 !autoscrollDefault);
        dialog.forceDarkMode->setChecked(false);
        dialog.middleClickAutoscroll->setChecked(autoscrollDefault);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("websettings/forceDarkMode"))
                 .toBool(), false);
    QCOMPARE(settings.value(QLatin1String("websettings/middleClickAutoscroll"))
                 .toBool(), autoscrollDefault);
}

// EXT02: the permission-review dialog is the consent gate every
// load/install passes through — a hostile manifest must surface its
// permissions humanized and the support caveats, MV2 must block
// approval, and cancel/approve must map to refuse/install.
void tst_SettingsDialog::extensionReview()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString manifestPath = dir.path() + QLatin1String("/manifest.json");
    {
        QFile file(manifestPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({
            "manifest_version": 3,
            "name": "HostileExt",
            "version": "1.2.3",
            "description": "totally harmless",
            "action": {},
            "permissions": ["tabs", "cookies", "debugger", "unknownthing"],
            "host_permissions": ["<all_urls>"]
        })");
    }
    const ExtensionManager::Manifest manifest =
        ExtensionManager::inspectManifest(dir.path());
    QVERIFY(manifest.valid);
    QCOMPARE(manifest.manifestVersion, 3);
    QCOMPARE(manifest.name, QLatin1String("HostileExt"));

    {
        ExtensionReviewDialog dialog(manifest, dir.path(),
                                     ExtensionReviewDialog::Install);
        QLabel *nameLabel = dialog.findChild<QLabel *>(
            QLatin1String("nameLabel"));
        QVERIFY(nameLabel);
        QVERIFY(nameLabel->text().contains(QLatin1String("HostileExt")));
        QVERIFY(nameLabel->text().contains(QLatin1String("1.2.3")));
        QCOMPARE(nameLabel->textFormat(), Qt::PlainText);

        QLabel *sourceLabel = dialog.findChild<QLabel *>(
            QLatin1String("sourceLabel"));
        QVERIFY(sourceLabel);
        QVERIFY(sourceLabel->text().contains(dir.path()));

        QListWidget *perms = dialog.findChild<QListWidget *>(
            QLatin1String("permissionsList"));
        QVERIFY(perms);
        QStringList permTexts;
        for (int i = 0; i < perms->count(); ++i)
            permTexts << perms->item(i)->text();
        QVERIFY(permTexts.join(QLatin1Char('\n'))
                .contains(QLatin1String("browsing history")));
        QVERIFY(permTexts.join(QLatin1Char('\n'))
                .contains(QLatin1String("cookies")));
        QVERIFY(permTexts.join(QLatin1Char('\n'))
                .contains(QLatin1String("debugger")));
        QVERIFY(permTexts.join(QLatin1Char('\n'))
                .contains(QLatin1String("all websites")));
        // Unknown API names fall back to a generic line naming them.
        QVERIFY(permTexts.join(QLatin1Char('\n'))
                .contains(QLatin1String("unknownthing")));

        QListWidget *warnings = dialog.findChild<QListWidget *>(
            QLatin1String("warningsList"));
        QVERIFY(warnings);
        QString warningText;
        for (int i = 0; i < warnings->count(); ++i)
            warningText += warnings->item(i)->text() + QLatin1Char('\n');
        // "debugger" is on the unsupported list; the action key earns
        // the no-toolbar honesty note.
        QVERIFY(warningText.contains(QLatin1String("will not work")));
        QVERIFY(warningText.contains(QLatin1String("debugger")));
        QVERIFY(warningText.contains(QLatin1String("toolbar")));

        QPushButton *approve = dialog.findChild<QPushButton *>(
            QLatin1String("approveButton"));
        QVERIFY(approve);
        QVERIFY(approve->isEnabled());
        QCOMPARE(approve->text(), QLatin1String("Install"));
    }

    // MV2 manifests disable the approve button entirely.
    QTemporaryDir mv2Dir;
    QVERIFY(mv2Dir.isValid());
    {
        QFile file(mv2Dir.path() + QLatin1String("/manifest.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"manifest_version\": 2, \"name\": \"Legacy\"}");
    }
    {
        const ExtensionManager::Manifest mv2 =
            ExtensionManager::inspectManifest(mv2Dir.path());
        QVERIFY(mv2.valid);
        ExtensionReviewDialog dialog(mv2, mv2Dir.path(),
                                     ExtensionReviewDialog::Install);
        QPushButton *approve = dialog.findChild<QPushButton *>(
            QLatin1String("approveButton"));
        QVERIFY(approve);
        QVERIFY(!approve->isEnabled());
    }

    // A .zip cannot be inspected — the review still demands consent
    // and labels the package.
    {
        QFile zip(dir.path() + QLatin1String("/packed.zip"));
        QVERIFY(zip.open(QIODevice::WriteOnly));
        zip.write("PK\x05\x06");
        zip.close();
        const ExtensionManager::Manifest packed =
            ExtensionManager::inspectManifest(zip.fileName());
        QVERIFY(!packed.valid);
        ExtensionReviewDialog dialog(packed, zip.fileName(),
                                     ExtensionReviewDialog::Install);
        QPushButton *approve = dialog.findChild<QPushButton *>(
            QLatin1String("approveButton"));
        QVERIFY(approve);
        QVERIFY(approve->isEnabled());
        QLabel *sourceLabel = dialog.findChild<QLabel *>(
            QLatin1String("sourceLabel"));
        QVERIFY(sourceLabel->text().contains(QLatin1String("package")));
        QListWidget *warnings = dialog.findChild<QListWidget *>(
            QLatin1String("warningsList"));
        QVERIFY(warnings->count() > 0);
    }

    // Consent gate: approve returns true, cancel returns false.
    const QString dirPath = dir.path();
    QTimer::singleShot(50, qApp, [dirPath]() {
        QWidget *widget = QApplication::activeModalWidget();
        if (ExtensionReviewDialog *dialog =
                qobject_cast<ExtensionReviewDialog *>(widget)) {
            dialog->findChild<QPushButton *>(
                QLatin1String("approveButton"))->click();
        }
    });
    QVERIFY(ExtensionReviewDialog::review(
        manifest, dirPath, ExtensionReviewDialog::Install));

    rejectModal(50);
    QVERIFY(!ExtensionReviewDialog::review(
        manifest, dirPath, ExtensionReviewDialog::Load));

    // Humanization sanity: the marquee mappings read like risk text.
    QCOMPARE(ExtensionReviewDialog::describePermission(
                 QLatin1String("cookies")),
             ExtensionReviewDialog::tr("Read and modify cookies"));
    QVERIFY(ExtensionReviewDialog::describeHostPermission(
                QLatin1String("*://*.example.com/*"))
            .contains(QLatin1String("example.com")));
}

QTEST_MAIN(tst_SettingsDialog)
#include "tst_settingsdialog.moc"
