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
#include <qcheckbox.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qframe.h>
#include <qgroupbox.h>
#include <qscrollarea.h>
#include <qscrollbar.h>
#include <qstandarditemmodel.h>
#include <qstyle.h>
#include <qtemporarydir.h>
#include <qwebengineprofile.h>

#include "settings.h"
#include "addressdialog.h"
#include "autofillmanager.h"
#include "adblockdialog.h"
#include "adblockmanager.h"
#include "aroraicon.h"
#include "browserapplication.h"
#include "browsertheme.h"
#include "extensionmanager.h"
#include "extensionreviewdialog.h"
#include "browserprofile.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "energysaver.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "popupblocker.h"
#include "scriptcontrolmanager.h"
#include "securestore.h"
#include "webpermissionmanager.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif
#include "scopeshortcuts.h"
#include "sidebarpanel.h"
#include "sidebarpositionpicker.h"
#include "tabpositionpicker.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_SettingsDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

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
    void sidebarPositionPicker();
    void settingsFilter();
    void iconThemeSelector();
    void themeSelector();
    void themePreview();
    void uniformSidebarIcons();
    void scrollablePages();
    void densityAndFooter();
    void subDialogButtons();
    void setHomeToCurrentPage();
    void popupExceptions();
    void containersPage();
    void autoFillAddressesPage();
    void downloadsPage();
    void tabPositionPicker();
    void contentBlockerEngine();
    void adblockManageButton();
    void pagePolishSettings();
    void extensionReview();
    void permissionsPage();
    void securityLevelHint();
    void extensionPage();
    void credentialsPage();
    void containerSiteRows();
    void performancePage();
    void energySaver();
    void tabHoverPreviews();
    void appearanceLeftAnchored();
    void fieldWidthCaps();
};

// Every re-arming modal driver below stamps itself with this counter
// at schedule time and self-terminates once it changes — cleanup()
// bumps it after each test so a chain whose prompt never appeared can
// never accept or reject a later test's dialogs (an accept() that is
// not a button click maps QMessageBox::question to "not Yes", and a
// reject() is an outright cancel).
static int s_janitorGeneration = 0;

// CONT03: fills the container editor's Name field and accepts — the
// editor exec()s synchronously inside the button click, so the driver
// keeps re-arming until the modal appears.
static void fillContainerEditor(const QString &name, int attempts = 150,
                                int generation = s_janitorGeneration)
{
    QTimer::singleShot(20, qApp, [name, attempts, generation]() {
        if (generation != s_janitorGeneration)
            return;
        QDialog *dialog = qobject_cast<QDialog *>(
            QApplication::activeModalWidget());
        if (dialog) {
            if (QLineEdit *edit = dialog->findChild<QLineEdit *>())
                edit->setText(name);
            dialog->accept();
        } else if (attempts > 0) {
            fillContainerEditor(name, attempts - 1, generation);
        }
    });
}

// ADDR01: fills the address editor's Name (+ optional ZIP) field and
// accepts — same re-arming shape as fillContainerEditor.
static void fillAddressEditor(const QString &name,
                              const QString &zip = QString(),
                              int attempts = 150,
                              int generation = s_janitorGeneration)
{
    QTimer::singleShot(20, qApp, [name, zip, attempts, generation]() {
        if (generation != s_janitorGeneration)
            return;
        AddressDialog *dialog = qobject_cast<AddressDialog *>(
            QApplication::activeModalWidget());
        if (dialog) {
            dialog->nameEdit->setText(name);
            if (!zip.isNull())
                dialog->zipEdit->setText(zip);
            dialog->accept();
        } else if (attempts > 0) {
            fillAddressEditor(name, zip, attempts - 1, generation);
        }
    });
}

// Same re-arming driver for the delete confirmation QMessageBox.
static void answerNextBox(QMessageBox::StandardButton button,
                          int attempts = 150,
                          int generation = s_janitorGeneration)
{
    QTimer::singleShot(20, qApp, [button, attempts, generation]() {
        if (generation != s_janitorGeneration)
            return;
        QMessageBox *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (box && box->button(button)) {
            box->button(button)->click();
        } else if (attempts > 0) {
            answerNextBox(button, attempts - 1, generation);
        }
    });
}

// Clicks the button whose text matches inside the next QMessageBox —
// needed for custom addButton() choices standard-button lookups miss.
static void clickModalButtonText(const QString &text, int attempts = 150,
                                 int generation = s_janitorGeneration)
{
    QTimer::singleShot(30, qApp, [text, attempts, generation]() {
        if (generation != s_janitorGeneration)
            return;
        QMessageBox *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!box) {
            if (attempts > 0)
                clickModalButtonText(text, attempts - 1, generation);
            return;
        }
        for (QAbstractButton *button : box->buttons()) {
            if (button->text().contains(text)) {
                button->click();
                return;
            }
        }
        box->reject();
    });
}

// Accepts whatever QMessageBox is active, then re-arms — a modal
// janitor for paths that raise a box asynchronously (the extension
// update check finishes whenever the fetch settles).
static void dismissModalBoxes(int generation, int attempts = 200)
{
    QTimer::singleShot(30, qApp, [generation, attempts]() {
        if (generation != s_janitorGeneration)
            return;
        if (QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget()))
            box->accept();
        if (attempts > 0)
            dismissModalBoxes(generation, attempts - 1);
    });
}

// Rejects whatever QDialog is active, then re-arms — the EXT05
// extension consent review exec()s synchronously inside
// updateCheckFinished and is not a QMessageBox.
static void rejectModalDialogs(int generation, int attempts = 200)
{
    QTimer::singleShot(30, qApp, [generation, attempts]() {
        if (generation != s_janitorGeneration)
            return;
        if (QDialog *dialog = qobject_cast<QDialog *>(
                QApplication::activeModalWidget()))
            dialog->reject();
        if (attempts > 0)
            rejectModalDialogs(generation, attempts - 1);
    });
}

// Fills the two password QLineEdits of the master-passphrase prompt
// and accepts.  The prompt exec()s once per validation failure, so
// each call answers one round — schedule one call per round and the
// shared list doles out that round's pair.
static void fillPassphraseDialog(std::shared_ptr<QStringList> fills,
                                 int attempts = 150,
                                 int generation = s_janitorGeneration)
{
    QTimer::singleShot(30, qApp, [fills, attempts, generation]() {
        if (generation != s_janitorGeneration)
            return;
        QDialog *dialog = qobject_cast<QDialog *>(
            QApplication::activeModalWidget());
        if (!dialog) {
            if (attempts > 0)
                fillPassphraseDialog(fills, attempts - 1, generation);
            return;
        }
        const QList<QLineEdit *> edits =
            dialog->findChildren<QLineEdit *>();
        if (edits.size() >= 2) {
            if (!fills->isEmpty())
                edits.at(0)->setText(fills->takeFirst());
            if (!fills->isEmpty())
                edits.at(1)->setText(fills->takeFirst());
        }
        dialog->accept();
    });
}

// Types into the single QLineEdit of the unlock prompt and accepts.
static void answerModalPassphrase(const QString &text, int attempts = 150,
                                  int generation = s_janitorGeneration)
{
    QTimer::singleShot(30, qApp, [text, attempts, generation]() {
        if (generation != s_janitorGeneration)
            return;
        QWidget *widget = QApplication::activeModalWidget();
        if (!widget) {
            if (attempts > 0)
                answerModalPassphrase(text, attempts - 1, generation);
            return;
        }
        if (QLineEdit *edit = widget->findChild<QLineEdit *>())
            edit->setText(text);
        if (QDialog *dialog = qobject_cast<QDialog *>(widget))
            dialog->accept();
    });
}

void tst_SettingsDialog::initTestCase()
{
    QCoreApplication::setApplicationName("tst_settingsdialog");
    QSettings settings;
    settings.clear();
#if defined(ARORA_RUSTCORE)
    // Site decisions outlive QSettings cleanup — clear them too.
    SiteDecisionStore::reset();
#endif
}

// Disarm any modal driver chain still armed when the test function
// returns — without this, an unanswered fillPassphraseDialog /
// answerNextBox / rejectModalDialogs tick keeps polling for ~5s and
// accepts or rejects the NEXT test's modal (performancePage's reset
// confirmation was eaten this way: accept() maps to "not Yes", the
// reset bailed, and hardwareAccel stayed unchecked).
void tst_SettingsDialog::cleanup()
{
    ++s_janitorGeneration;
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
        QCOMPARE(dialog.pagesList->count(), 12);
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
        QCOMPARE(dialog.pagesList->item(10)->text(),
                 QStringLiteral("Downloads"));
        QCOMPARE(dialog.pagesList->item(11)->text(),
                 QStringLiteral("Performance"));
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

// SIDE02: the sidebar controls live in the Appearance page's
// "Sidebar" group — off by default, Left/Right dock side round-trips
// through MainWindow/sidebarDockArea, and the Panels checklist
// persists each registered section under sidebar/panels/<id>.
void tst_SettingsDialog::sidebarSettings()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("sidebar/panels"));

    {
        SettingsDialog dialog;
        QVERIFY(!dialog.showSidebar->isChecked());
        QCOMPARE(dialog.sidebarDockArea->currentIndex(), 0);
        // The whole group moved off General onto the Appearance page.
        QWidget *host = dialog.sidebarGroupBox;
        while (host && host->parentWidget() != dialog.tabWidget)
            host = host->parentWidget();
        QVERIFY(host);
        QCOMPARE(dialog.tabWidget->indexOf(host),
                 int(SettingsDialog::AppearancePage));

        // The Panels checklist mirrors the registered rail entries —
        // every one on by default.
        QCOMPARE(dialog.sidebarPanelsList->count(),
                 SidebarPanel::panels().count());
        QListWidgetItem *history = nullptr;
        for (int i = 0; i < dialog.sidebarPanelsList->count(); ++i) {
            QListWidgetItem *item = dialog.sidebarPanelsList->item(i);
            QVERIFY(item->flags() & Qt::ItemIsUserCheckable);
            QCOMPARE(item->checkState(), Qt::Checked);
            QVERIFY(!item->icon().isNull());
            if (item->data(Qt::UserRole).toByteArray() == "history")
                history = item;
        }
        QVERIFY(history);

        // Unchecking persists under sidebar/panels/history.
        history->setCheckState(Qt::Unchecked);
        dialog.showSidebar->setChecked(true);
        dialog.sidebarDockArea->setCurrentIndex(1);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSidebar")).toBool(),
             true);
    QCOMPARE(settings.value(QLatin1String("MainWindow/sidebarDockArea")).toInt(),
             int(Qt::RightDockWidgetArea));
    QCOMPARE(settings.value(QLatin1String("sidebar/panels/history")).toBool(),
             false);
    QVERIFY(!SidebarPanel::isPanelVisible("history"));
    {
        SettingsDialog dialog;
        QVERIFY(dialog.showSidebar->isChecked());
        QCOMPARE(dialog.sidebarDockArea->currentIndex(), 1);
        for (int i = 0; i < dialog.sidebarPanelsList->count(); ++i) {
            QListWidgetItem *item = dialog.sidebarPanelsList->item(i);
            QCOMPARE(item->checkState(),
                     item->data(Qt::UserRole).toByteArray() == "history"
                         ? Qt::Unchecked : Qt::Checked);
        }
        dialog.showSidebar->setChecked(false);
        dialog.sidebarDockArea->setCurrentIndex(0);
        for (int i = 0; i < dialog.sidebarPanelsList->count(); ++i)
            dialog.sidebarPanelsList->item(i)->setCheckState(Qt::Checked);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSidebar")).toBool(),
             false);
    QCOMPARE(settings.value(QLatin1String("MainWindow/sidebarDockArea")).toInt(),
             int(Qt::LeftDockWidgetArea));
    QCOMPARE(settings.value(QLatin1String("sidebar/panels/history")).toBool(),
             true);
}

