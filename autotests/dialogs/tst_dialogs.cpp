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

// COV04: dialog-level coverage for the small preference/management
// dialogs — these widgets are exercised at model + slot level offscreen
// (acceptModal()/rejectModal() dismiss the nested exec() loops).

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <QtNetwork/QtNetwork>
#include <qstandarditemmodel.h>
#include <qwebengineprofile.h>

#include "aboutdialog.h"
#include "clearprivatedata.h"
#include "cookiedialog.h"
#include "cookieexceptionsdialog.h"
#include "cookieexceptionsmodel.h"
#include "cookiejar.h"
#include "autofilldialog.h"
#include "autofillmanager.h"
#include "bookmarksdialog.h"
#include "bookmarksmanager.h"
#include "bookmarknode.h"
#include "acceptlanguagedialog.h"
#include "adblockdialog.h"
#include "adblockmanager.h"
#include "adblocksubscription.h"
#include "adblockrule.h"
#include "opensearchdialog.h"
#include "opensearchenginemodel.h"
#include "opensearchengineaction.h"
#include "opensearchmanager.h"
#include "opensearchengine.h"
#include "toolbarsearch.h"
#include "edittableview.h"
#include "useragentmenu.h"
#include "webpage.h"
#include "modeltest.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_Dialogs : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void aboutDialog();
    void clearPrivateData();
    void cookieDialog();
    void cookieExceptionsDialog();
    void openSearchEngineModel();
    void openSearchDialog();
    void openSearchEngineAction();
    void autoFillDialog();
    void bookmarksDialog();
    void acceptLanguageDialog();
    void adBlockDialog();
    void editTableView();
    void userAgentMenu();
};

void tst_Dialogs::initTestCase()
{
    QCoreApplication::setApplicationName("tst_dialogs");
    QSettings settings;
    settings.clear();
    // Keep AdBlockManager away from live subscription URLs — point the
    // stored list at a dead local file (same trick as tst_schemehandlers).
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-cov04.txt"
            "&title=DeadList"));
}

void tst_Dialogs::cleanup()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("QDialog"))
            widget->close();
    }
}

// AboutDialog keeps its Ui private — invokeMethod still reaches the
// slots; each one exec()s a displayFile() dialog the timer accepts.
void tst_Dialogs::aboutDialog()
{
    AboutDialog dialog;
    acceptModal();
    QVERIFY(QMetaObject::invokeMethod(&dialog, "authorsButtonClicked"));
    acceptModal();
    QVERIFY(QMetaObject::invokeMethod(&dialog, "licenseButtonClicked"));
}

// accept() with every checkbox ticked runs the full clearing fan-out
// (history, downloads, searches, cookies, caches, icons).
void tst_Dialogs::clearPrivateData()
{
    ClearPrivateData dialog;
    dialog.accept();
    QSettings settings;
    QVERIFY(settings.contains(QLatin1String("clearprivatedata/browsingHistory")));
}

void tst_Dialogs::cookieDialog()
{
    CookieJar jar;
    QNetworkCookie cookie("session_id", "abc123");
    cookie.setDomain(QLatin1String("dialog.example.com"));
    // setCookies() updates the jar's mirror synchronously (unlike
    // setCookiesFromUrl, which waits on the WebEngine store signals).
    jar.setCookies(QList<QNetworkCookie>() << cookie);

    CookieDialog dialog(&jar);
    QVERIFY(dialog.cookiesTable->model()->rowCount() >= 1);

    // Filter + select a row, then addRule() spawns the exceptions dialog.
    dialog.search->setText(QLatin1String("dialog.example.com"));
    dialog.cookiesTable->selectRow(0);
    acceptModal();
    dialog.addRuleButton->click();
}

void tst_Dialogs::cookieExceptionsDialog()
{
    CookieJar jar;
    CookieExceptionsDialog dialog(&jar);
    dialog.setDomainName(QLatin1String("blocked.example.com"));
    QVERIFY(dialog.blockButton->isEnabled());
    dialog.blockButton->click();

    dialog.setDomainName(QLatin1String("allowed.example.com"));
    dialog.allowButton->click();
    dialog.setDomainName(QLatin1String("session.example.com"));
    dialog.allowForSessionButton->click();

    dialog.accept();
    QVERIFY(jar.blockedCookies().contains(QLatin1String("blocked.example.com")));
    QVERIFY(jar.allowedCookies().contains(QLatin1String("allowed.example.com")));
    QVERIFY(jar.allowForSessionCookies().contains(QLatin1String("session.example.com")));
}

