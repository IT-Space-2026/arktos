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

// PROF03: the profile-menu hygiene backend.  Each single-purpose
// action runs exactly one confirm, executes its single-category
// clear plan and reports on the status bar; unrelated stores must
// survive.  'Delete Profile' is gated twice (itemize confirm +
// type-to-confirm), arms arora-profile-wipe.pending, and the wipe
// itself runs at the next boot — before any profile exists.  The
// cross-process contract (marker -> exit -> fresh boot with wiped
// stores) is exercised by --profile-wipe-smoke; the ARORA_WIPE_NO_QUIT
// seam lets the in-process legs stop after the marker write.

#include <QtTest/QtTest>
#include "qtest_arora.h"

#include <browserpaths.h>
#include <browserprofile.h>
#include <clearprivatedata.h>
#include <cookiejar.h>
#include <historymanager.h>
#include <profilemenuactions.h>

#include <qaction.h>
#include <qdir.h>
#include <qfile.h>
#include <qlineedit.h>
#include <qmainwindow.h>
#include <qmessagebox.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qstatusbar.h>
#include <qwidgetaction.h>

// Clicks Yes on a clear action's synchronous QMessageBox::warning —
// QDialog::accept() does not map back to a standard button, so the
// modal answer must click the button itself once the box is up.
static void approveClearConfirm(int msec = 80)
{
    QTimer::singleShot(msec, qApp, []() {
        QMessageBox *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!box)
            return;
        if (QAbstractButton *yes = box->button(QMessageBox::Yes))
            yes->click();
        else
            box->reject();
    });
}

// Clicks "Continue…" (the AcceptRole button) on the wipe's itemize
// QMessageBox once it is the active modal.
static void approveWipeGate1(int msec = 80)
{
    QTimer::singleShot(msec, qApp, []() {
        QMessageBox *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!box)
            return;
        const QList<QAbstractButton *> buttons = box->buttons();
        for (QAbstractButton *button : buttons) {
            if (box->buttonRole(button) == QMessageBox::AcceptRole) {
                button->click();
                return;
            }
        }
    });
}

// Drives both gates of requestProfileWipe() — the itemize box's
// AcceptRole button, then the confirm dialog's phrase + wipe button.
static QTimer *driveWipeConfirm()
{
    QTimer *timer = new QTimer(qApp);
    timer->setInterval(40);
    QObject::connect(timer, &QTimer::timeout, qApp, []() {
        QWidget *widget = QApplication::activeModalWidget();
        if (QMessageBox *box = qobject_cast<QMessageBox*>(widget)) {
            const QList<QAbstractButton *> buttons = box->buttons();
            for (QAbstractButton *button : buttons) {
                if (box->buttonRole(button) == QMessageBox::AcceptRole) {
                    button->click();
                    return;
                }
            }
            box->reject();
            return;
        }
        if (ProfileWipeConfirmDialog *dialog =
                qobject_cast<ProfileWipeConfirmDialog *>(widget)) {
            dialog->phraseEdit->setText(
                ProfileWipeConfirmDialog::confirmPhrase());
            if (dialog->wipeButton->isEnabled())
                dialog->wipeButton->click();
        }
    });
    timer->start();
    return timer;
}

class tst_ProfileMenu : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void hygieneActionsExist();
    void clearHistoryAction();
    void clearCookiesAction();
    void clearCacheKeepsStores();
    void cancelledConfirmClearsNothing();
    void clearPrivateDataOpensExistingDialog();
    void deleteProfileActionIsDangerStyled();
    void wipeConfirmDialogGatesOnPhrase();
    void wipeCancelAtItemizeSchedulesNothing();
    void wipeCancelAtConfirmSchedulesNothing();
    void requestWipeSchedulesMarker();
    void pendingWipeAppliesAtBoot();
};

void tst_ProfileMenu::initTestCase()
{
    // The seam: requestProfileWipe stops after the marker write — the
    // respawn + exit only run in the real binary.
    qputenv("ARORA_WIPE_NO_QUIT", QByteArray("1"));
}

void tst_ProfileMenu::cleanup()
{
    // A failed leg must not leave the marker armed for the next one.
    QFile::remove(ProfileMenuActions::wipeMarkerPath());
}