// SIDE03: the Sidebar group docks its position choice in a two-tile
// visual picker (miniature window + panel edge sketch) — same
// currentIndex semantics as the old combo (0 Left, 1 Right), click +
// keyboard commits, and the pick still persists through
// MainWindow/sidebarDockArea so applySidebarSettings moves the dock.
void tst_SettingsDialog::sidebarPositionPicker()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));

    SettingsDialog dialog;
    // The tiles can only take focus while their page is current —
    // navigate there instead of relying on a persisted currentTab.
    dialog.tabWidget->setCurrentIndex(int(SettingsDialog::AppearancePage));
    QCOMPARE(dialog.sidebarDockArea->count(), 2);
    QCOMPARE(dialog.sidebarDockArea->currentIndex(), 0);

    // Two tile children, one per dock side.
    const QList<QWidget *> tiles =
        dialog.sidebarDockArea->findChildren<QWidget *>();
    QCOMPARE(tiles.count(), 2);
    QCOMPARE(tiles.at(0)->accessibleName(), QLatin1String("Left"));
    QCOMPARE(tiles.at(1)->accessibleName(), QLatin1String("Right"));

    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    // Focus asserts need the window ACTIVE, not just exposed — the
    // offscreen platform can lag activation past the expose event.
    dialog.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&dialog));

    QSignalSpy spy(dialog.sidebarDockArea,
                   &SidebarPositionPicker::positionChanged);

    // Clicking the Right tile commits index 1.
    QTest::mouseClick(tiles.at(1), Qt::LeftButton);
    QCOMPARE(dialog.sidebarDockArea->currentIndex(), 1);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 1);
    // Re-clicking the selected tile is a no-op.
    QTest::mouseClick(tiles.at(1), Qt::LeftButton);
    QCOMPARE(spy.count(), 0);

    // Keyboard: arrows move focus between the two tiles (wrapping);
    // Enter commits the focused tile's index.
    tiles.at(0)->setFocus();
    QVERIFY(tiles.at(0)->hasFocus());
    QTest::keyClick(tiles.at(0), Qt::Key_Left);   // wraps to Right
    QVERIFY(tiles.at(1)->hasFocus());
    QTest::keyClick(tiles.at(1), Qt::Key_Right);  // wraps to Left
    QVERIFY(tiles.at(0)->hasFocus());
    QTest::keyClick(tiles.at(0), Qt::Key_Return); // commits 0
    QCOMPARE(dialog.sidebarDockArea->currentIndex(), 0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.last().at(0).toInt(), 0);
    dialog.close();

    // Persisted round-trip through accept(): index 1 keeps writing
    // RightDockWidgetArea, index 0 LeftDockWidgetArea — the values
    // applySidebarSettings consumes, unchanged from the combo.
    {
        SettingsDialog persist;
        persist.sidebarDockArea->setCurrentIndex(1);
        persist.accept();
        QCOMPARE(QSettings().value(QLatin1String("MainWindow/sidebarDockArea"))
                     .toInt(), int(Qt::RightDockWidgetArea));
    }
    {
        SettingsDialog reload;
        QCOMPARE(reload.sidebarDockArea->currentIndex(), 1);
        reload.sidebarDockArea->setCurrentIndex(0);
        reload.accept();
        QCOMPARE(QSettings().value(QLatin1String("MainWindow/sidebarDockArea"))
                     .toInt(), int(Qt::LeftDockWidgetArea));
    }

    // An out-of-range stored area maps to the Left tile.
    settings.setValue(QLatin1String("MainWindow/sidebarDockArea"), 99);
    {
        SettingsDialog clamped;
        QCOMPARE(clamped.sidebarDockArea->currentIndex(), 0);
    }
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
}

// PREFUI03: the nav filter field narrows the sidebar by page title
// and by control labels inside each page, keeps the stack on a listed
// hit, and restores the full list when cleared.
void tst_SettingsDialog::settingsFilter()
{
    SettingsDialog dialog;
    QVERIFY(dialog.pagesFilter);
    QCOMPARE(dialog.pagesList->count(), dialog.tabWidget->count());
    const int total = dialog.pagesList->count();

    // "cookie" is not a page title — it matches Privacy's cookie
    // controls (and the Containers explainer) through control labels,
    // and the stack auto-jumps to the first listed hit.
    dialog.pagesFilter->setText(QStringLiteral("cookie"));
    int visible = 0;
    for (int i = 0; i < total; ++i) {
        if (!dialog.pagesList->item(i)->isHidden())
            ++visible;
    }
    QVERIFY(visible >= 1);
    QVERIFY(visible < total);
    QVERIFY(!dialog.pagesList->item(int(SettingsDialog::PrivacyPage))
                 ->isHidden());
    QVERIFY(dialog.pagesList->item(int(SettingsDialog::GeneralPage))
                ->isHidden());
    QCOMPARE(dialog.pagesList->currentRow(),
             int(SettingsDialog::PrivacyPage));
    QCOMPARE(dialog.tabWidget->currentIndex(),
             int(SettingsDialog::PrivacyPage));

    // Selecting a listed hit opens its page (UserRole mapping —
    // visible row != stacked index once rows are hidden).
    dialog.pagesList->setCurrentRow(int(SettingsDialog::ContainersPage));
    QCOMPARE(dialog.tabWidget->currentIndex(),
             int(SettingsDialog::ContainersPage));

    // Page-title matching: "proxy" lists the Proxy page.
    dialog.pagesFilter->setText(QStringLiteral("proxy"));
    QVERIFY(!dialog.pagesList->item(int(SettingsDialog::ProxyPage))
                 ->isHidden());
    QVERIFY(dialog.pagesList->item(int(SettingsDialog::ContainersPage))
                ->isHidden());
    QCOMPARE(dialog.tabWidget->currentIndex(),
             int(SettingsDialog::ProxyPage));

    // "font" lives inside Appearance's controls, not its title.
    dialog.pagesFilter->setText(QStringLiteral("font"));
    QVERIFY(!dialog.pagesList->item(int(SettingsDialog::AppearancePage))
                 ->isHidden());
    QCOMPARE(dialog.tabWidget->currentIndex(),
             int(SettingsDialog::AppearancePage));

    // A no-match term hides every row without crashing.
    dialog.pagesFilter->setText(QStringLiteral("zzz-no-such-setting"));
    for (int i = 0; i < total; ++i)
        QVERIFY(dialog.pagesList->item(i)->isHidden());

    // Clearing restores the full list.
    dialog.pagesFilter->clear();
    for (int i = 0; i < total; ++i)
        QVERIFY(!dialog.pagesList->item(i)->isHidden());
}

// ICONS03: the icon-theme selector lives on the Appearance page (not
// General), and every row previews a multi-glyph strip rendered from
// the set's own artwork — Adwaita/Breeze/Tabler strips must visibly
// differ from each other.
void tst_SettingsDialog::iconThemeSelector()
{
    SettingsDialog dialog;

    // The combo's top-level page inside the stacked widget is
    // Appearance (sidebar row 2).
    QWidget *host = dialog.iconThemeCombo;
    while (host && host->parentWidget() != dialog.tabWidget)
        host = host->parentWidget();
    QVERIFY(host);
    QCOMPARE(dialog.tabWidget->indexOf(host), 2);
    QCOMPARE(dialog.pagesList->item(2)->text(), QStringLiteral("Appearance"));
    QCOMPARE(dialog.iconThemeLabel->parentWidget(),
             dialog.iconThemeCombo->parentWidget());
    QCOMPARE(dialog.iconThemeLabel->buddy(),
             static_cast<QWidget *>(dialog.iconThemeCombo));

    // Every row carries a painted strip, and distinct sets paint
    // distinct previews.
    QCOMPARE(dialog.iconThemeCombo->count(),
             AroraIcon::themeIds().count());
    QHash<QString, QImage> strips;
    for (int row = 0; row < dialog.iconThemeCombo->count(); ++row) {
        const QIcon icon = dialog.iconThemeCombo->itemIcon(row);
        QVERIFY2(!icon.isNull(),
                 qPrintable(dialog.iconThemeCombo->itemText(row)));
        const QImage image =
            icon.pixmap(dialog.iconThemeCombo->iconSize()).toImage();
        int opaque = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y).alpha() > 40)
                    ++opaque;
        QVERIFY2(opaque > 0,
                 qPrintable(dialog.iconThemeCombo->itemText(row)));
        strips[dialog.iconThemeCombo->itemData(row).toString()] = image;
    }
    QVERIFY(strips.value(QLatin1String("adwaita"))
            != strips.value(QLatin1String("tabler")));
    QVERIFY(strips.value(QLatin1String("adwaita"))
            != strips.value(QLatin1String("breeze")));
}