// Pure model coverage: flags, data roles, keyword setData, removal.
void tst_Dialogs::openSearchEngineModel()
{
    OpenSearchManager manager;
    manager.restoreDefaults();
    QVERIFY(manager.enginesCount() > 0);

    OpenSearchEngineModel model(&manager);
    ModelTest tester(&model);
    Q_UNUSED(tester);

    QCOMPARE(model.columnCount(), 2);
    QCOMPARE(model.rowCount(), manager.enginesCount());
    QVERIFY(!model.index(0, 0).data(Qt::DisplayRole).toString().isEmpty());
    QVERIFY(model.index(0, 0).data(Qt::ToolTipRole).toString()
                .contains(QLatin1String("Description")));
    QVERIFY(!model.flags(model.index(0, 0)).testFlag(Qt::ItemIsEditable));
    QVERIFY(model.flags(model.index(0, 1)).testFlag(Qt::ItemIsEditable));
    QCOMPARE(model.headerData(0, Qt::Horizontal).toString(),
             QLatin1String("Name"));

    OpenSearchEngine *engine = manager.engine(manager.allEnginesNames().first());
    QVERIFY(engine);
    QVERIFY(model.setData(model.index(0, 1), QLatin1String("kw1, kw2")));
    QVERIFY(manager.keywordsForEngine(engine).contains(QLatin1String("kw1")));
    // Wrong column / role are rejected.
    QVERIFY(!model.setData(model.index(0, 0), QLatin1String("nope")));
    QVERIFY(!model.setData(model.index(0, 1), QLatin1String("x"), Qt::DisplayRole));
}

void tst_Dialogs::openSearchDialog()
{
    OpenSearchDialog dialog;
    QVERIFY(dialog.m_tableView->model()->rowCount() > 0);

    // addButtonClicked opens a file dialog — reject it.
    rejectModal();
    QVERIFY(QMetaObject::invokeMethod(&dialog, "addButtonClicked"));

    // restoreButtonClicked reloads the bundled defaults.
    QVERIFY(QMetaObject::invokeMethod(&dialog, "restoreButtonClicked"));

    // Deleting the last remaining engine raises the error box; with more
    // than one row it goes through EditTableView::removeSelected.
    while (dialog.m_tableView->model()->rowCount() > 1) {
        dialog.m_tableView->selectRow(0);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "deleteButtonClicked"));
    }
    acceptModal();
    QVERIFY(QMetaObject::invokeMethod(&dialog, "deleteButtonClicked"));
    QCOMPARE(dialog.m_tableView->model()->rowCount(), 1);

    // Leave the shared manager usable for the rest of the suite.
    QVERIFY(QMetaObject::invokeMethod(&dialog, "restoreButtonClicked"));
}

void tst_Dialogs::openSearchEngineAction()
{
    OpenSearchEngine engine;
    engine.setName(QLatin1String("ActionEngine"));
    OpenSearchEngineAction action(&engine);
    QCOMPARE(action.text(), QLatin1String("ActionEngine"));

    engine.setImage(QImage(2, 2, QImage::Format_ARGB32));
    QVERIFY(!action.icon().isNull());
}

// AutoFillModel reflects the manager's form list; the dialog wires the
// EditTableView removal buttons.
void tst_Dialogs::autoFillDialog()
{
    AutoFillManager *manager = AutoFillManager::instance();
    const QList<AutoFillManager::Form> saved = manager->forms();

    AutoFillManager::Form form;
    form.url = QUrl(QLatin1String("http://fill.example.com/login"));
    form.name = QLatin1String("loginform");
    form.hasAPassword = true;
    form.elements << AutoFillManager::Element(QLatin1String("user"), QLatin1String("alice"))
                  << AutoFillManager::Element(QLatin1String("pass"), QLatin1String("secret"));
    manager->setForms(QList<AutoFillManager::Form>() << form);

    AutoFillDialog dialog;
    QCOMPARE(dialog.tableView->model()->rowCount(), 1);
    QCOMPARE(dialog.tableView->model()->index(0, 0).data().toString(),
             QLatin1String("fill.example.com"));
    QCOMPARE(dialog.tableView->model()->index(0, 1).data().toString(),
             QLatin1String("alice"));

    // removeAll clears through to the manager.
    dialog.removeAllButton->click();
    QCOMPARE(manager->forms().count(), 0);

    manager->setForms(saved);
}

