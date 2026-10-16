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

#include <QtTest/QtTest>
#include "qtest_arora.h"
#include "qtry.h"

#include <browserapplication.h>
#include <browsermainwindow.h>
#include <browserpaths.h>
#include <browserprofile.h>
#include <historymanager.h>
#include <profileregistry.h>
#include <profilesettings.h>
#include <tabwidget.h>
#include <webpage.h>
#include <webview.h>

#include <qdatetime.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qrandom.h>
#include <qsignalspy.h>
#include <qtemporaryfile.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

// MPROF03: the window<->profile binding.  A window bound to a registry
// persona must put every WebView it creates on that persona's named
// QWebEngineProfile, keep its app-side stores (history, settings,
// containers) inside the persona, and record the binding in the saved
// session so a restore recreates the window on the same profile.
// Private windows share one unnamed OTR overlay across personas
// (memory-only — documented choice); tor keeps its dedicated profile.
class tst_ProfileWindow : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void personaProfileFactory();
    void windowBindsNamedProfile();
    void unregisteredIdFallsBack();
    void ambientFollowsWindow();
    void childTabUsesWindowPersonaNotAmbient();
    void twoWindowsCoexist();
    void historyWrittenUnderBoundProfile();
    void privateWindowSharesOtrAndWritesNothing();
    void sessionRoundTripKeepsBindings();
    void deletedProfileFallsBackOnRestore();

private:
    void writeRegistry(const QStringList &ids);
    void closeAllWindows();
    BrowserMainWindow *windowForProfile(const QString &id) const;
};

static const QString kAlpha = QStringLiteral("alpha");
static const QString kBeta = QStringLiteral("beta");
static const QString kDefault = QStringLiteral("default");

void tst_ProfileWindow::initTestCase()
{
    QVERIFY(BrowserPaths::ensureDataLayout());
    writeRegistry(QStringList() << kDefault << kAlpha << kBeta);
    // A window-close with several tabs would prompt — every window
    // this test opens stays single-tab by construction, but arm the
    // honest "don't ask" for all personas anyway.
    for (const QString &id : {kDefault, kAlpha, kBeta}) {
        ProfileSettings(id).setValue(
            QLatin1String("tabs/confirmClosingMultipleTabs"), false);
    }
}

void tst_ProfileWindow::cleanup()
{
    // Legs must not leak ambient persona or the private flag — the
    // process-global knobs every unscoped store resolves through.
    BrowserApplication::setPrivate(false);
    BrowserPaths::setActiveProfileId(kDefault);
    // The restore crash-guard is a global key — a leg that left it
    // armed would make the next restore pop a modal question box.
    ProfileSettings().setValue(QLatin1String("MainWindow/restoring"),
                               false);
    closeAllWindows();
}

void tst_ProfileWindow::writeRegistry(const QStringList &ids)
{
    QJsonArray profiles;
    for (const QString &id : ids) {
        QJsonObject entry;
        entry.insert(QLatin1String("id"), id);
        entry.insert(QLatin1String("name"), id);
        entry.insert(QLatin1String("avatar"), QString());
        entry.insert(QLatin1String("createdAt"),
                     QDateTime::currentDateTime().toString(Qt::ISODate));
        profiles.append(entry);
    }
    QJsonObject root;
    root.insert(QLatin1String("version"), 1);
    root.insert(QLatin1String("profiles"), profiles);
    const QString path = BrowserPaths::globalDataFilePath(
        QLatin1String("profiles.json"));
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    ProfileRegistry::instance()->reload();
}

void tst_ProfileWindow::closeAllWindows()
{
    BrowserApplication *app = BrowserApplication::instance();
    if (!app)
        return;
    for (BrowserMainWindow *window : app->mainWindows()) {
        if (window)
            window->close();
    }
    QCoreApplication::processEvents();
}

BrowserMainWindow *tst_ProfileWindow::windowForProfile(const QString &id) const
{
    BrowserApplication *app = BrowserApplication::instance();
    if (!app)
        return nullptr;
    for (BrowserMainWindow *window : app->mainWindows()) {
        if (window && window->profileId() == id)
            return window;
    }
    return nullptr;
}

