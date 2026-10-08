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
#include <qcheckbox.h>
#include <qlabel.h>
#include <qpushbutton.h>
#include <qstandarditemmodel.h>
#include <qtreewidget.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtemporarydir.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebengineview.h>

#include "aboutdialog.h"
#include "browserprofile.h"
#include "browsertheme.h"
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
#include "settings.h"
#include "toolbarsearch.h"
#include "edittableview.h"
#include "useragentmenu.h"
#include "webpage.h"
#include "modeltest.h"
#include "qtest_arora.h"
#include "qtry.h"

// SEC12 helper: does any file under rootPath contain the needle?
// Files >= 4 MiB are skipped — nothing we seed gets that large.
static bool storageTreeContains(const QString &rootPath, const QByteArray &needle)
{
    const QFileInfoList entries = QDir(rootPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink())
            continue;
        if (entry.isDir()) {
            if (storageTreeContains(entry.absoluteFilePath(), needle))
                return true;
        } else if (entry.size() < 4 * 1024 * 1024) {
            QFile file(entry.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly) && file.readAll().contains(needle))
                return true;
        }
    }
    return false;
}

// Recursive copy so the deferred wipe can be exercised on a real
// seeded tree without touching the live profile's (deleting the
// real tree under the running browser is exactly what HARD01 found
// to wedge it).
static bool copyStorageTree(const QString &fromPath, const QString &toPath)
{
    const QDir from(fromPath);
    const QDir to(toPath);
    const QFileInfoList entries = from.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink())
            continue;
        const QString dest = to.absoluteFilePath(entry.fileName());
        if (entry.isDir()) {
            if (!to.mkpath(entry.fileName())
                || !copyStorageTree(entry.absoluteFilePath(), dest))
                return false;
        } else if (!QFile::copy(entry.absoluteFilePath(), dest)) {
            return false;
        }
    }
    return true;
}

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
    void clearSiteData();
    void storagePermissions();
    void deferredSiteWipe();
    void colorSchemeApply();
    void dialogsRenderInBothSchemes();
    void dialogButtonPolish();
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

    // Copyright lines and the repo link live in the author label.
    QLabel *author = dialog.findChild<QLabel *>(QLatin1String("author"));
    QVERIFY(author);
    QVERIFY(author->text().contains(QLatin1String("Benjamin C. Meyer")));
    QVERIFY(author->text().contains(QLatin1String("Janos Szenfner")));
    QVERIFY(author->text().contains(
        QLatin1String("href=\"https://github.com/janos-szenfner/arora\"")));

    // The Authors button shows :AUTHORS — the maintainer is listed first.
    QFile authorsFile(QLatin1String(":AUTHORS"));
    QVERIFY(authorsFile.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(authorsFile.readLine())
            .startsWith(QLatin1String("Janos Szenfner")));
    authorsFile.close();

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

    QCOMPARE(model.columnCount(), 3);
    QCOMPARE(model.rowCount(), manager.enginesCount());
    QVERIFY(!model.index(0, 0).data(Qt::DisplayRole).toString().isEmpty());
    QVERIFY(model.index(0, 0).data(Qt::ToolTipRole).toString()
                .contains(QLatin1String("Description")));
    QVERIFY(!model.flags(model.index(0, 0)).testFlag(Qt::ItemIsEditable));
    QVERIFY(model.flags(model.index(0, 1)).testFlag(Qt::ItemIsEditable));
    QCOMPARE(model.headerData(0, Qt::Horizontal).toString(),
             QLatin1String("Name"));
    QCOMPARE(model.headerData(2, Qt::Horizontal).toString(),
             QLatin1String("Suggestions"));

    // SEC11: the Suggestions column is the per-engine opt-in —
    // checkable only for engines with a suggest endpoint.
    int capableRow = -1, incapableRow = -1;
    const QStringList names = manager.allEnginesNames();
    for (int row = 0; row < names.count(); ++row) {
        OpenSearchEngine *engine = manager.engine(names.at(row));
        if (engine->providesSuggestions() && capableRow == -1)
            capableRow = row;
        if (!engine->providesSuggestions() && incapableRow == -1)
            incapableRow = row;
    }
    QVERIFY(capableRow != -1);
    QVERIFY(incapableRow != -1);
    const QModelIndex capable = model.index(capableRow, 2);
    const QModelIndex incapable = model.index(incapableRow, 2);
    QVERIFY(model.flags(capable).testFlag(Qt::ItemIsUserCheckable));
    QVERIFY(!model.flags(incapable).testFlag(Qt::ItemIsUserCheckable));
    QCOMPARE(capable.data(Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Unchecked));
    QVERIFY(!incapable.data(Qt::CheckStateRole).isValid());

    const QString capableName = names.at(capableRow);
    QVERIFY(model.setData(capable, Qt::Checked, Qt::CheckStateRole));
    QVERIFY(manager.suggestionsEnabledForEngine(capableName));
    QCOMPARE(model.index(capableRow, 2).data(Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Checked));
    // The toggle emits changed() -> full model reset, so re-fetch.
    QVERIFY(model.setData(model.index(capableRow, 2), Qt::Unchecked,
                          Qt::CheckStateRole));
    QVERIFY(!manager.suggestionsEnabledForEngine(capableName));
    // An engine without a suggest endpoint cannot be opted in.
    QVERIFY(!model.setData(model.index(incapableRow, 2), Qt::Checked,
                           Qt::CheckStateRole));

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
    AdBlockManager *manager = AdBlockManager::instance();

    // ADB04: one checkable row per subscription — remote lists never
    // expand into per-rule rows, so a 60k-rule list cannot stall the
    // dialog or show untoggleable comment/metadata rows.
    QCOMPARE(dialog.subscriptionsTree->topLevelItemCount(),
             manager->subscriptions().count());
    for (int i = 0; i < dialog.subscriptionsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = dialog.subscriptionsTree->topLevelItem(i);
        QVERIFY(item->flags() & Qt::ItemIsUserCheckable);
    }
    dialog.adblockCheckBox->setChecked(true);

    // The InstantPopup action menu builds its contents on aboutToShow.
    QMenu *menu = dialog.actionToolButton->menu();
    QVERIFY(menu);
    emit menu->aboutToShow();
    QVERIFY(menu->actions().count() >= 4);

    // Toggling a subscription checkbox writes straight through to the
    // subscription and round-trips through QSettings on save.
    AdBlockSubscription *dead = manager->subscriptions().first();
    QTreeWidgetItem *firstItem = dialog.subscriptionsTree->topLevelItem(0);
    QVERIFY(firstItem);
    const bool wasEnabled = dead->isEnabled();
    firstItem->setCheckState(0, wasEnabled ? Qt::Unchecked : Qt::Checked);
    QCOMPARE(dead->isEnabled(), !wasEnabled);
    firstItem->setCheckState(0, wasEnabled ? Qt::Checked : Qt::Unchecked);
    QCOMPARE(dead->isEnabled(), wasEnabled);

    // The file-backed custom subscription is managed through the text
    // pane — Update/Remove stay disabled for it, enabled for remotes.
    AdBlockSubscription *custom = manager->customRules();
    dialog.selectSubscription(custom);
    QVERIFY(!dialog.updateSubscriptionButton->isEnabled());
    QVERIFY(!dialog.removeSubscriptionButton->isEnabled());
    dialog.selectSubscription(dead);
    QVERIFY(dialog.updateSubscriptionButton->isEnabled());
    QVERIFY(dialog.removeSubscriptionButton->isEnabled());

    // addCustomRule lands in the "Custom Rules" subscription and is
    // reflected in the text pane.
    const int rulesBefore = custom->allRules().count();
    dialog.addCustomRule(QLatin1String("||cov04-dialog.invalid^"));
    QCOMPARE(custom->allRules().count(), rulesBefore + 1);
    QVERIFY(dialog.customRulesEdit->toPlainText().contains(
        QLatin1String("||cov04-dialog.invalid^")));

    // The debounced edit path commits live without closing the dialog.
    dialog.customRulesEdit->setPlainText(QLatin1String(
        "||adb04.invalid^\n! a comment\n@@||adb04-ok.invalid^"));
    QTRY_COMPARE(custom->allRules().count(), 3);
    QCOMPARE(custom->allRules().at(0).filter(),
             QLatin1String("||adb04.invalid^"));
    QCOMPARE(custom->allRules().at(1).filter(),
             QLatin1String("! a comment"));
    QVERIFY(custom->allRules().at(2).isException());

    // done() commits a pending edit that never hit the debounce.
    dialog.customRulesEdit->setPlainText(QLatin1String("||adb04-done.invalid^"));
    dialog.done(QDialog::Accepted);
    QCOMPARE(custom->allRules().count(), 1);
    QCOMPARE(custom->allRules().at(0).filter(),
             QLatin1String("||adb04-done.invalid^"));

    // The opt-in rules viewer lists the selected subscription's rules
    // read-only; the search box narrows the displayed lines.
    dialog.selectSubscription(custom);
    QVERIFY(dialog.rulesView->toPlainText().isEmpty()); // opt-in, off by default
    dialog.rulesGroup->setChecked(true);
    QVERIFY(dialog.rulesView->isReadOnly());
    QVERIFY(dialog.rulesView->toPlainText().contains(
        QLatin1String("||adb04-done.invalid^")));
    dialog.search->setText(QLatin1String("nomatch"));
    QVERIFY(dialog.rulesView->toPlainText().isEmpty());
    dialog.search->setText(QString());
    QVERIFY(dialog.rulesView->toPlainText().contains(
        QLatin1String("||adb04-done.invalid^")));

    // Leave the shared singleton's custom list empty for later tests.
    custom->setRules(QList<AdBlockRule>());
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
    QAction *custom = nullptr;
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