// THEME01: the Appearance Theme combo persists browser/colorScheme
// and applies live — accept() flips the palette mid-session; picking
// System again restores the captured platform palette even though the
// offscreen QPA reports Qt::ColorScheme::Unknown.
void tst_SettingsDialog::themeSelector()
{
    // Sits on the Appearance page (sidebar row 2), next to the
    // page-content forceDarkMode checkbox it must not be confused
    // with — the note label underneath keeps the split honest.
    {
        SettingsDialog dialog;
        QWidget *host = dialog.themeCombo;
        while (host && host->parentWidget() != dialog.tabWidget)
            host = host->parentWidget();
        QVERIFY(host);
        QCOMPARE(dialog.tabWidget->indexOf(host), 2);
        QCOMPARE(dialog.themeCombo->count(), 3);
        QCOMPARE(dialog.themeCombo->itemText(0),
                 QStringLiteral("System default"));
        QCOMPARE(dialog.themeLabel->buddy(),
                 static_cast<QWidget *>(dialog.themeCombo));
        QVERIFY(!dialog.themeNoteLabel->text().isEmpty());
        // Default with no stored key is System.
        QCOMPARE(dialog.themeCombo->currentIndex(), 0);
    }

    const QColor platformWindow =
        QApplication::palette().color(QPalette::Window);

    {
        SettingsDialog dialog;
        dialog.themeCombo->setCurrentIndex(2);   // Dark
        dialog.accept();
    }
    QCOMPARE(QSettings().value(
                 QLatin1String("browser/colorScheme")).toString(),
             QLatin1String("dark"));
    QVERIFY(BrowserTheme::paletteIsForced());
    QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));

    // Reloaded dialog shows the stored pick; Light applies live too.
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.themeCombo->currentIndex(), 2);
        dialog.themeCombo->setCurrentIndex(1);   // Light
        dialog.accept();
    }
    QCOMPARE(QSettings().value(
                 QLatin1String("browser/colorScheme")).toString(),
             QLatin1String("light"));
    QVERIFY(!BrowserTheme::paletteIsForced());
    QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));

    // Dark once more, then back to System — offscreen reports Unknown,
    // so this exercises the forced-palette teardown on the no-opinion
    // path rather than an explicit Light.
    {
        SettingsDialog dialog;
        dialog.themeCombo->setCurrentIndex(2);
        dialog.accept();
    }
    QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));
    {
        SettingsDialog dialog;
        dialog.themeCombo->setCurrentIndex(0);   // System default
        dialog.accept();
    }
    QCOMPARE(QSettings().value(
                 QLatin1String("browser/colorScheme")).toString(),
             QLatin1String("system"));
    QVERIFY(!BrowserTheme::paletteIsForced());
    QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             platformWindow);
}

// THEME02: the combo previews instantly — the palette flips on index
// change with no accept() and no key write; reverting the combo,
// pressing Cancel, or closing the page all undo the preview by
// re-resolving the persisted pick.
void tst_SettingsDialog::themePreview()
{
    // themeSelector (previous leg) leaves "system" persisted and the
    // platform palette restored.
    QCOMPARE(QSettings().value(
                 QLatin1String("browser/colorScheme")).toString(),
             QLatin1String("system"));
    const QColor platformWindow =
        QApplication::palette().color(QPalette::Window);
    QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));

    // Instant preview: the palette flips on index change alone and
    // nothing is persisted yet.
    {
        SettingsDialog dialog;
        dialog.themeCombo->setCurrentIndex(2);   // Dark
        QVERIFY(BrowserTheme::paletteIsForced());
        QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));
        QCOMPARE(QSettings().value(
                     QLatin1String("browser/colorScheme")).toString(),
                 QLatin1String("system"));
        // Reverting the combo before Apply restores the prior scheme.
        dialog.themeCombo->setCurrentIndex(0);   // System default
        QVERIFY(!BrowserTheme::paletteIsForced());
        QCOMPARE(QApplication::palette().color(QPalette::Window),
                 platformWindow);
    }

    // Cancel discards the preview, not just the widget state.
    {
        SettingsDialog dialog;
        dialog.themeCombo->setCurrentIndex(2);
        QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));
        dialog.reject();
    }
    QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             platformWindow);

    // Closing the page outright (tab close — no reject()) reverts too.
    {
        SettingsDialog *dialog = new SettingsDialog;
        dialog->themeCombo->setCurrentIndex(2);
        QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));
        delete dialog;
    }
    QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             platformWindow);

    // Committed dark + Light preview + discard -> the committed dark
    // scheme comes back, not the platform palette.
    {
        SettingsDialog dialog;
        dialog.themeCombo->setCurrentIndex(2);
        dialog.accept();
    }
    QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));
    {
        SettingsDialog *dialog = new SettingsDialog;
        QCOMPARE(dialog->themeCombo->currentIndex(), 2);
        dialog->themeCombo->setCurrentIndex(1);   // Light
        QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));
        delete dialog;
    }
    QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));

    // Restore "system" + the platform palette for the following legs.
    {
        SettingsDialog dialog;
        dialog.themeCombo->setCurrentIndex(0);
        dialog.accept();
    }
    QVERIFY(!BrowserTheme::paletteIsForced());
    QCOMPARE(QApplication::palette().color(QPalette::Window),
             platformWindow);
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
    // ICONS03: the combo's pin is the whole preview strip — N glyph
    // cells wide, one small-icon extent tall.
    QCOMPARE(dialog.iconThemeCombo->iconSize().height(), extent);
    QCOMPARE(dialog.iconThemeCombo->iconSize().width() % extent, 0);
    QVERIFY(dialog.iconThemeCombo->iconSize().width() > extent);
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
    QCOMPARE(dialog.tabWidget->count(), 12);
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

// PREFUI02: one compact padding rhythm across the whole page —
// identical insets on every stacked page, identical inner padding in
// every group box, and a slim right-aligned footer strip (separator
// line + content-sized button box) instead of the full-width box the
// style defaults produced.
void tst_SettingsDialog::densityAndFooter()
{
    SettingsDialog dialog;

    for (int i = 0; i < dialog.tabWidget->count(); ++i) {
        QScrollArea *area = qobject_cast<QScrollArea *>(
            dialog.tabWidget->widget(i));
        QVERIFY(area);
        QWidget *page = area->widget();
        QVERIFY(page && page->layout());
        QCOMPARE(page->layout()->contentsMargins(),
                 QMargins(8, 8, 8, 8));
    }
    const QList<QGroupBox *> groups =
        dialog.findChildren<QGroupBox *>();
    QVERIFY(!groups.isEmpty());
    for (QGroupBox *box : groups) {
        QVERIFY2(box->layout(), qPrintable(box->title()));
        QCOMPARE(box->layout()->contentsMargins(),
                 QMargins(9, 6, 9, 9));
    }

    // Footer strip: a full-width separator line above a button box
    // pinned to the right edge at content width — the box must no
    // longer stretch across the window.
    QFrame *line = dialog.findChild<QFrame *>(
        QLatin1String("footerLine"));
    QVERIFY(line);
    QCOMPARE(line->frameShape(), QFrame::HLine);
    QCOMPARE(dialog.buttonBox->sizePolicy().horizontalPolicy(),
             QSizePolicy::Maximum);

    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    qApp->processEvents();
    const int shellRight =
        dialog.layout()->contentsMargins().right();
    // geometry().right() is inclusive — +1 is the real edge.
    QVERIFY(qAbs(dialog.buttonBox->geometry().right() + 1
                 - (dialog.width() - shellRight)) <= 1);
    QVERIFY(dialog.buttonBox->geometry().left()
            > dialog.width() / 2);
    // The line spans the content width just above the buttons.
    QVERIFY(line->geometry().bottom()
            <= dialog.buttonBox->geometry().top());
    QVERIFY(line->width() >= dialog.width() - shellRight
            - dialog.layout()->contentsMargins().left());

    // Offscreen page grabs for visual review — set
    // ARORA_SETTINGS_GRAB_DIR to a directory and each page lands
    // there as settings-page-<n>.png; ARORA_SETTINGS_GRAB_SIZE=WxH
    // resizes the dialog before grabbing.
    const QByteArray grabDir = qgetenv("ARORA_SETTINGS_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        const QStringList grabSizeParts = QString::fromLocal8Bit(
            qgetenv("ARORA_SETTINGS_GRAB_SIZE")).split(QLatin1Char('x'));
        if (grabSizeParts.size() == 2) {
            dialog.resize(grabSizeParts[0].toInt(), grabSizeParts[1].toInt());
            qApp->processEvents();
        }
        for (int i = 0; i < dialog.tabWidget->count(); ++i) {
            dialog.tabWidget->setCurrentIndex(i);
            dialog.pagesList->setCurrentRow(i);
            qApp->processEvents();
            dialog.grab().save(QString::fromLocal8Bit(grabDir)
                + QStringLiteral("/settings-page-%1.png").arg(i));
        }
    }
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