void tst_ProfileWindow::personaProfileFactory()
{
    // The canonical persona factory: lazily created, tagged, named
    // "arora-<id>" — "default" aliases the existing "arora" profile
    // so pre-MPROF01 disk data keeps resolving.
    QCOMPARE(BrowserProfile::profileFor(kDefault),
             BrowserProfile::normalProfile());
    QWebEngineProfile *alpha = BrowserProfile::profileFor(kAlpha);
    QCOMPARE(alpha->storageName(), QStringLiteral("arora-alpha"));
    QCOMPARE(BrowserProfile::profileIdFor(alpha), kAlpha);
    QCOMPARE(BrowserProfile::profileFor(kAlpha), alpha);
    // A non-slug id can never name a profile — default, honestly.
    QCOMPARE(BrowserProfile::profileFor(QStringLiteral("../x")),
             BrowserProfile::normalProfile());
}

void tst_ProfileWindow::windowBindsNamedProfile()
{
    BrowserApplication *app = BrowserApplication::instance();
    BrowserMainWindow *window = app->newMainWindow(kAlpha);
    QCOMPARE(window->profileId(), kAlpha);

    // The tab the ctor made is already on the persona's named profile
    // — not the shared default's.
    WebView *tab = window->currentTab();
    QVERIFY(tab && tab->webPage());
    QWebEngineProfile *pageProfile = tab->webPage()->profile();
    QCOMPARE(pageProfile, BrowserProfile::profileFor(kAlpha));
    QCOMPARE(BrowserProfile::profileIdFor(pageProfile), kAlpha);
    QCOMPARE(pageProfile->storageName(), QStringLiteral("arora-alpha"));

    // The window's tab strip reports the same binding.
    QCOMPARE(window->tabWidget()->profileId(), kAlpha);
}

void tst_ProfileWindow::unregisteredIdFallsBack()
{
    BrowserApplication *app = BrowserApplication::instance();
    // "ghost" is a valid slug but no registry persona carries it —
    // the window binds default honestly instead of reviving a stale
    // profiles/<ghost>/ tree.
    BrowserMainWindow *window = app->newMainWindow(QStringLiteral("ghost"));
    QCOMPARE(window->profileId(), kDefault);
    QCOMPARE(window->currentTab()->webPage()->profile(),
             BrowserProfile::normalProfile());

    // A non-slug id is refused the same way.
    BrowserMainWindow *bad = app->newMainWindow(QStringLiteral("../evil"));
    QCOMPARE(bad->profileId(), kDefault);
}

void tst_ProfileWindow::ambientFollowsWindow()
{
    // The ambient persona is the focused window's: construction pushes
    // it, activation re-pushes it, and the context-less resolvers
    // (ProfileSettings(), *Manager::instance()) follow.
    BrowserApplication *app = BrowserApplication::instance();
    BrowserMainWindow *alphaWindow = app->newMainWindow(kAlpha);
    QCoreApplication::processEvents();
    QCOMPARE(BrowserPaths::activeProfileId(), kAlpha);
    QCOMPARE(ProfileSettings::currentId(), kAlpha);
    QCOMPARE(ProfileRegistry::instance()->activeProfileId(), kAlpha);

    BrowserMainWindow *defaultWindow = app->newMainWindow(kDefault);
    QCoreApplication::processEvents();
    QCOMPARE(BrowserPaths::activeProfileId(), kDefault);
    QCOMPARE(ProfileSettings::currentId(), kDefault);

    // Focus switching keeps ambient honest — whatever window wins
    // activation pushes its own persona again.
    alphaWindow->activateWindow();
    alphaWindow->raise();
    QCoreApplication::processEvents();
    QTRY_COMPARE_WITH_TIMEOUT(BrowserPaths::activeProfileId(), kAlpha,
                              5000);
    defaultWindow->activateWindow();
    defaultWindow->raise();
    QCoreApplication::processEvents();
    QTRY_COMPARE_WITH_TIMEOUT(BrowserPaths::activeProfileId(), kDefault,
                              5000);
}

