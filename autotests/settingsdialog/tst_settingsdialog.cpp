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
    void suggestionsCheckbox();
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
    QCOMPARE(settings.value(QLatin1String("proxy/enabled")).toBool(), true);

    // A fresh dialog must read every value back through loadFromSettings.
    {
        SettingsDialog reloaded;
        QCOMPARE(reloaded.homeLineEdit->text(),
                 QLatin1String("http://home.example.com/"));
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