void tst_ProfileMenu::hygieneActionsExist()
{
    ProfileMenuActions actions(nullptr);
    const QList<QAction *> list = actions.hygieneActions();
    QCOMPARE(list.count(), 4);
    QVERIFY(list.at(0) == actions.clearHistoryAction());
    QVERIFY(list.at(1) == actions.clearCookiesAction());
    QVERIFY(list.at(2) == actions.clearCacheAction());
    QVERIFY(list.at(3) == actions.clearPrivateDataAction());
    QCOMPARE(actions.clearHistoryAction()->text(),
             QStringLiteral("Clear History…"));
    QCOMPARE(actions.clearCookiesAction()->text(),
             QStringLiteral("Clear Cookies…"));
    QCOMPARE(actions.clearCacheAction()->text(),
             QStringLiteral("Clear Cache…"));
    QCOMPARE(actions.clearPrivateDataAction()->text(),
             QStringLiteral("Clear Private Data…"));
}

void tst_ProfileMenu::clearHistoryAction()
{
    QMainWindow window;
    ProfileMenuActions actions(&window);

    HistoryManager *history = HistoryManager::instance();
    history->clear();
    const QString url = QLatin1String("http://prof03-hist.test/");
    history->addHistoryEntry(url);
    CookieJar *jar =
        CookieJar::instance(BrowserProfile::normalProfile());
    const QNetworkCookie seeded("prof03hist", "1");
    QList<QNetworkCookie> cookies = jar->cookies();
    cookies << seeded;
    jar->setCookies(cookies);

    QVERIFY(history->historyContains(url));
    QVERIFY(jar->cookies().contains(seeded));

    approveClearConfirm();
    actions.clearHistoryAction()->trigger();

    QVERIFY(!history->historyContains(url));
    QVERIFY(jar->cookies().contains(seeded));
    QVERIFY(window.statusBar()->currentMessage()
                .contains(QLatin1String("cleared")));
}

void tst_ProfileMenu::clearCookiesAction()
{
    QMainWindow window;
    ProfileMenuActions actions(&window);

    HistoryManager *history = HistoryManager::instance();
    const QString url = QLatin1String("http://prof03-cookies.test/");
    history->addHistoryEntry(url);
    CookieJar *jar =
        CookieJar::instance(BrowserProfile::normalProfile());
    const QNetworkCookie seeded("prof03cookies", "1");
    QList<QNetworkCookie> cookies = jar->cookies();
    cookies << seeded;
    jar->setCookies(cookies);

    QVERIFY(history->historyContains(url));
    QVERIFY(jar->cookies().contains(seeded));

    approveClearConfirm();
    actions.clearCookiesAction()->trigger();

    QVERIFY(jar->cookies().isEmpty());
    QVERIFY(history->historyContains(url));
    QVERIFY(window.statusBar()->currentMessage()
                .contains(QLatin1String("cleared")));
}

void tst_ProfileMenu::clearCacheKeepsStores()
{
    QMainWindow window;
    ProfileMenuActions actions(&window);

    HistoryManager *history = HistoryManager::instance();
    const QString url = QLatin1String("http://prof03-cache.test/");
    history->addHistoryEntry(url);
    CookieJar *jar =
        CookieJar::instance(BrowserProfile::normalProfile());
    const QNetworkCookie seeded("prof03cache", "1");
    QList<QNetworkCookie> cookies = jar->cookies();
    cookies << seeded;
    jar->setCookies(cookies);

    approveClearConfirm();
    actions.clearCacheAction()->trigger();

    // The engine's http cache flush is async and unobservable from
    // here — the contract this leg pins is that the single-category
    // plan does not bleed into the other stores.
    QVERIFY(history->historyContains(url));
    QVERIFY(jar->cookies().contains(seeded));
    QVERIFY(window.statusBar()->currentMessage()
                .contains(QLatin1String("cleared")));
}

void tst_ProfileMenu::cancelledConfirmClearsNothing()
{
    QMainWindow window;
    ProfileMenuActions actions(&window);

    HistoryManager *history = HistoryManager::instance();
    const QString url = QLatin1String("http://prof03-cancel.test/");
    history->addHistoryEntry(url);
    CookieJar *jar =
        CookieJar::instance(BrowserProfile::normalProfile());
    const QNetworkCookie seeded("prof03cancel", "1");
    QList<QNetworkCookie> cookies = jar->cookies();
    cookies << seeded;
    jar->setCookies(cookies);

    rejectModal();
    actions.clearHistoryAction()->trigger();

    QVERIFY(history->historyContains(url));
    QVERIFY(jar->cookies().contains(seeded));
}

void tst_ProfileMenu::clearPrivateDataOpensExistingDialog()
{
    ProfileMenuActions actions(nullptr);

    bool seen = false;
    QTimer::singleShot(60, qApp, [&seen]() {
        seen = qobject_cast<ClearPrivateData *>(
                   QApplication::activeModalWidget()) != nullptr;
    });
    QTimer::singleShot(140, qApp, []() {
        if (QDialog *dialog = qobject_cast<QDialog *>(
                QApplication::activeModalWidget()))
            dialog->reject();
    });
    actions.clearPrivateDataAction()->trigger();
    QVERIFY(seen);
}