// ADDR01: the AutoFill page's Saved Addresses section — Add opens the
// editor, the row appears on accept, Edit rewrites the stored fields,
// Remove/Remove All delete, and the manager's change signal re-syncs
// the open page.
void tst_SettingsDialog::autoFillAddressesPage()
{
    AutoFillManager *manager = AutoFillManager::instance();
    const QList<AutoFillManager::Address> saved = manager->addresses();
    manager->setAddresses(QList<AutoFillManager::Address>());

    {
        SettingsDialog dialog;
        dialog.tabWidget->setCurrentIndex(int(SettingsDialog::AutoFillPage));

        // Empty state: Edit/Remove are gated on a selection.
        QCOMPARE(dialog.addressesList->count(), 0);
        QVERIFY(!dialog.editAddressButton->isEnabled());
        QVERIFY(!dialog.removeAddressButton->isEnabled());

        // Cancelling the editor adds nothing.
        rejectModal();
        dialog.addAddressButton->click();
        QCOMPARE(manager->addresses().count(), 0);

        // Accepting with a name stores + selects the new row.
        fillAddressEditor(QLatin1String("Ada"));
        dialog.addAddressButton->click();
        QCOMPARE(manager->addresses().count(), 1);
        QCOMPARE(manager->addresses().first().name, QLatin1String("Ada"));
        QCOMPARE(dialog.addressesList->count(), 1);
        QCOMPARE(dialog.addressesList->currentRow(), 0);
        QVERIFY(dialog.addressesList->item(0)->text()
                .contains(QLatin1String("Ada")));
        QVERIFY(dialog.editAddressButton->isEnabled());
        QVERIFY(dialog.removeAddressButton->isEnabled());

        // Edit rewrites the persisted fields.
        fillAddressEditor(QLatin1String("Ada"), QLatin1String("62701"));
        dialog.editAddressButton->click();
        QCOMPARE(manager->addresses().first().zip, QLatin1String("62701"));

        // Remove deletes the selected row.
        dialog.addressesList->setCurrentRow(0);
        dialog.removeAddressButton->click();
        QCOMPARE(manager->addresses().count(), 0);
        QCOMPARE(dialog.addressesList->count(), 0);
        QVERIFY(!dialog.removeAddressButton->isEnabled());

        // A write through the manager re-syncs the open page; Remove
        // All clears the store.
        AutoFillManager::Address address;
        address.name = QLatin1String("Grace");
        manager->addAddress(address);
        QCOMPARE(dialog.addressesList->count(), 1);
        dialog.removeAllAddressesButton->click();
        QCOMPARE(manager->addresses().count(), 0);
        QCOMPARE(dialog.addressesList->count(), 0);
    }

    manager->setAddresses(saved);
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

// DLACC01: the Downloads page carries the engine selector — persisted
// under downloadmanager/engine, the same key the routing stub in
// handleDownloadRequested reads — plus the accelerated segment count
// and the finished-downloads cleanup policy, which had no UI before.
void tst_SettingsDialog::downloadsPage()
{
    // DLACC06: the fresh-profile default is only provable with no
    // persisted pick — drop any engine key an earlier leg wrote.
    QSettings().remove(QLatin1String("downloadmanager/engine"));
    {
        SettingsDialog dialog;
        dialog.openAtPage(SettingsDialog::DownloadsPage);
        QVERIFY(dialog.tabWidget->currentWidget());
        QCOMPARE(dialog.pagesList->currentItem()->text(),
                 QStringLiteral("Downloads"));

        QCOMPARE(dialog.downloadEngineCombo->count(), 2);
        QCOMPARE(dialog.downloadEngineCombo->itemData(0).toString(),
                 QLatin1String("builtin"));
        QCOMPARE(dialog.downloadEngineCombo->itemData(1).toString(),
                 QLatin1String("rust"));
        QStandardItemModel *model = qobject_cast<QStandardItemModel *>(
            dialog.downloadEngineCombo->model());
        QVERIFY(model);
#ifdef ARORA_RUSTDL
        QVERIFY(model->item(1)->isEnabled());
        // DLACC06: rust is the default — a fresh profile opens with
        // Accelerated selected and the segment spin armed.
        QCOMPARE(dialog.downloadEngineCombo->currentData().toString(),
                 QLatin1String("rust"));
        QVERIFY(dialog.downloadConnectionsSpin->isEnabled());
        // Picking Accelerated arms the segment spin; back to built-in
        // greys it out again.
        dialog.downloadEngineCombo->setCurrentIndex(1);
        QVERIFY(dialog.downloadConnectionsSpin->isEnabled());
        dialog.downloadEngineCombo->setCurrentIndex(0);
        QVERIFY(!dialog.downloadConnectionsSpin->isEnabled());
#else
        // Without a rustdl build the Accelerated row stays listed but
        // disabled (a persisted pick round-trips), and the segment
        // spin greys out under Built-in.
        QVERIFY(!model->item(1)->isEnabled());
        QVERIFY(!dialog.downloadConnectionsSpin->isEnabled());
#endif
        dialog.downloadConnectionsSpin->setValue(12);
        dialog.downloadCleanupCombo->setCurrentIndex(1);   // Exit
        // DLACC02: the built-in engine's ParallelDownloading toggle —
        // on by default, persisted downloadmanager/parallelSegments.
        QVERIFY(dialog.downloadParallelSegments->isChecked());
        dialog.downloadParallelSegments->setChecked(false);
        dialog.accept();
    }

    QSettings settings;
    QCOMPARE(settings.value(QLatin1String("downloadmanager/engine")).toString(),
             QLatin1String("builtin"));
    QCOMPARE(settings.value(QLatin1String("downloadmanager/connections")).toInt(),
             12);
    QCOMPARE(settings.value(QLatin1String("downloadmanager/parallelSegments"))
                 .toBool(), false);
    QCOMPARE(settings.value(QLatin1String("downloadmanager/removeDownloadsPolicy"))
                 .toString(), QLatin1String("Exit"));
    // The choice live-applied to the running manager, not just the key.
    QCOMPARE(int(DownloadManager::instance()->removePolicy()),
             int(DownloadManager::Exit));

    // Reopen: the saved engine, segment count, and policy read back.
    {
        SettingsDialog reloaded;
        QCOMPARE(reloaded.downloadEngineCombo->currentIndex(), 0);
        QCOMPARE(reloaded.downloadConnectionsSpin->value(), 12);
        QCOMPARE(reloaded.downloadCleanupCombo->currentIndex(), 1);
        QCOMPARE(reloaded.downloadParallelSegments->isChecked(), false);
        // Restore harness defaults for the tests that follow.
        reloaded.downloadConnectionsSpin->setValue(0);
        reloaded.downloadCleanupCombo->setCurrentIndex(0);
        reloaded.downloadParallelSegments->setChecked(true);
        reloaded.accept();
    }
    QCOMPARE(settings.value(QLatin1String("downloadmanager/removeDownloadsPolicy"))
                 .toString(), QLatin1String("Never"));
    QCOMPARE(settings.value(QLatin1String("downloadmanager/parallelSegments"))
                 .toBool(), true);
}

// TABS03: the Tab Settings page replaced the position combo with a
// Vivaldi-style tile picker inside a "Tab Bar Position" group.  The
// tiles display in Vivaldi's Top/Left/Right/Bottom order but commit
// the persisted combo indices Top/Bottom/Left/Right = 0/1/2/3 — the
// click, keyboard and clamp paths all prove the decoupled order.
void tst_SettingsDialog::tabPositionPicker()
{
    QSettings settings;
    settings.remove(QLatin1String("tabs/tabBarPosition"));

    SettingsDialog dialog;
    // The tiles can only take focus while their page is current —
    // navigate there instead of relying on a persisted currentTab.
    dialog.tabWidget->setCurrentIndex(int(SettingsDialog::TabSettingsPage));
    QVERIFY(dialog.tabBarPositionGroup);
    QCOMPARE(dialog.tabBarPositionGroup->title(),
             QLatin1String("Tab Bar Position"));
    QCOMPARE(dialog.tabBarPosition->count(), 4);
    QCOMPARE(dialog.tabBarPosition->currentIndex(), 0);

    // Four tile children, one per position, in display order.
    const QList<QWidget *> tiles =
        dialog.tabBarPosition->findChildren<QWidget *>();
    QCOMPARE(tiles.count(), 4);
    QCOMPARE(tiles.at(0)->accessibleName(), QLatin1String("Top"));
    QCOMPARE(tiles.at(1)->accessibleName(), QLatin1String("Left"));
    QCOMPARE(tiles.at(2)->accessibleName(), QLatin1String("Right"));
    QCOMPARE(tiles.at(3)->accessibleName(), QLatin1String("Bottom"));

    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    // Focus asserts need the window ACTIVE, not just exposed — the
    // offscreen platform can lag activation past the expose event.
    dialog.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&dialog));

    QSignalSpy spy(dialog.tabBarPosition,
                   &TabPositionPicker::positionChanged);

    // Clicking Left (display slot 1) commits persisted index 2;
    // clicking Bottom (slot 3) commits index 1.
    QTest::mouseClick(tiles.at(1), Qt::LeftButton);
    QCOMPARE(dialog.tabBarPosition->currentIndex(), 2);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 2);
    QTest::mouseClick(tiles.at(3), Qt::LeftButton);
    QCOMPARE(dialog.tabBarPosition->currentIndex(), 1);
    QCOMPARE(spy.count(), 1);

    // Keyboard: arrows move focus between tiles in display order
    // (wrapping); Enter commits the focused tile's persisted index.
    tiles.at(0)->setFocus();
    QVERIFY(tiles.at(0)->hasFocus());
    QTest::keyClick(tiles.at(0), Qt::Key_Left);      // wraps to Bottom
    QVERIFY(tiles.at(3)->hasFocus());
    QTest::keyClick(tiles.at(3), Qt::Key_Right);
    QVERIFY(tiles.at(0)->hasFocus());
    QTest::keyClick(tiles.at(0), Qt::Key_Right);
    QVERIFY(tiles.at(1)->hasFocus());
    QTest::keyClick(tiles.at(1), Qt::Key_Return);    // Left -> index 2
    QCOMPARE(dialog.tabBarPosition->currentIndex(), 2);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.last().at(0).toInt(), 2);
    QTest::keyClick(tiles.at(1), Qt::Key_Space);     // re-commit: no-op
    QCOMPARE(spy.count(), 2);
    dialog.close();

    // Persisted round-trip through accept().
    {
        SettingsDialog persist;
        persist.tabBarPosition->setCurrentIndex(3);  // Right
        persist.accept();
        QCOMPARE(QSettings().value(QLatin1String("tabs/tabBarPosition"))
                     .toInt(), 3);
    }

    // Out-of-range saved values clamp onto the edge tiles.
    settings.setValue(QLatin1String("tabs/tabBarPosition"), 99);
    {
        SettingsDialog clamped;
        QCOMPARE(clamped.tabBarPosition->currentIndex(), 3);
    }
    settings.setValue(QLatin1String("tabs/tabBarPosition"), -2);
    {
        SettingsDialog clamped;
        QCOMPARE(clamped.tabBarPosition->currentIndex(), 0);
    }
    settings.remove(QLatin1String("tabs/tabBarPosition"));
}