// SEC12: DOM storage is what trackers actually use, and
// clearHttpCache() never touches it.  Seed localStorage/IndexedDB and
// a cookie through a real page on the normal profile, run the
// dialog's clear path, then prove the data is gone from the live
// profile AND from the on-disk storage tree.
void tst_Dialogs::clearSiteData()
{
    CookieJar *jar = CookieJar::instance(BrowserProfile::normalProfile());
    QWebEngineProfile *profile = jar->profile();
    const QString storagePath = profile->persistentStoragePath();
    QVERIFY(!storagePath.isEmpty());

    // Loopback origin — the first hit serves a seeding page, later
    // hits a plain one so re-loading does not re-seed.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    connect(&server, &QTcpServer::newConnection, this, [&server, &requests] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                             [socket, &requests] {
                socket->readAll();
                const QByteArray script = ++requests == 1
                    ? QByteArrayLiteral(
                        "<script>localStorage.setItem('sec12key','sec12val');"
                        "var r=indexedDB.open('sec12db',1);"
                        "r.onupgradeneeded=function(e){"
                        "e.target.result.createObjectStore('s')};"
                        "document.cookie='sec12cookie=1; path=/';</script>")
                    : QByteArrayLiteral("");
                const QByteArray body =
                    QByteArrayLiteral("<html><body>") + script +
                    QByteArrayLiteral("done</body></html>");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                              "Content-Length: "
                              + QByteArray::number(body.size())
                              + "\r\n\r\n" + body);
            });
        }
    });
    const QUrl pageUrl(QStringLiteral("http://127.0.0.1:%1/")
                       .arg(server.serverPort()));

    QWebEngineView view;
    QWebEnginePage *page = new QWebEnginePage(profile, &view);
    view.setPage(page);
    QSignalSpy loadedSpy(page, &QWebEnginePage::loadFinished);
    view.load(pageUrl);
    QVERIFY(loadedSpy.wait(15000));

    QVariant seeded;
    page->runJavaScript(QStringLiteral("String(localStorage.getItem('sec12key'))"),
                        [&seeded](const QVariant &v) { seeded = v; });
    QTRY_VERIFY(seeded.isValid());
    QCOMPARE(seeded.toString(), QLatin1String("sec12val"));

    // The cookie reaches the profile's jar asynchronously.
    const auto hasSeededCookie = [jar] {
        const QList<QNetworkCookie> cookies = jar->cookies();
        for (const QNetworkCookie &cookie : cookies) {
            if (cookie.name() == "sec12cookie")
                return true;
        }
        return false;
    };
    QTRY_VERIFY(hasSeededCookie());

    // Chromium commits localStorage to its leveldb lazily (several
    // seconds — under a loaded make-check run the commit interval can
    // stretch well past 20s) — wait until the seeded value is actually
    // on disk so the filesystem wipe is genuinely exercised.
    bool onDisk = false;
    for (int ms = 0; ms < 90000 && !onDisk; ms += 250) {
        QTest::qWait(250);
        onDisk = storageTreeContains(storagePath, QByteArrayLiteral("sec12val"));
    }
    QVERIFY2(onDisk, "localStorage never reached the profile storage tree");

    // HARD01: a navigation started while the dialog's async profile
    // clears are in flight can lose its transaction — the wedged load
    // never finishes (the profile itself is unharmed; a retry loads
    // instantly).  Drain the only completion signal Qt exposes, plus
    // a short grace for the page-side sweep and cookie delete, before
    // re-navigating below.
    QSignalSpy cacheCleared(profile, &QWebEngineProfile::clearHttpCacheCompleted);
    ClearPrivateData dialog;
    const QList<QCheckBox*> boxes = dialog.findChildren<QCheckBox*>();
    QVERIFY(!boxes.isEmpty());
    for (QCheckBox *box : boxes)
        box->setChecked(true);
    dialog.accept();
    if (cacheCleared.isEmpty())
        QVERIFY(cacheCleared.wait(15000));
    QTest::qWait(300);

    // The open page's live storage area was emptied by the sweep.
    QVariant cleared;
    page->runJavaScript(QStringLiteral("String(localStorage.getItem('sec12key'))"),
                        [&cleared](const QVariant &v) { cleared = v; });
    QTRY_VERIFY(cleared.isValid());
    QCOMPARE(cleared.toString(), QLatin1String("null"));

    // A fresh page on the same origin sees nothing either — the
    // browser-process storage is empty, not just the tab's.
    QWebEngineView secondView;
    QWebEnginePage *secondPage = new QWebEnginePage(profile, &secondView);
    secondView.setPage(secondPage);
    QSignalSpy secondLoaded(secondPage, &QWebEnginePage::loadFinished);
    secondView.load(pageUrl);
    // A load that still stalls has hit the doomed-transaction race —
    // retry once: a retried load that also hangs is a real wedge, a
    // completed one just proves the transaction was orphaned.
    if (!secondLoaded.wait(30000)) {
        qInfo() << "clearSiteData: second load stalled racing the clears"
                << "(server requests:" << requests << ") — retrying";
        secondView.load(pageUrl);
        QVERIFY2(secondLoaded.wait(30000),
                 "same-origin navigation still hung after retry");
    }
    QVariant reread;
    secondPage->runJavaScript(
        QStringLiteral("String(localStorage.getItem('sec12key'))"),
        [&reread](const QVariant &v) { reread = v; });
    QTRY_VERIFY(reread.isValid());
    QCOMPARE(reread.toString(), QLatin1String("null"));

    QVERIFY(!hasSeededCookie());

    // HARD01: deleting any of Chromium's live storage trees wedges
    // the storage services — the next navigation hangs — so the
    // clear leaves the deferred-wipe sentinel for the next profile
    // startup instead of removing them now.
    QVERIFY(QFileInfo::exists(storagePath
            + QLatin1String("/arora-site-wipe.pending")));

    // Simulated restart: the deferred wipe is run on a COPY of the
    // seeded tree (running it on the live tree would itself wedge
    // this still-running profile) — the seeded bytes go with it.
    QTemporaryDir copy;
    QVERIFY(copy.isValid());
    QVERIFY(copyStorageTree(storagePath, copy.path()));
    QVERIFY(BrowserProfile::clearDeferredSiteStorage(copy.path()));
    QVERIFY(!storageTreeContains(copy.path(), QByteArrayLiteral("sec12val")));

    // The storage tree itself must be owner-only.
    QVERIFY(QFileInfo::exists(storagePath));
    const QFile::Permissions nonOwner =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    QVERIFY(!(QFileInfo(storagePath).permissions() & nonOwner));
}