void tst_ProfileWindow::childTabUsesWindowPersonaNotAmbient()
{
    // The contamination guard: a tab created while the ambient persona
    // is somewhere else must still land on the WINDOW's profile —
    // the binding comes from the window, never the focused persona.
    BrowserApplication *app = BrowserApplication::instance();
    BrowserMainWindow *window = app->newMainWindow(kAlpha);
    BrowserPaths::setActiveProfileId(kDefault); // pretend focus moved

    WebView *tab = window->tabWidget()->makeNewTab(true);
    QVERIFY(tab);
    QCOMPARE(BrowserProfile::profileIdFor(tab->webPage()->profile()),
             kAlpha);
    QCOMPARE(window->tabWidget()->profileId(), kAlpha);
    closeAllWindows();
}

void tst_ProfileWindow::twoWindowsCoexist()
{
    BrowserApplication *app = BrowserApplication::instance();
    BrowserMainWindow *alphaWindow = app->newMainWindow(kAlpha);
    BrowserMainWindow *betaWindow = app->newMainWindow(kBeta);
    QVERIFY(alphaWindow != betaWindow);
    QCOMPARE(windowForProfile(kAlpha), alphaWindow);
    QCOMPARE(windowForProfile(kBeta), betaWindow);
    QVERIFY(alphaWindow->currentTab()->webPage()->profile()
            != betaWindow->currentTab()->webPage()->profile());
    QCOMPARE(alphaWindow->currentTab()->webPage()->profile(),
             BrowserProfile::profileFor(kAlpha));
    QCOMPARE(betaWindow->currentTab()->webPage()->profile(),
             BrowserProfile::profileFor(kBeta));
}

void tst_ProfileWindow::historyWrittenUnderBoundProfile()
{
    // The Verify leg: a window bound to profile B writes its visits
    // into B's store only — A's and default's stay clean.
    BrowserApplication *app = BrowserApplication::instance();
    BrowserMainWindow *window = app->newMainWindow(kAlpha);
    WebView *view = window->currentTab();
    QVERIFY(view);

    const QString url = QStringLiteral(
        "data:text/plain,mprof03-alpha-history-%1").arg(
            QRandomGenerator::global()->bounded(1000000));
    HistoryManager *alphaHistory = HistoryManager::instanceFor(kAlpha);
    HistoryManager *betaHistory = HistoryManager::instanceFor(kBeta);
    HistoryManager *defaultHistory =
        HistoryManager::instanceFor(kDefault);
    QVERIFY(!alphaHistory->historyContains(url));

    QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
    view->loadUrl(QUrl(url));
    QVERIFY(loaded.wait(15000));

    QTRY_VERIFY_WITH_TIMEOUT(alphaHistory->historyContains(url), 5000);
    QVERIFY(!betaHistory->historyContains(url));
    QVERIFY(!defaultHistory->historyContains(url));
    closeAllWindows();
}

void tst_ProfileWindow::privateWindowSharesOtrAndWritesNothing()
{
    BrowserApplication *app = BrowserApplication::instance();
    BrowserApplication::setPrivate(true);
    QVERIFY(BrowserApplication::isPrivate());

    // The documented OTR decision: one unnamed memory-only profile
    // serves every persona — private windows on alpha and beta share
    // it, and it is never the persistent named profile.
    QWebEngineProfile *otrForAlpha =
        BrowserApplication::webEngineProfileFor(kAlpha);
    QWebEngineProfile *otrForBeta =
        BrowserApplication::webEngineProfileFor(kBeta);
    QVERIFY(otrForAlpha->isOffTheRecord());
    QCOMPARE(otrForAlpha, otrForBeta);
    QCOMPARE(otrForAlpha, BrowserProfile::privateProfile());

    // The window still binds its persona id — only the browsing
    // profile is the overlay.
    BrowserMainWindow *window = app->newMainWindow(kAlpha);
    QCOMPARE(window->profileId(), kAlpha);
    WebView *view = window->currentTab();
    QVERIFY(view->webPage()->profile()->isOffTheRecord());

    const QString url = QStringLiteral(
        "data:text/plain,mprof03-otr-%1").arg(
            QRandomGenerator::global()->bounded(1000000));
    QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
    view->loadUrl(QUrl(url));
    QVERIFY(loaded.wait(15000));

    // Nothing reaches any persistent persona store.
    QTest::qWait(250); // let any stray async write land — it must not
    QVERIFY(!HistoryManager::instanceFor(kAlpha)->historyContains(url));
    QVERIFY(!HistoryManager::instanceFor(kBeta)->historyContains(url));
    QVERIFY(!HistoryManager::instanceFor(kDefault)->historyContains(url));

    closeAllWindows();
    BrowserApplication::setPrivate(false);
    QVERIFY(!BrowserApplication::isPrivate());
}