// ADB06/ADBLK01: the Content Blocking group on the Privacy page
// carries the runtime adblock-engine picker — persisted as
// AdBlock/engine through AdBlockManager::setEngine, so the switch
// answers the next request.  Brave adblock-rust is the default (an
// absent key selects it and materializes no key on accept); without a
// CONFIG+=adblock_rust build the Brave row is listed but disabled (a
// persisted pick round-trips), never a dead option.
void tst_SettingsDialog::contentBlockerEngine()
{
    // ADBLK01: start from a genuinely unset key — earlier legs'
    // accept()s may already have persisted a pick — so the dialog
    // shows the new Brave default, not leftovers.
    QSettings().remove(QLatin1String("AdBlock/engine"));
    {
        SettingsDialog dialog;
        dialog.openAtPage(SettingsDialog::PrivacyPage);
        QCOMPARE(dialog.pagesList->currentItem()->text(),
                 QStringLiteral("Privacy"));

        // The picker lives inside the Privacy page's Content
        // Blocking group, not on a page of its own.
        QWidget *host = dialog.adblockEngineCombo;
        while (host && host->parentWidget() != dialog.tabWidget)
            host = host->parentWidget();
        QVERIFY(host);
        QCOMPARE(dialog.tabWidget->indexOf(host),
                 int(SettingsDialog::PrivacyPage));
        QCOMPARE(dialog.adblockEngineLabel->buddy(),
                 static_cast<QWidget *>(dialog.adblockEngineCombo));
        QVERIFY(!dialog.adblockEngineStatus->text().isEmpty());

        QCOMPARE(dialog.adblockEngineCombo->count(), 2);
        QCOMPARE(dialog.adblockEngineCombo->itemData(0).toString(),
                 QLatin1String("cpp"));
        QCOMPARE(dialog.adblockEngineCombo->itemData(1).toString(),
                 QLatin1String("rust"));
        QStandardItemModel *model = qobject_cast<QStandardItemModel *>(
            dialog.adblockEngineCombo->model());
        QVERIFY(model);
        // The unset default selects the Brave row on either build —
        // enabled where the engine ships, disabled with the rebuild
        // hint where it does not.
        QCOMPARE(dialog.adblockEngineCombo->currentIndex(), 1);
#ifdef ARORA_ADBLOCK_RUST
        QVERIFY(model->item(1)->isEnabled());
        QVERIFY(dialog.adblockEngineStatus->text().contains(
            QStringLiteral("adblock-rust")));
        // Accepting the default writes nothing — the absent key IS
        // the Brave pick.
        dialog.accept();
        QVERIFY(!QSettings().contains(QLatin1String("AdBlock/engine")));
        QCOMPARE(AdBlockManager::instance()->engine(),
                 AdBlockManager::RustEngine);

        // Opting out to Built-in persists "cpp" and flips live.
        SettingsDialog reloaded;
        reloaded.adblockEngineCombo->setCurrentIndex(0);
        reloaded.accept();
        QCOMPARE(QSettings().value(QLatin1String("AdBlock/engine"))
                     .toString(), QLatin1String("cpp"));
        QCOMPARE(AdBlockManager::instance()->engine(),
                 AdBlockManager::NativeEngine);
        SettingsDialog reloaded2;
        QCOMPARE(reloaded2.adblockEngineCombo->currentIndex(), 0);
        QVERIFY(reloaded2.adblockEngineStatus->text().contains(
            QStringLiteral("Built-in")));
        // Picking Brave again writes the explicit id back.
        reloaded2.adblockEngineCombo->setCurrentIndex(1);
        reloaded2.accept();
        QCOMPARE(QSettings().value(QLatin1String("AdBlock/engine"))
                     .toString(), QLatin1String("rust"));
        // Restore the opt-out so the shared config ends native.
        SettingsDialog reloaded3;
        reloaded3.adblockEngineCombo->setCurrentIndex(0);
        reloaded3.accept();
#else
        QVERIFY(!model->item(1)->isEnabled());
        // The defaulted rust pick selects the (disabled) row and the
        // status line explains the native fallback — no silent
        // rewrite, no half-selected engine.
        QVERIFY(dialog.adblockEngineStatus->text().contains(
            QStringLiteral("not compiled")));
        QCOMPARE(AdBlockManager::instance()->engine(),
                 AdBlockManager::NativeEngine);
        // Accepting keeps the default absent — nothing materializes
        // a key the user never picked.
        dialog.accept();
        QVERIFY(!QSettings().contains(QLatin1String("AdBlock/engine")));
        QCOMPARE(AdBlockManager::storedEngine(),
                 AdBlockManager::RustEngine);

        // A persisted rust pick round-trips through the disabled row
        // the same way the default does.
        QSettings().setValue(QLatin1String("AdBlock/engine"),
                             QLatin1String("rust"));
        SettingsDialog reloaded;
        QCOMPARE(reloaded.adblockEngineCombo->currentIndex(), 1);
        QVERIFY(reloaded.adblockEngineStatus->text().contains(
            QStringLiteral("not compiled")));
        reloaded.accept();
        QCOMPARE(QSettings().value(QLatin1String("AdBlock/engine"))
                     .toString(), QLatin1String("rust"));
        // Switch back to Built-in and persist that.
        reloaded.adblockEngineCombo->setCurrentIndex(0);
        reloaded.accept();
        QCOMPARE(QSettings().value(QLatin1String("AdBlock/engine"))
                     .toString(), QLatin1String("cpp"));
#endif
    }
    QCOMPARE(AdBlockManager::storedEngine(), AdBlockManager::NativeEngine);
}

// MENU05: with the Tools-menu "Ad Block..." entry removed, the
// Content Blocking group's Manage button is the Preferences-side
// path to the same modeless AdBlockDialog the site panel opens.
void tst_SettingsDialog::adblockManageButton()
{
    SettingsDialog dialog;
    dialog.openAtPage(SettingsDialog::PrivacyPage);

    QCOMPARE(dialog.adblockManageButton->parentWidget()
                 ->objectName(),
             QStringLiteral("contentBlockingGroup"));

    dialog.adblockManageButton->click();
    AdBlockDialog *adblock = nullptr;
    for (int i = 0; i < 50 && !adblock; ++i) {
        QTest::qWait(20);
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            AdBlockDialog *candidate =
                qobject_cast<AdBlockDialog *>(widget);
            if (candidate && candidate->isVisible()) {
                adblock = candidate;
                break;
            }
        }
    }
    QVERIFY(adblock);
    adblock->close();
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
        // EXT05: HostileExt declares dangerous access (cookies,
        // debugger, <all_urls>) — approve stays locked until every
        // per-permission consent box is ticked.
        QVERIFY(manifest.dangerous.contains(QLatin1String("cookies")));
        QVERIFY(manifest.dangerous.contains(QLatin1String("debugger")));
        QVERIFY(manifest.dangerous.contains(QLatin1String("<all_urls>")));
        QVERIFY(!approve->isEnabled());
        const QList<QCheckBox *> consents =
            dialog.findChildren<QCheckBox *>(
                QLatin1String("dangerousConsent"));
        QCOMPARE(consents.size(), manifest.dangerous.size());
        for (QCheckBox *box : consents) {
            QVERIFY(!box->isChecked());
            box->setChecked(true);
        }
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

    // Consent gate: approve returns true, cancel returns false — and
    // an unchecked dangerous permission keeps approve locked, so the
    // review can only pass once every consent box is ticked.
    const QString dirPath = dir.path();
    QTimer::singleShot(50, qApp, [dirPath]() {
        QWidget *widget = QApplication::activeModalWidget();
        if (ExtensionReviewDialog *dialog =
                qobject_cast<ExtensionReviewDialog *>(widget)) {
            const QList<QCheckBox *> consents =
                dialog->findChildren<QCheckBox *>(
                    QLatin1String("dangerousConsent"));
            for (QCheckBox *box : consents)
                box->setChecked(true);
            dialog->findChild<QPushButton *>(
                QLatin1String("approveButton"))->click();
        }
    });
    QVERIFY(ExtensionReviewDialog::review(
        manifest, dirPath, ExtensionReviewDialog::Install));
    // The approval recorded a one-shot consent for the path.
    QVERIFY(ExtensionManager::instance()->hasConsent(dirPath));

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

// COV05: the Privacy page's site-permissions audit table — every row
// kind (web permission, JSCTL rule, pop-up exception) is removed
// through the Remove button, and Clear All empties the stores behind
// the confirmation box.
void tst_SettingsDialog::permissionsPage()
{
    WebPermissionManager *perms = WebPermissionManager::instance();
    ScriptControlManager *scripts = ScriptControlManager::instance();
    PopupBlocker *popups = PopupBlocker::instance();
    perms->clearEntries();
    scripts->clearPersistentRules();
    popups->clearAllowedHosts();

    const QUrl origin(QStringLiteral("https://perm.example"));
    perms->setEntry(origin, QWebEnginePermission::PermissionType::Notifications, true);
    perms->setEntry(QUrl(QStringLiteral("https://deny.example")),
                    QWebEnginePermission::PermissionType::Geolocation, false);
    scripts->setRuleForHost(QStringLiteral("js-allow.example"),
                            ScriptControlManager::Allow);
    scripts->setRuleForHost(QStringLiteral("js-block.example"),
                            ScriptControlManager::Block);
    popups->allowHost(QStringLiteral("popup.example"));

    SettingsDialog dialog;
    dialog.openAtPage(SettingsDialog::PrivacyPage);

    // One row per seeded entry, keyed by kind.
    QCOMPARE(dialog.permissionsTree->topLevelItemCount(), 5);
    QStringList keys;
    for (int i = 0; i < dialog.permissionsTree->topLevelItemCount(); ++i)
        keys << dialog.permissionsTree->topLevelItem(i)
                    ->data(0, Qt::UserRole).toString();
    QVERIFY(keys.contains(QStringLiteral("js|js-allow.example")));
    QVERIFY(keys.contains(QStringLiteral("js|js-block.example")));
    QVERIFY(keys.contains(QStringLiteral("popup|popup.example")));

    // Removing each row kind routes to its own store.
    auto removeRowWithKey = [&dialog](const QString &key) {
        for (int i = 0; i < dialog.permissionsTree->topLevelItemCount();
                ++i) {
            QTreeWidgetItem *item =
                dialog.permissionsTree->topLevelItem(i);
            if (item->data(0, Qt::UserRole).toString() == key) {
                dialog.permissionsTree->setCurrentItem(item);
                dialog.permissionRemoveButton->click();
                return;
            }
        }
        QVERIFY2(false, qPrintable(key));
    };
    removeRowWithKey(QStringLiteral("js|js-allow.example"));
    QCOMPARE(scripts->ruleForHost(QStringLiteral("js-allow.example")),
             ScriptControlManager::SiteDefault);
    removeRowWithKey(QStringLiteral("popup|popup.example"));
    QVERIFY(!popups->allowedHosts().contains(
                QStringLiteral("popup.example")));
    const QString permKey =
        QString::fromUtf8(origin.toEncoded()) + QLatin1Char('|')
        + QString::number(
            static_cast<int>(QWebEnginePermission::PermissionType::Notifications));
    removeRowWithKey(permKey);
    QCOMPARE(perms->entries().count(), 1);

    // Remove with no selection is a no-op (guard path).
    dialog.permissionsTree->setCurrentItem(nullptr);
    dialog.permissionRemoveButton->click();

    // Clear All behind the confirmation box empties every store.
    answerNextBox(QMessageBox::Yes);
    dialog.permissionClearAllButton->click();
    QTest::qWait(400);
    QVERIFY(perms->entries().isEmpty());
    QVERIFY(scripts->allowedHosts().isEmpty());
    QVERIFY(scripts->blockedHosts().isEmpty());
    QVERIFY(popups->allowedHosts().isEmpty());
    QCOMPARE(dialog.permissionsTree->topLevelItemCount(), 0);

    // Clear on an empty table returns before the confirmation prompt.
    dialog.permissionClearAllButton->click();
}

// COV05: the privacy tier combo drives updateSecurityLevelHint() —
// each tier rewrites the hint text and only Safest greys the global
// JavaScript checkbox.
void tst_SettingsDialog::securityLevelHint()
{
    SettingsDialog dialog;
    QVERIFY(dialog.securityLevelCombo->count() >= 2);
    const bool javascriptEnabledAtStart =
        dialog.enableJavascript->isEnabled();
    for (int i = 0; i < dialog.securityLevelCombo->count(); ++i) {
        dialog.securityLevelCombo->setCurrentIndex(i);
        QVERIFY(!dialog.securityLevelHint->text().isEmpty());
    }
    // The widget returns to a non-Safest state usable by later tests.
    dialog.securityLevelCombo->setCurrentIndex(0);
    Q_UNUSED(javascriptEnabledAtStart);
}