// SEC12: ensureUserOnlyPermissions repairs a loosened profile tree —
// directories end up 0700, files 0600.
void tst_Dialogs::storagePermissions()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    QVERIFY(QDir(tree.path()).mkdir(QLatin1String("sub")));
    const QString filePath = tree.filePath(QLatin1String("sub/loose.txt"));
    {
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    const QFile::Permissions looseDir =
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
        | QFile::ReadGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::ExeOther;
    const QFile::Permissions looseFile =
        QFile::ReadOwner | QFile::WriteOwner
        | QFile::ReadGroup | QFile::ReadOther;
    QVERIFY(QFile::setPermissions(tree.path(), looseDir));
    QVERIFY(QFile::setPermissions(tree.filePath(QLatin1String("sub")), looseDir));
    QVERIFY(QFile::setPermissions(filePath, looseFile));

    QVERIFY(BrowserProfile::ensureUserOnlyPermissions(tree.path()));

    const QFile::Permissions nonOwner =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    QVERIFY(!(QFileInfo(tree.path()).permissions() & nonOwner));
    QVERIFY(!(QFileInfo(tree.filePath(QLatin1String("sub"))).permissions()
              & nonOwner));
    QVERIFY(!(QFileInfo(filePath).permissions() & nonOwner));

    // Conforming trees and missing paths report success too.
    QVERIFY(BrowserProfile::ensureUserOnlyPermissions(tree.path()));
    QVERIFY(BrowserProfile::ensureUserOnlyPermissions(
                tree.filePath(QLatin1String("no-such-dir"))));
}