void tst_ProfileMenu::deleteProfileActionIsDangerStyled()
{
    ProfileMenuActions actions(nullptr);
    QAction *action = actions.deleteProfileAction();
    QVERIFY(action);
    QWidgetAction *widgetAction = qobject_cast<QWidgetAction *>(action);
    QVERIFY(widgetAction);
    QWidget *row = widgetAction->defaultWidget();
    QVERIFY(row);
    // The danger row is rendered red — the accent must be on the
    // widget, not left to whatever the menu style resolves.
    QVERIFY(row->styleSheet().contains(QLatin1String("#c62828")));
    QCOMPARE(static_cast<QPushButton *>(row)->text(),
             QStringLiteral("Delete Profile…"));
}

void tst_ProfileMenu::wipeConfirmDialogGatesOnPhrase()
{
    ProfileWipeConfirmDialog dialog;
    QVERIFY(!dialog.wipeButton->isEnabled());

    // Partial and wrong-case phrases must never arm the wipe.
    dialog.phraseEdit->setText(QStringLiteral("DEL"));
    QVERIFY(!dialog.wipeButton->isEnabled());
    dialog.phraseEdit->setText(QStringLiteral("delete"));
    QVERIFY(!dialog.wipeButton->isEnabled());

    dialog.phraseEdit->setText(ProfileWipeConfirmDialog::confirmPhrase());
    QVERIFY(dialog.wipeButton->isEnabled());

    dialog.wipeButton->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
}

void tst_ProfileMenu::wipeCancelAtItemizeSchedulesNothing()
{
    QMainWindow window;
    rejectModal();
    QVERIFY(!ProfileMenuActions::requestProfileWipe(&window));
    QVERIFY(!ProfileMenuActions::wipePending());
}

void tst_ProfileMenu::wipeCancelAtConfirmSchedulesNothing()
{
    QMainWindow window;
    approveWipeGate1();
    rejectModal(220);   // the type-to-confirm dialog, after gate 1
    QVERIFY(!ProfileMenuActions::requestProfileWipe(&window));
    QVERIFY(!ProfileMenuActions::wipePending());
}

void tst_ProfileMenu::requestWipeSchedulesMarker()
{
    QMainWindow window;
    QTimer *driver = driveWipeConfirm();
    const bool armed = ProfileMenuActions::requestProfileWipe(&window);
    driver->stop();
    delete driver;
    QVERIFY(armed);
    QVERIFY(ProfileMenuActions::wipePending());

    // The marker names the writer — the next boot waits that pid out
    // before wiping so teardown writes cannot land in the fresh tree.
    QFile marker(ProfileMenuActions::wipeMarkerPath());
    QVERIFY(marker.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(marker.readAll()).trimmed().toLongLong(),
             QCoreApplication::applicationPid());
    marker.close();
    QFile::remove(ProfileMenuActions::wipeMarkerPath());
}

void tst_ProfileMenu::pendingWipeAppliesAtBoot()
{
    // Keep this leg LAST — it removes the test-mode data + cache
    // roots under the live test app, so nothing it could disturb may
    // run after it.
    {
        QSettings settings;
        settings.setValue(QLatin1String("profilewipe/seed"),
                          QLatin1String("present"));
        settings.sync();
    }
    const QString dataSeed = BrowserPaths::dataFilePath(
        QLatin1String("boot-wipe-seed.txt"));
    {
        QFile file(dataSeed);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("seed\n");
    }
    const QString cacheRoot = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    QDir().mkpath(cacheRoot);
    const QString cacheSeed =
        cacheRoot + QLatin1String("/boot-wipe-seed.txt");
    {
        QFile file(cacheSeed);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("seed\n");
    }
    const QString settingsFile = QSettings().fileName();
    QVERIFY(QFile::exists(settingsFile));

    // Own pid -> the wait-for-exit skips, the wipe runs now.
    QVERIFY(ProfileMenuActions::scheduleProfileWipe());
    QVERIFY(ProfileMenuActions::applyPendingProfileWipe());

    QVERIFY(!ProfileMenuActions::wipePending());
    QVERIFY(!QFile::exists(dataSeed));
    QVERIFY(!QFile::exists(cacheSeed));
    QVERIFY(!QFile::exists(settingsFile));
    QVERIFY(QSettings()
                .value(QLatin1String("profilewipe/seed")).isNull());
}

QTEST_MAIN(tst_ProfileMenu)
#include "tst_profilemenu.moc"