// EXT01/EXT03: the Extensions page over a real session-loaded
// extension — the details pane renders manifest fields, the checkbox
// routes enable toggles, the update check funnels results into the
// summary box, and Remove unloads with confirmation.
void tst_SettingsDialog::extensionPage()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt WebEngine extensions unavailable in this build");

    ExtensionManager *manager = ExtensionManager::instance();

    // A minimal MV3 extension with a dead update source — the fetch
    // fails fast and the manifest exercises the details-pane branches.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile file(dir.path() + QLatin1String("/manifest.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"({
            "manifest_version": 3,
            "name": "CovExt",
            "version": "1.0.0",
            "description": "coverage extension",
            "permissions": ["tabs"],
            "update_url": "https://127.0.0.1:9/updates.xml"
        })");
    }
    // EXT05: simulate the approved review — loadExtension refuses
    // paths that never passed the consent gate.
    manager->grantConsent(dir.path());
    manager->loadExtension(dir.path());

    QString extId;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QList<ExtensionManager::ExtensionInfo> list =
            manager->extensions();
        for (const ExtensionManager::ExtensionInfo &info : list) {
            if (info.path == dir.path()) {
                extId = info.id;
                return true;
            }
        }
        return false;
    }(), 10000);

    SettingsDialog dialog;
    dialog.tabWidget->setCurrentIndex(int(SettingsDialog::ExtensionsPage));

    QTreeWidgetItem *row = nullptr;
    for (int i = 0; i < dialog.extensionsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = dialog.extensionsTree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == extId)
            row = item;
    }
    QVERIFY(row);
    dialog.extensionsTree->setCurrentItem(row);
    const QString details = dialog.extensionDetailsLabel->text();
    QVERIFY(details.contains(QLatin1String("coverage extension")));
    QVERIFY(details.contains(QLatin1String("Version 1.0.0")));
    QVERIFY(details.contains(QLatin1String("Update source:")));
    QVERIFY(details.contains(QLatin1String("Permissions: tabs")));
    QVERIFY(dialog.extensionRemoveButton->isEnabled());

    // The checkbox routes through extensionItemChanged.
    row->setCheckState(0, Qt::Unchecked);
    row->setCheckState(0, Qt::Checked);

    // Wrong-column and id-less item changes are ignored.
    QVERIFY(QMetaObject::invokeMethod(&dialog, "extensionItemChanged",
        Q_ARG(QTreeWidgetItem*, row), Q_ARG(int, 1)));
    // Heap like every tree item — the tree clears own and delete its
    // rows; a stack item would be freed twice.
    QTreeWidgetItem *bogus = new QTreeWidgetItem(dialog.extensionsTree);
    bogus->setData(0, Qt::UserRole, QString());
    QVERIFY(QMetaObject::invokeMethod(&dialog, "extensionItemChanged",
        Q_ARG(QTreeWidgetItem*, bogus), Q_ARG(int, 0)));
    delete bogus;

    // installExtension's chooser: Cancel bails; Folder hands off to
    // the directory picker (rejected → no path → early return).
    clickModalButtonText(QLatin1String("Cancel"));
    dialog.extensionInstallButton->click();
    clickModalButtonText(QLatin1String("Folder"));
    rejectModal(); // QFileDialog::getExistingDirectory
    dialog.extensionInstallButton->click();
    clickModalButtonText(QLatin1String("ZIP"));
    rejectModal(); // QFileDialog::getOpenFileName
    dialog.extensionInstallButton->click();

    // Load bails on an empty directory pick too.
    rejectModal(); // QFileDialog::getExistingDirectory
    dialog.extensionLoadButton->click();

    // The update button kicks a real check; the dead https source
    // fails asynchronously, so the manual flag is still set when the
    // crafted results land through the signal — the summary box shows
    // the offered/downloaded/failed join, and the installable package
    // falls through to the EXT05 consent review (rejected here).
    const int janitorGeneration = ++s_janitorGeneration;
    dismissModalBoxes(janitorGeneration);
    rejectModalDialogs(janitorGeneration);
    dialog.extensionUpdateButton->click();
    {
        QList<ExtensionManager::UpdateResult> results;
        ExtensionManager::UpdateResult installed;
        installed.name = QLatin1String("Installed");
        installed.availableVersion = QLatin1String("2.0");
        installed.savedTo = QLatin1String("/tmp/covext-installed.zip");
        installed.installable = true;
        results << installed;
        ExtensionManager::UpdateResult downloaded;
        downloaded.name = QLatin1String("Downloaded");
        downloaded.availableVersion = QLatin1String("3.0");
        downloaded.savedTo = QLatin1String("/tmp/covext.zip");
        results << downloaded;
        ExtensionManager::UpdateResult failed;
        failed.name = QLatin1String("Failed");
        failed.error = QLatin1String("boom");
        results << failed;
        emit manager->updateCheckFinished(results);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!manager->updateCheckInProgress(), 10000);

    // The user-scripts list shows its placeholder when the folder is
    // empty.
    QVERIFY(dialog.userScriptsList->count() >= 1);

    // Remove asks once and unloads on Yes.
    answerNextBox(QMessageBox::Yes);
    dialog.extensionRemoveButton->click();
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        const QList<ExtensionManager::ExtensionInfo> list =
            manager->extensions();
        for (const ExtensionManager::ExtensionInfo &info : list) {
            if (info.id == extId)
                return false;
        }
        return true;
    }(), 10000);

    // Disarm the modal janitors — the async update check has
    // settled, and armed rejects would eat the next test's dialogs.
    ++s_janitorGeneration;
}

// SEC13: the saved-credentials group reflects SecureStore state —
// the copy flips between file-key / unlocked / locked modes, the
// passphrase prompt retries on bad input, and Remove confirms first.
void tst_SettingsDialog::credentialsPage()
{
    if (!SecureStore::isAvailable())
        QSKIP("crypto backend unavailable");
    SecureStore::resetForTests();
    SecureStore::setInteractiveUnlockEnabled(true);

    SettingsDialog dialog;
    QVERIFY(dialog.credentialStatusLabel->text()
            .contains(QLatin1String("Set a master passphrase")));
    QVERIFY(dialog.credentialPassphraseButton->isEnabled());
    QVERIFY(!dialog.credentialRemoveButton->isEnabled());
    QVERIFY(!dialog.credentialLockButton->isEnabled());

    // Cancelling the prompt leaves file-key mode in place.
    rejectModal();
    dialog.credentialPassphraseButton->click();
    QVERIFY(!SecureStore::passphraseProtectionEnabled());

    // The prompt retries on a short entry and a mismatch before
    // accepting — one exec() round per failure.
    auto fills = std::make_shared<QStringList>(QStringList()
        << QLatin1String("short") << QLatin1String("short")
        << QLatin1String("cov-pass-1") << QLatin1String("other")
        << QLatin1String("cov-pass-1") << QLatin1String("cov-pass-1"));
    fillPassphraseDialog(fills);
    fillPassphraseDialog(fills);
    fillPassphraseDialog(fills);
    dialog.credentialPassphraseButton->click();
    QVERIFY(SecureStore::passphraseProtectionEnabled());
    QVERIFY(SecureStore::isUnlocked());
    QVERIFY(dialog.credentialStatusLabel->text()
            .contains(QLatin1String("currently unlocked")));
    QVERIFY(dialog.credentialRemoveButton->isEnabled());
    QVERIFY(dialog.credentialLockButton->isEnabled());

    // Lock drops to the locked copy; Remove still needs the passphrase.
    dialog.credentialLockButton->click();
    QVERIFY(!SecureStore::isUnlocked());
    QVERIFY(dialog.credentialStatusLabel->text()
            .contains(QLatin1String("store is locked")));
    QVERIFY(!dialog.credentialLockButton->isEnabled());

    // Denying the unlock prompt cancels the removal.
    rejectModal();
    dialog.credentialRemoveButton->click();
    QVERIFY(SecureStore::passphraseProtectionEnabled());

    // Unlocking then declining the confirmation keeps it too.
    answerModalPassphrase(QLatin1String("cov-pass-1"));
    answerNextBox(QMessageBox::No);
    dialog.credentialRemoveButton->click();
    QVERIFY(SecureStore::passphraseProtectionEnabled());

    // Unlock + Yes removes the passphrase custody.
    answerModalPassphrase(QLatin1String("cov-pass-1"));
    answerNextBox(QMessageBox::Yes);
    dialog.credentialRemoveButton->click();
    QVERIFY(!SecureStore::passphraseProtectionEnabled());
    QVERIFY(dialog.credentialStatusLabel->text()
            .contains(QLatin1String("key stored next")));

    SecureStore::resetForTests();
}

// CONT04: the Containers page's site-rules list follows the selected
// container — rows come from siteRules(), selection arms Remove, and
// removeSiteRule repopulates through siteRulesChanged.
void tst_SettingsDialog::containerSiteRows()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString id = manager->createContainer(
        QLatin1String("Sites"), QColor(Qt::red)).id;
    QVERIFY(manager->setSiteRule(QLatin1String("Example.COM."), id));
    QCOMPARE(manager->siteRules(id),
             QStringList() << QLatin1String("example.com"));

    SettingsDialog dialog;
    dialog.tabWidget->setCurrentIndex(int(SettingsDialog::ContainersPage));
    dialog.containersList->setCurrentRow(0);
    QCOMPARE(dialog.containerSitesList->count(), 1);
    QCOMPARE(dialog.containerSitesList->item(0)->text(),
             QLatin1String("example.com"));
    QVERIFY(dialog.containerSitesLabel->text()
            .contains(QLatin1String("always open in this container")));

    // No selected rule → Remove is a no-op.
    dialog.containerSitesList->setCurrentItem(nullptr);
    dialog.containerSiteRemoveButton->click();
    QCOMPARE(manager->siteRules(id).count(), 1);

    // A selected rule deletes through the shared manager; the changed
    // signal rebuilds the list empty.
    dialog.containerSitesList->setCurrentRow(0);
    QVERIFY(dialog.containerSiteRemoveButton->isEnabled());
    dialog.containerSiteRemoveButton->click();
    QVERIFY(manager->siteRules(id).isEmpty());
    QCOMPARE(dialog.containerSitesList->count(), 0);

    // No container selected → the explanatory label returns.
    dialog.containersList->setCurrentItem(nullptr);
    QVERIFY(dialog.containerSitesLabel->text()
            .contains(QLatin1String("always open in the selected container")));

    QVERIFY(manager->deleteContainer(id));
}