// HARD01: the deferred half of a site-data clear — the sentinel makes
// clearDeferredSiteStorage() remove the service-coupled trees;
// without it the function must not touch the directory.
void tst_Dialogs::deferredSiteWipe()
{
    QTemporaryDir tree;
    QVERIFY(tree.isValid());
    QVERIFY(QDir(tree.path()).mkdir(QLatin1String("Service Worker")));
    QVERIFY(QDir(tree.path()).mkdir(QLatin1String("Keep Me")));
    QFile tokens(tree.filePath(QLatin1String("Trust Tokens")));
    QVERIFY(tokens.open(QIODevice::WriteOnly));
    tokens.close();

    // No sentinel: nothing is removed.
    QVERIFY(BrowserProfile::clearDeferredSiteStorage(tree.path()));
    QVERIFY(QFileInfo::exists(tree.filePath(QLatin1String("Service Worker"))));

    // With the sentinel the deferred trees and files go, unrelated
    // entries stay and the sentinel itself is consumed.
    QFile sentinel(tree.filePath(QLatin1String("arora-site-wipe.pending")));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.close();
    QVERIFY(BrowserProfile::clearDeferredSiteStorage(tree.path()));
    QVERIFY(!QFileInfo::exists(tree.filePath(QLatin1String("Service Worker"))));
    QVERIFY(!QFileInfo::exists(tree.filePath(QLatin1String("Trust Tokens"))));
    QVERIFY(QFileInfo::exists(tree.filePath(QLatin1String("Keep Me"))));
    QVERIFY(!QFileInfo::exists(tree.filePath(
                QLatin1String("arora-site-wipe.pending"))));
}