void tst_ProfileWindow::sessionRoundTripKeepsBindings()
{
    BrowserApplication *app = BrowserApplication::instance();
    ProfileSettings().setValue(QLatin1String("MainWindow/restoring"),
                             false);

    // Two persona-bound windows + one default window, each with a
    // navigated tab so the manifest has real content.
    struct { QString id; QString url; } legs[] = {
        {kAlpha, QStringLiteral("data:text/plain,mprof03-sess-a")},
        {kBeta, QStringLiteral("data:text/plain,mprof03-sess-b")},
        {kDefault, QStringLiteral("data:text/plain,mprof03-sess-d")},
    };
    for (const auto &leg : legs) {
        BrowserMainWindow *window = app->newMainWindow(leg.id);
        QCOMPARE(window->profileId(), leg.id);
        WebView *view = window->currentTab();
        QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
        view->loadUrl(QUrl(leg.url));
        QVERIFY(loaded.wait(15000));
    }

    app->saveSession();

    // ~BrowserMainWindow flushes an autosave on the way down — at a
    // real launch the manifest was already written at quit.  Holding
    // writes off while the windows die (saveSession() is inert under
    // isPrivate) keeps the snapshot the launcher would see.
    BrowserApplication::setPrivate(true);
    closeAllWindows();
    BrowserApplication::setPrivate(false);

    QVERIFY(app->restoreLastSession());
    QCoreApplication::processEvents();

    QStringList restored;
    for (BrowserMainWindow *window : app->mainWindows()) {
        if (window)
            restored.append(window->profileId());
    }
    restored.sort();
    QCOMPARE(restored,
             (QStringList() << kAlpha << kBeta << kDefault));

    // Each restored window's tab is on its own persona profile again.
    QVERIFY(windowForProfile(kAlpha));
    QCOMPARE(BrowserProfile::profileIdFor(
                 windowForProfile(kAlpha)->currentTab()
                     ->webPage()->profile()),
             kAlpha);
    QCOMPARE(BrowserProfile::profileIdFor(
                 windowForProfile(kBeta)->currentTab()
                     ->webPage()->profile()),
             kBeta);
    closeAllWindows();
}

void tst_ProfileWindow::deletedProfileFallsBackOnRestore()
{
    BrowserApplication *app = BrowserApplication::instance();
    ProfileSettings().setValue(QLatin1String("MainWindow/restoring"),
                             false);

    BrowserMainWindow *window = app->newMainWindow(kAlpha);
    QCOMPARE(window->profileId(), kAlpha);
    app->saveSession();

    // The persona vanishes between sessions — the manifest still
    // names alpha, but the honest restore is "default", not a
    // silently resurrected profiles/alpha/ tree.
    writeRegistry(QStringList() << kDefault << kBeta);
    QCOMPARE(ProfileRegistry::instance()->profile(kAlpha).isValid(),
             false);

    // Same destructor-autosave guard as the round-trip leg.
    BrowserApplication::setPrivate(true);
    closeAllWindows();
    BrowserApplication::setPrivate(false);
    QVERIFY(app->restoreLastSession());
    QCoreApplication::processEvents();

    BrowserMainWindow *restored = windowForProfile(kDefault);
    QVERIFY(restored);
    QCOMPARE(BrowserProfile::profileIdFor(
                 restored->currentTab()->webPage()->profile()),
             kDefault);

    // Leave the registry the way the other legs expect it.
    writeRegistry(QStringList() << kDefault << kAlpha << kBeta);
    closeAllWindows();
}

QTEST_MAIN(tst_ProfileWindow)
#include "tst_profilewindow.moc"