// PREFUI04: the Performance page — every control persists under its
// consumer's key, the migrated suspend/prefetch keys keep their
// original paths, each setting has exactly one home (the old Privacy
// prefetch checkbox and the Tab Settings suspend row are gone), and
// the page's reset restores compiled defaults only.
void tst_SettingsDialog::performancePage()
{
    QSettings settings;
    settings.remove(QLatin1String("performance"));
    settings.remove(QLatin1String("tabs/suspendTabs"));
    settings.remove(QLatin1String("tabs/suspendTabsMinutes"));
    settings.remove(QLatin1String("privacy/blockPrefetch"));

    // Defaults and the single-home invariant.
    {
        SettingsDialog dialog;
        QCOMPARE(dialog.pagesList->item(
                     int(SettingsDialog::PerformancePage))->text(),
                 QStringLiteral("Performance"));
        QVERIFY(dialog.hardwareAccel->isChecked());
        QVERIFY(dialog.memorySaverNever->isChecked());
        QVERIFY(!dialog.lazyRestoreTabs->isChecked());
        // The pinned opt-out only enables while lazy restore is on.
        QVERIFY(!dialog.alwaysLoadPinnedTabs->isEnabled());
        QVERIFY(dialog.alwaysLoadPinnedTabs->isChecked());
        // Positive form of the default-on privacy/blockPrefetch key.
        QVERIFY(!dialog.prefetchResources->isChecked());
        QVERIFY(!dialog.findChild<QCheckBox *>(
                    QLatin1String("blockPrefetch")));
        QVERIFY(!dialog.findChild<QCheckBox *>(
                    QLatin1String("suspendTabs")));
        QVERIFY(!dialog.findChild<QSpinBox *>(
                    QLatin1String("suspendTabsMinutes")));

        dialog.hardwareAccel->setChecked(false);
        dialog.memorySaverMaximum->setChecked(true);
        dialog.lazyRestoreTabs->setChecked(true);
        QVERIFY(dialog.alwaysLoadPinnedTabs->isEnabled());
        dialog.alwaysLoadPinnedTabs->setChecked(false);
        dialog.prefetchResources->setChecked(true);
        dialog.accept();
    }
    QCOMPARE(settings.value(
                 QLatin1String("performance/hardwareAcceleration"))
                 .toBool(), false);
    QCOMPARE(settings.value(
                 QLatin1String("performance/lazyRestoreTabs"))
                 .toBool(), true);
    QCOMPARE(settings.value(
                 QLatin1String("performance/alwaysLoadPinnedTabs"))
                 .toBool(), false);
    QCOMPARE(settings.value(QLatin1String("tabs/suspendTabs"))
                 .toBool(), true);
    QCOMPARE(settings.value(QLatin1String("tabs/suspendTabsMinutes"))
                 .toInt(), 10);
    QCOMPARE(settings.value(QLatin1String("privacy/blockPrefetch"))
                 .toBool(), false);

    // Round-trip back into a fresh dialog.
    {
        SettingsDialog dialog;
        QVERIFY(!dialog.hardwareAccel->isChecked());
        QVERIFY(dialog.memorySaverMaximum->isChecked());
        QVERIFY(dialog.lazyRestoreTabs->isChecked());
        QVERIFY(dialog.alwaysLoadPinnedTabs->isEnabled());
        QVERIFY(!dialog.alwaysLoadPinnedTabs->isChecked());
        QVERIFY(dialog.prefetchResources->isChecked());
        dialog.accept();
    }

    // Migration mapping: a stored non-preset minute value lands on
    // the nearest tier, and "Never" keeps it for later.
    settings.setValue(QLatin1String("tabs/suspendTabs"), true);
    settings.setValue(QLatin1String("tabs/suspendTabsMinutes"), 45);
    {
        SettingsDialog dialog;
        QVERIFY(dialog.memorySaverBalanced->isChecked());
        dialog.memorySaverNever->setChecked(true);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("tabs/suspendTabs"))
                 .toBool(), false);
    QCOMPARE(settings.value(QLatin1String("tabs/suspendTabsMinutes"))
                 .toInt(), 45);
    {
        SettingsDialog dialog;
        QVERIFY(dialog.memorySaverNever->isChecked());
        dialog.reject();
    }

    // The page's filter haystack: "performance" and "saver" list it.
    {
        SettingsDialog dialog;
        dialog.pagesFilter->setText(QStringLiteral("saver"));
        QVERIFY(!dialog.pagesList->item(
                     int(SettingsDialog::PerformancePage))->isHidden());
    }

    // Reset restores only this page's keys (the migrated ones
    // included) — a foreign setting survives.
    settings.setValue(QLatin1String("performance/lazyRestoreTabs"), true);
    settings.setValue(QLatin1String("tabs/suspendTabs"), true);
    settings.setValue(QLatin1String("privacy/blockPrefetch"), false);
    settings.setValue(QLatin1String("MainWindow/showSidebar"), true);
    {
        SettingsDialog dialog;
        QVERIFY(dialog.lazyRestoreTabs->isChecked());
        QVERIFY(dialog.prefetchResources->isChecked());
        answerNextBox(QMessageBox::Yes);
        dialog.resetPerformanceButton->click();
        QVERIFY(dialog.hardwareAccel->isChecked());
        QVERIFY(dialog.memorySaverNever->isChecked());
        QVERIFY(!dialog.lazyRestoreTabs->isChecked());
        QVERIFY(!dialog.prefetchResources->isChecked());
        dialog.reject();
    }
    QCOMPARE(settings.value(
                 QLatin1String("performance/lazyRestoreTabs"))
                 .toBool(), false);
    QCOMPARE(settings.value(QLatin1String("tabs/suspendTabs"))
                 .toBool(), false);
    QCOMPARE(settings.value(QLatin1String("privacy/blockPrefetch"))
                 .toBool(), true);
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSidebar"))
                 .toBool(), true);
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("performance"));
    settings.remove(QLatin1String("tabs/suspendTabs"));
    settings.remove(QLatin1String("tabs/suspendTabsMinutes"));
    settings.remove(QLatin1String("privacy/blockPrefetch"));
}

// PREFUI05: Energy Saver — radio persistence, the sysfs battery seam
// driving live activation, the forced suspend clock, and the honest
// no-battery degrade.
void tst_SettingsDialog::energySaver()
{
    QSettings settings;
    settings.remove(QLatin1String("performance"));

    // Fake sysfs tree: one battery on AC power.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    auto writeAttr = [&root](const QString &rel, const QByteArray &data) {
        const QString path = root.filePath(rel);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(data);
        file.close();
    };
    writeAttr(QLatin1String("BAT0/type"), "Battery\n");
    writeAttr(QLatin1String("BAT0/status"), "Charging\n");
    writeAttr(QLatin1String("BAT0/capacity"), "64\n");
    writeAttr(QLatin1String("AC/type"), "Mains\n");
    writeAttr(QLatin1String("AC/online"), "1\n");

    EnergySaver *saver = EnergySaver::instance();
    EnergySaver::setPowerSupplyRootForTesting(root.path());
    QVERIFY(saver->hasBattery());
    QVERIFY(!saver->onBattery());
    QCOMPARE(saver->batteryPercent(), 64);

    // Default is Never; the radios persist a round trip.
    {
        SettingsDialog dialog;
        QVERIFY(dialog.energySaverNever->isChecked());
        QVERIFY(!dialog.energySaverStatus->text().isEmpty());
        dialog.energySaverOnBattery->setChecked(true);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("performance/energySaver"))
                 .toInt(), int(EnergySaver::OnBattery));
    {
        SettingsDialog dialog;
        QVERIFY(dialog.energySaverOnBattery->isChecked());
        dialog.reject();
    }

    // The suspend clock: Memory Saver's own pick applies while the
    // saver is off.
    TabWidget tabs;
    QCOMPARE(tabs.suspendIdleIntervalMs(), qint64(0));
    settings.setValue(QLatin1String("tabs/suspendTabs"), true);
    settings.setValue(QLatin1String("tabs/suspendTabsMinutes"), 60);
    tabs.loadSettings();
    QCOMPARE(tabs.suspendIdleIntervalMs(), qint64(60 * 60 * 1000));

    // AC -> battery flips the saver live and pins the clock to the
    // 10-minute cap, overriding the 60-minute pick.
    QSignalSpy spy(saver, &EnergySaver::activeChanged);
    QVERIFY(!saver->active());
    writeAttr(QLatin1String("AC/online"), "0\n");
    writeAttr(QLatin1String("BAT0/status"), "Discharging\n");
    saver->refreshBatteryState();
    QVERIFY(saver->onBattery());
    QVERIFY(saver->active());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(tabs.suspendIdleIntervalMs(), qint64(10 * 60 * 1000));

    // Back on AC it releases and the configured pick returns.
    writeAttr(QLatin1String("AC/online"), "1\n");
    writeAttr(QLatin1String("BAT0/status"), "Charging\n");
    saver->refreshBatteryState();
    QVERIFY(!saver->active());
    QCOMPARE(spy.count(), 2);
    QCOMPARE(tabs.suspendIdleIntervalMs(), qint64(60 * 60 * 1000));

    // The 20% mode is level-driven.
    settings.setValue(QLatin1String("performance/energySaver"),
                      int(EnergySaver::BelowThreshold));
    saver->loadSettings();
    QVERIFY(!saver->active());
    writeAttr(QLatin1String("BAT0/capacity"), "19\n");
    saver->refreshBatteryState();
    QVERIFY(saver->active());
    writeAttr(QLatin1String("BAT0/capacity"), "21\n");
    saver->refreshBatteryState();
    QVERIFY(!saver->active());

    // Always and Never need no battery state at all.
    settings.setValue(QLatin1String("performance/energySaver"),
                      int(EnergySaver::Always));
    saver->loadSettings();
    QVERIFY(saver->active());
    QCOMPARE(tabs.suspendIdleIntervalMs(), qint64(10 * 60 * 1000));
    settings.setValue(QLatin1String("performance/energySaver"),
                      int(EnergySaver::Never));
    saver->loadSettings();
    QVERIFY(!saver->active());

    // No-battery degrade: conditioned modes behave like Never and the
    // page says so.
    QTemporaryDir empty;
    QVERIFY(empty.isValid());
    EnergySaver::setPowerSupplyRootForTesting(empty.path());
    QVERIFY(!saver->hasBattery());
    settings.setValue(QLatin1String("performance/energySaver"),
                      int(EnergySaver::OnBattery));
    saver->loadSettings();
    QVERIFY(!saver->active());
    {
        SettingsDialog dialog;
        QVERIFY(dialog.energySaverStatus->text().contains(
            QLatin1String("No battery")));
    }

    // With a battery the status line names the charge level.
    EnergySaver::setPowerSupplyRootForTesting(root.path());
    {
        SettingsDialog dialog;
        QVERIFY(dialog.energySaverStatus->text().contains(
            QLatin1String("21%")));
    }

    // Reset covers energySaver via the performance group.
    settings.setValue(QLatin1String("performance/energySaver"),
                      int(EnergySaver::Always));
    {
        SettingsDialog dialog;
        QVERIFY(dialog.energySaverAlways->isChecked());
        answerNextBox(QMessageBox::Yes);
        dialog.resetPerformanceButton->click();
        QVERIFY(dialog.energySaverNever->isChecked());
        dialog.reject();
    }
    QCOMPARE(settings.value(QLatin1String("performance/energySaver"))
                 .toInt(), int(EnergySaver::Never));

    EnergySaver::setPowerSupplyRootForTesting(QString());
    settings.remove(QLatin1String("performance"));
    settings.remove(QLatin1String("tabs/suspendTabs"));
    settings.remove(QLatin1String("tabs/suspendTabsMinutes"));
    saver->loadSettings();
}