// UIP01: ARORA_COLOR_SCHEME drives preferredColorScheme() so the whole
// install/restore path is deterministic under the offscreen QPA (which
// reports Qt::ColorScheme::Unknown).
void tst_Dialogs::colorSchemeApply()
{
    QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));
    const QColor platformWindow =
        QApplication::palette().color(QPalette::Window);

    qputenv("ARORA_COLOR_SCHEME", "dark");
    BrowserTheme::applyColorScheme();
    QVERIFY(BrowserTheme::paletteIsForced());
    QVERIFY(BrowserTheme::isDarkPalette(QApplication::palette()));
    QVERIFY(QApplication::palette().color(QPalette::Text).lightness() > 128);

    qputenv("ARORA_COLOR_SCHEME", "light");
    BrowserTheme::applyColorScheme();
    QVERIFY(!BrowserTheme::paletteIsForced());
    QVERIFY(!BrowserTheme::isDarkPalette(QApplication::palette()));
    QCOMPARE(QApplication::palette().color(QPalette::Window), platformWindow);
    qunsetenv("ARORA_COLOR_SCHEME");
}

// UIP01: dialogs render in both schemes — every visible direct child
// must fit inside the dialog's client area, and the rendered pixels
// must actually follow the palette (a dark grab reads darker than a
// light one).  Returns the mean rendered lightness; out-of-bounds
// children are reported through failures (QVERIFY cannot run inside a
// value-returning helper).
static int renderDialogs(QStringList *failures)
{
    int lightnessSum = 0;
    int count = 0;
    SettingsDialog settings;
    AboutDialog about;
    ClearPrivateData clear;
    QWidget *dialogs[] = { &settings, &about, &clear };
    for (QWidget *dialog : dialogs) {
        dialog->show();
        if (dialog->layout())
            dialog->layout()->activate();
        const QList<QWidget *> children = dialog->findChildren<QWidget *>(
            QString(), Qt::FindDirectChildrenOnly);
        for (QWidget *child : children) {
            if (!child->isVisibleTo(dialog))
                continue;
            if (!dialog->rect().contains(child->geometry()))
                failures->append(QStringLiteral("%1 child %2 out of bounds")
                                 .arg(QLatin1String(dialog->metaObject()->className()),
                                      QLatin1String(child->metaObject()->className())));
        }
        const QImage image = dialog->grab().toImage()
            .scaled(64, 64)
            .convertToFormat(QImage::Format_RGB32);
        if (image.isNull()) {
            failures->append(QStringLiteral("%1 grab returned a null image")
                             .arg(QLatin1String(dialog->metaObject()->className())));
            dialog->hide();
            continue;
        }
        qint64 sum = 0;
        for (int y = 0; y < image.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
                sum += QColor::fromRgba(row[x]).lightness();
        }
        lightnessSum += int(sum / (image.width() * image.height()));
        ++count;
        dialog->hide();
    }
    return count ? lightnessSum / count : 0;
}