void tst_Dialogs::bookmarksDialog()
{
    BookmarksManager *manager = BookmarksManager::instance();
    BookmarkNode *bookmark = new BookmarkNode(BookmarkNode::Bookmark);
    bookmark->title = QLatin1String("DialogBookmark");
    bookmark->url = QLatin1String("http://bookmark.example.com/");
    manager->addBookmark(manager->menu(), bookmark);

    BookmarksDialog dialog;
    QVERIFY(dialog.tree->model()->rowCount() > 0);
    QVERIFY(dialog.removeButton);

    manager->removeBookmark(bookmark);
}

void tst_Dialogs::acceptLanguageDialog()
{
    // Static helpers first — httpString quality-decays the list.
    const QStringList defaults = AcceptLanguageDialog::defaultAcceptList();
    const QByteArray header = AcceptLanguageDialog::httpString(
        QStringList() << QLatin1String("English (United States) [en-us]")
                      << QLatin1String("fr"));
    QVERIFY(header.startsWith("en-us"));
    QVERIFY(header.contains(QLatin1String(";q=")));

    AcceptLanguageDialog dialog;
    QVERIFY(dialog.listView->model()->rowCount() >= 0);

    // Add a language through the combo + button path; walk the combo
    // until an entry that is not already in the model is found.
    const int before = dialog.listView->model()->rowCount();
    for (int i = 0; i < dialog.addComboBox->count() && i < 20; ++i) {
        dialog.addComboBox->setCurrentIndex(i);
        dialog.addButton->click();
        if (dialog.listView->model()->rowCount() > before)
            break;
    }

    // Move the last row up and back down, then remove it.
    const int rows = dialog.listView->model()->rowCount();
    if (rows > 0) {
        dialog.listView->setCurrentIndex(dialog.listView->model()->index(rows - 1, 0));
        dialog.moveUpButton->click();
        dialog.moveDownButton->click();
        dialog.removeButton->click();
    }

    dialog.accept();
    QVERIFY(!AcceptLanguageDialog::acceptLanguages().isEmpty() || defaults.isEmpty());
}

void tst_Dialogs::adBlockDialog()
{
    AdBlockDialog dialog;
    QVERIFY(dialog.treeView->model()->rowCount() >= 0);
    dialog.adblockCheckBox->setChecked(true);

    // The InstantPopup action menu builds its contents on aboutToShow.
    QMenu *menu = dialog.actionToolButton->menu();
    QVERIFY(menu);
    emit menu->aboutToShow();
    QVERIFY(menu->actions().count() >= 4);

    // addCustomRule lands in the "Custom Rules" subscription.
    AdBlockSubscription *custom = AdBlockManager::instance()->customRules();
    const int rulesBefore = custom->allRules().count();
    dialog.addCustomRule(QLatin1String("||cov04-dialog.invalid^"));
    QCOMPARE(custom->allRules().count(), rulesBefore + 1);
    custom->removeRule(rulesBefore); // customRules keeps insertion order
}

void tst_Dialogs::editTableView()
{
    QStandardItemModel model;
    for (int i = 0; i < 3; ++i)
        model.appendRow(new QStandardItem(QString::number(i)));

    EditTableView view;
    view.setModel(&model);
    view.show();

    // Delete key removes the selected row.
    view.selectRow(1);
    QTest::keyClick(&view, Qt::Key_Delete);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.item(1)->text(), QLatin1String("2"));

    // removeAll empties the model; calls on an empty model are inert.
    view.selectRow(0);
    QTest::keyClick(&view, Qt::Key_Delete);
    QTest::keyClick(&view, Qt::Key_Delete);
    QCOMPARE(model.rowCount(), 0);
    QTest::keyClick(&view, Qt::Key_Delete);
    view.removeAll();
}

void tst_Dialogs::userAgentMenu()
{
    UserAgentMenu menu;
    emit menu.aboutToShow(); // populateMenu
    QVERIFY(menu.actions().count() > 2);

    const QString defaultAgent = WebPage::userAgent();

    // Triggering a parsed entry sets the profile user agent.
    QAction *custom = 0;
    const QList<QAction *> menuActions = menu.actions();
    for (QAction *action : menuActions) {
        if (!action->data().toString().isEmpty()) {
            custom = action;
            break;
        }
    }
    QVERIFY(custom);
    custom->trigger();
    QCOMPARE(WebPage::userAgent(), custom->data().toString());

    // "Other..." opens an input dialog — reject it.
    rejectModal();
    menu.actions().last()->trigger();

    // "Default" restores the bundled UA.
    menu.actions().first()->trigger();
    QCOMPARE(WebPage::userAgent(), defaultAgent);
}

QTEST_MAIN(tst_Dialogs)
#include "tst_dialogs.moc"