// TPREV01: the Appearance page's 'Show tab previews on hover'
// checkbox persists tabs/tabHoverPreviews — on by default for a
// fresh profile, and the state round-trips through save/reload.
void tst_SettingsDialog::tabHoverPreviews()
{
    QSettings settings;
    settings.remove(QLatin1String("tabs/tabHoverPreviews"));

    // The control lives on the Appearance page (sidebar row 2).
    {
        SettingsDialog dialog;
        QWidget *host = dialog.tabHoverPreviews;
        while (host && host->parentWidget() != dialog.tabWidget)
            host = host->parentWidget();
        QVERIFY(host);
        QCOMPARE(dialog.tabWidget->indexOf(host), 2);

        QVERIFY(dialog.tabHoverPreviews->isChecked());
        dialog.tabHoverPreviews->setChecked(false);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("tabs/tabHoverPreviews"))
                 .toBool(), false);

    {
        SettingsDialog dialog;
        QVERIFY(!dialog.tabHoverPreviews->isChecked());
        dialog.tabHoverPreviews->setChecked(true);
        dialog.accept();
    }
    QCOMPARE(settings.value(QLatin1String("tabs/tabHoverPreviews"))
                 .toBool(), true);

    settings.remove(QLatin1String("tabs/tabHoverPreviews"));
}

// PREFUI06: the Appearance page rendered its label column mid-page —
// a column shared by the 'Minimum Font size:' checkbox and the long
// 'Window theme' label pushed every label's text right while the
// checkbox/group rows anchored at the inset. Labels must paint their
// text at the page inset, the label column must be sized by the
// labels alone, and the font-name/note fields must not center text.
void tst_SettingsDialog::appearanceLeftAnchored()
{
    SettingsDialog dialog;
    dialog.resize(1400, 800);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.pagesList->setCurrentRow(2);
    dialog.tabWidget->setCurrentIndex(2);
    qApp->processEvents();

    QScrollArea *area = qobject_cast<QScrollArea *>(
        dialog.tabWidget->widget(2));
    QVERIFY(area);
    QWidget *page = area->widget();
    QVERIFY(page);

    auto textX = [&page](QLabel *l) {
        const int x = l->mapTo(page, QPoint(0, 0)).x();
        const int tw = l->fontMetrics().horizontalAdvance(l->text());
        const Qt::Alignment a = l->alignment();
        if (a & Qt::AlignHCenter)
            return x + (l->width() - tw) / 2;
        if (a & Qt::AlignRight)
            return x + l->width() - tw;
        return x;
    };

    const QList<QLabel *> pageLabels = {
        dialog.iconThemeLabel, dialog.label_5,
        dialog.label_6, dialog.themeLabel
    };
    for (QLabel *l : pageLabels) {
        QVERIFY2(textX(l) <= 20,
                 qPrintable(QStringLiteral(
                     "%1 text starts at %2 — label column drifts")
                     .arg(l->objectName()).arg(textX(l))));
    }

    // The label column is sized by the labels alone — no checkbox or
    // other non-label row may inflate it past the widest label hint.
    int maxLabelHint = 0;
    for (QLabel *l : pageLabels)
        maxLabelHint = qMax(maxLabelHint, l->sizeHint().width());
    QVERIFY2(dialog.iconThemeLabel->width() <= maxLabelHint,
             qPrintable(QStringLiteral(
                 "label column %1 wider than widest label hint %2")
                 .arg(dialog.iconThemeLabel->width())
                 .arg(maxLabelHint)));

    // Sidebar group's inner labels anchor inside the group too.
    for (QLabel *l : {dialog.sidebarDockAreaLabel,
                      dialog.sidebarPanelsLabel}) {
        QVERIFY2(!(l->alignment() & Qt::AlignRight),
                 qPrintable(l->objectName()));
        QVERIFY2(textX(l) <= 30,
                 qPrintable(QStringLiteral(
                     "%1 text starts at %2")
                     .arg(l->objectName()).arg(textX(l))));
    }

    // Field text reads like a line edit — no centered painting.
    QVERIFY(!(dialog.standardLabel->alignment() & Qt::AlignHCenter));
    QVERIFY(!(dialog.fixedLabel->alignment() & Qt::AlignHCenter));
    QVERIFY(!(dialog.themeNoteLabel->alignment() & Qt::AlignHCenter));
    QVERIFY(dialog.themeNoteLabel->mapTo(page, QPoint(0, 0)).x() <= 20);

    // Widening the dialog must not drift the anchored block.
    dialog.resize(1600, 800);
    qApp->processEvents();
    for (QLabel *l : pageLabels)
        QVERIFY2(textX(l) <= 20,
                 qPrintable(l->objectName()));
    dialog.close();
}

// PREFUI07: every settings text field caps at a content-class width
// so a wide dialog can't stretch a proxy credential or a home-page
// URL across the page. Classes: 240 short names/credentials/tokens,
// 300 hostnames, 480 URLs and paths. Measured at a 1500px dialog
// where the uncapped fields ran to ~85% of the page width.
void tst_SettingsDialog::fieldWidthCaps()
{
    struct Capped { QLineEdit *edit; int cap; };
    SettingsDialog dialog;
    const QList<Capped> fields = {
        {dialog.homeLineEdit, 480},
        {dialog.engineNameEdit, 240},
        {dialog.engineNicknameEdit, 240},
        {dialog.engineUrlEdit, 480},
        {dialog.engineSuggestUrlEdit, 480},
        {dialog.engineImageUrlEdit, 480},
        {dialog.enginePostParamsEdit, 480},
        {dialog.engineImagePostParamsEdit, 480},
        {dialog.shortcutBookmarksToken, 240},
        {dialog.shortcutHistoryToken, 240},
        {dialog.shortcutTabsToken, 240},
        {dialog.secureDnsServer, 480},
        {dialog.proxyHostName, 300},
        {dialog.proxyUserName, 240},
        {dialog.proxyPassword, 240},
        {dialog.userStyleSheet, 480},
        {dialog.downloadsLocation, 480},
        {dialog.externalDownloadPath, 480}
    };
    for (const Capped &f : fields) {
        QCOMPARE(f.edit->maximumWidth(), f.cap);
        // Capped, not fixed — fields still shrink with a narrow window.
        QVERIFY(f.edit->sizePolicy().horizontalPolicy()
                != QSizePolicy::Fixed);
    }

    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    dialog.resize(1500, 900);
    qApp->processEvents();

    // A field's geometry is only real while its page is current —
    // walk up to the scroll area to find which stack index hosts it.
    auto showPageOf = [&dialog](QWidget *w) {
        while (w->parentWidget()
               && w->parentWidget() != dialog.tabWidget)
            w = w->parentWidget();
        const int page = dialog.tabWidget->indexOf(w);
        if (page >= 0) {
            dialog.tabWidget->setCurrentIndex(page);
            qApp->processEvents();
        }
        return page;
    };

    for (const Capped &f : fields) {
        QVERIFY2(showPageOf(f.edit) >= 0,
                 qPrintable(f.edit->objectName()));
        QVERIFY2(f.edit->width() <= f.cap,
                 qPrintable(QStringLiteral(
                     "%1 width %2 exceeds cap %3")
                     .arg(f.edit->objectName())
                     .arg(f.edit->width()).arg(f.cap)));
        QVERIFY(f.edit->width() > 0);
    }

    // The browse buttons sit directly beside their capped fields —
    // no dead band where the old stretch used to be.
    const QList<QPair<QLineEdit *, QPushButton *>> rows = {
        {dialog.userStyleSheet, dialog.styleSheetBrowseButton},
        {dialog.downloadsLocation, dialog.downloadDirectoryButton},
        {dialog.externalDownloadPath, dialog.externalDownloadBrowse}
    };
    for (const auto &row : rows) {
        QVERIFY2(showPageOf(row.first) >= 0,
                 qPrintable(row.first->objectName()));
        const int gap = row.second->x()
                - (row.first->x() + row.first->width());
        QVERIFY2(gap <= 40,
                 qPrintable(QStringLiteral(
                     "%1..%2 gap %3 — button floated off the field")
                     .arg(row.first->objectName(),
                          row.second->objectName())
                     .arg(gap)));
    }

    // Already-capped combos on the swept pages still render capped.
    for (QComboBox *combo : {dialog.proxyType, dialog.secureDnsMode}) {
        QVERIFY2(showPageOf(combo) >= 0,
                 qPrintable(combo->objectName()));
        QCOMPARE(combo->sizePolicy().horizontalPolicy(),
                 QSizePolicy::Maximum);
        QVERIFY2(combo->width() <= combo->sizeHint().width() + 4,
                 qPrintable(combo->objectName()));
    }

    // Narrow case: at 700px the Proxy page fields stay inside their
    // caps and nothing forces a horizontal scrollbar.
    dialog.resize(700, 900);
    qApp->processEvents();
    QVERIFY(showPageOf(dialog.proxyPassword) >= 0);
    QVERIFY2(dialog.proxyPassword->width() <= 240,
             qPrintable(QString::number(dialog.proxyPassword->width())));
    QScrollArea *area = qobject_cast<QScrollArea *>(
        dialog.tabWidget->currentWidget());
    QVERIFY(area);
    QCOMPARE(area->horizontalScrollBar()->maximum(), 0);
    dialog.close();
}

QTEST_MAIN(tst_SettingsDialog)
#include "tst_settingsdialog.moc"