void tst_Dialogs::dialogsRenderInBothSchemes()
{
    QStringList failures;

    qputenv("ARORA_COLOR_SCHEME", "light");
    BrowserTheme::applyColorScheme();
    const int lightLevel = renderDialogs(&failures);

    qputenv("ARORA_COLOR_SCHEME", "dark");
    BrowserTheme::applyColorScheme();
    const int darkLevel = renderDialogs(&failures);

    qputenv("ARORA_COLOR_SCHEME", "light");
    BrowserTheme::applyColorScheme();
    qunsetenv("ARORA_COLOR_SCHEME");

    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1String("; "))));
    QVERIFY(darkLevel < lightLevel);
}

// UIP02: dialog push buttons get uniform modern metrics — a minimum
// width so captions like "OK" don't collapse the button, and a taller
// minimum height.  Applied via size constraints (no stylesheet), it
// must never shrink an explicit larger minimum, and composite rows
// flagged "aroraNoButtonPolish" keep their compact buttons.
void tst_Dialogs::dialogButtonPolish()
{
    QDialog dialog;
    QPushButton *button = new QPushButton(QLatin1String("OK"), &dialog);
    const QFontMetrics fm = dialog.fontMetrics();
    const int expectedWidth =
        fm.horizontalAdvance(QLatin1String("MMMMMMMM"));

    BrowserTheme::polishDialogButtons(&dialog);
    QVERIFY(button->minimumWidth() >= expectedWidth);
    QVERIFY(button->minimumHeight() >= fm.height() + 14);

    // Idempotent + never shrinks a larger explicit minimum.
    button->setMinimumWidth(400);
    BrowserTheme::polishDialogButtons(&dialog);
    QCOMPARE(button->minimumWidth(), 400);

    // Exempt composite rows are left compact.
    QWidget *row = new QWidget(&dialog);
    row->setProperty("aroraNoButtonPolish", true);
    QPushButton *rowButton =
        new QPushButton(QLatin1String("Stop"), row);
    BrowserTheme::polishDialogButtons(&dialog);
    QCOMPARE(rowButton->minimumWidth(), 0);
}

QTEST_MAIN(tst_Dialogs)
#include "tst_dialogs.moc"
