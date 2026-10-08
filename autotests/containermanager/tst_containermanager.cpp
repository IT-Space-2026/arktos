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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

// CONT01: Firefox-style containers — the ContainerManager registry
// (id/name/color persisted in QSettings), lazy per-container
// QWebEngineProfiles under containers/<id>/, the shared
// prepareProfile() service attach, cross-container cookie isolation,
// profile persistence across a simulated restart, deletion hygiene
// and the tor-mode refusal.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qbuffer.h>
#include <qdatastream.h>
#include <qdir.h>
#include <qimage.h>
#include <qprocess.h>
#include <qsettings.h>
#include <qsignalspy.h>
#include <qstandardpaths.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

#include <memory>

#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "adblockrule.h"
#include "adblocksubscription.h"
#include "browserapplication.h"
#include "browserprofile.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "opensearchmanager.h"
#include "qtest_arora.h"
#include "qtry.h"
#include "tabbar.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webpage.h"
#include "webview.h"

// Minimal HTTP responder that records every request target — blocked
// requests never appear here — and answers 200.  setCookie, when set,
// rides along on every response.
class LocalHttpServer : public QObject
{
    Q_OBJECT

public:
    LocalHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                respond(socket, socket->readAll());
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url(const QString &path) const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1%2")
                    .arg(m_server.serverPort()).arg(path));
    }

    QStringList requests;
    QStringList cookieHeaders;
    QByteArray indexHtml;
    QByteArray setCookie;

private:
    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QByteArray target = request.split(' ').value(1);
        requests.append(QString::fromUtf8(target));
        // CONT02: cookie headers are recorded "<target> <cookie>" so a
        // test can attribute each one to the request that carried it —
        // an in-flight favicon fetch must not be mistaken for the
        // request under test.
        for (const QByteArray &line : request.split('\n')) {
            if (line.startsWith("Cookie:"))
                cookieHeaders.append(QString::fromUtf8(target + ' '
                                     + line.mid(7).trimmed()));
        }

        QByteArray mimeType = "text/plain";
        QByteArray body = "ok\n";
        if (target.contains(".png")) {
            mimeType = "image/png";
            body = pngBody();
        } else if (target.contains(".html") || target == "/") {
            mimeType = "text/html";
            body = indexHtml.isEmpty()
                ? QByteArray("<html><body>ok</body></html>") : indexHtml;
        }

        QByteArray response = "HTTP/1.0 200 OK\r\nContent-Type: " + mimeType
            + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (!setCookie.isEmpty())
            response += "Set-Cookie: " + setCookie + "\r\n";
        response += "Connection: close\r\n\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    static QByteArray pngBody()
    {
        static const QByteArray body = []() {
            QImage image(1, 1, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            QByteArray out;
            QBuffer buffer(&out);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return out;
        }();
        return body;
    }

    QTcpServer m_server;
};

static bool waitFor(const std::shared_ptr<bool> &flag, int timeout = 15000)
{
    for (int waited = 0; !*flag && waited < timeout; waited += 50)
        QTest::qWait(50);
    return *flag;
}

static bool loadSync(QWebEnginePage *page, const QUrl &url)
{
    std::shared_ptr<bool> done(new bool(false));
    std::shared_ptr<bool> ok(new bool(false));
    QMetaObject::Connection connection = QObject::connect(
        page, &QWebEnginePage::loadFinished, page,
        [done, ok](bool result) { *done = true; *ok = result; });
    page->load(url);
    waitFor(done);
    QObject::disconnect(connection);
    return *ok;
}

static bool hasCookie(CookieJar *jar, const QString &name)
{
    const QList<QNetworkCookie> cookies = jar->cookies();
    for (const QNetworkCookie &cookie : cookies) {
        if (cookie.name() == name.toUtf8())
            return true;
    }
    return false;
}

class tst_ContainerManager : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void registryCrud();
    void profileForBasics();
    void preparedServices();
    void cookieIsolation();
    void persistenceAcrossProfileRestart();
    void persistenceSeed();
    void persistenceVerify();
    void deleteContainerRemoves();
    void torModeRefuses();
    void reapplySettingsCoversContainers();
    void adblockAppliesOnContainerProfile();

    // CONT02: tab<->container binding and its UI surface.
    void tabContainerBinding();
    void reopenInContainer();
    void tabCookieIsolation();
    void sessionRestoresContainers();
    void containerChipIndicator();

private:
    QString create(const QString &name = QString())
    {
        const ContainerManager::Container container =
            ContainerManager::instance()->createContainer(
                name.isEmpty() ? QLatin1String("Test") : name, QColor(Qt::blue));
        m_created << container.id;
        return container.id;
    }

    LocalHttpServer *m_server;
    QStringList m_created;
};

void tst_ContainerManager::initTestCase()
{
    QCoreApplication::setApplicationName("tst_containermanager");
    QStandardPaths::setTestModeEnabled(true);

    // The persistenceVerify child must not wipe the registry it exists
    // to read back; the seed child still wants a clean slate.
    QSettings settings;
    if (qEnvironmentVariable("CONT01_STAGE") != QLatin1String("verify"))
        settings.clear();
    // A dead local list keeps AdBlockManager::load() off the network.
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-cont01.txt"
            "&title=DeadList"));
    AdBlockManager::instance()->setEnabled(true);
    ToolbarSearch::openSearchManager()->restoreDefaults();

    m_server = new LocalHttpServer(this);
    QVERIFY(m_server->start());
}

void tst_ContainerManager::init()
{
    if (qEnvironmentVariable("CONT01_STAGE") != QLatin1String("verify")) {
        QSettings settings;
        settings.clear();
    }
    m_server->requests.clear();
    m_server->cookieHeaders.clear();
    m_server->indexHtml.clear();
    m_server->setCookie.clear();
}

void tst_ContainerManager::cleanup()
{
    BrowserApplication::setTorMode(false);
    // Stage children leave what they seeded/verified alone — the seed
    // child's whole point is handing its container to the next process.
    if (qEnvironmentVariableIsSet("CONT01_STAGE"))
        return;
    ContainerManager *manager = ContainerManager::instance();
    for (const QString &id : m_created)
        manager->deleteContainer(id);
    m_created.clear();
}

// The QSettings registry round-trips id/name/color, tracks creation
// order, edits persist, and a freshly constructed manager reads back
// the same list — the "persist across restart" property.
void tst_ContainerManager::registryCrud()
{
    ContainerManager *manager = ContainerManager::instance();
    QSignalSpy spy(manager, &ContainerManager::containersChanged);

    const QString idA = create(QLatin1String("Work"));
    const QString idB = create(QLatin1String("Shopping"));
    QCOMPARE(spy.count(), 2);

    QCOMPARE(manager->containers().count(), 2);
    QVERIFY(manager->isContainerId(idA));
    QVERIFY(manager->isContainerId(idB));
    QVERIFY(!manager->isContainerId(QString()));
    QCOMPARE(manager->containers().at(0).id, idA);
    QCOMPARE(manager->containers().at(1).id, idB);

    QVERIFY(manager->renameContainer(idA, QLatin1String("Work Renamed")));
    QVERIFY(manager->setContainerColor(idB, QColor(Qt::red)));
    QCOMPARE(manager->containerForId(idA).name, QLatin1String("Work Renamed"));
    QCOMPARE(manager->containerForId(idB).color, QColor(Qt::red));
    QVERIFY(!manager->renameContainer(QLatin1String("bogus"), QString()));
    QVERIFY(!manager->setContainerColor(QLatin1String("bogus"), QColor()));

    // A fresh manager instance (the restart proxy) reads the same
    // registry from QSettings.
    ContainerManager fresh;
    QCOMPARE(fresh.containers().count(), 2);
    QCOMPARE(fresh.containerForId(idA).name, QLatin1String("Work Renamed"));
    QCOMPARE(fresh.containerForId(idB).color, QColor(Qt::red));
}

// profileFor resolves the default container to the normal profile,
// lazy-creates a persistent per-container profile under
// containers/<id>/, and refuses unknown ids.
void tst_ContainerManager::profileForBasics()
{
    ContainerManager *manager = ContainerManager::instance();

    QCOMPARE(manager->profileFor(QString()), BrowserProfile::normalProfile());
    QVERIFY(manager->profileFor(QLatin1String("nonexistent")) == nullptr);
    QVERIFY(manager->profileIfCreated(QLatin1String("nonexistent")) == nullptr);

    const QString id = create();
    QVERIFY(manager->profileIfCreated(id) == nullptr);
    QWebEngineProfile *profile = manager->profileFor(id);
    QVERIFY(profile);
    QCOMPARE(manager->profileFor(id), profile);
    QCOMPARE(manager->profileIfCreated(id), profile);

    QVERIFY(!profile->isOffTheRecord());
    QVERIFY(profile->storageName().startsWith(QLatin1String("arora-container-")));
    QVERIFY(profile->persistentStoragePath().contains(
        QLatin1String("/containers/") + id));
    QVERIFY(QDir(profile->persistentStoragePath()).exists());
    QVERIFY(manager->createdProfiles().contains(profile));
    QCOMPARE(manager->containerIdForProfile(profile), id);
    QCOMPARE(manager->containerIdForProfile(BrowserProfile::normalProfile()),
             QString());
    QCOMPARE(manager->containerIdForProfile(BrowserProfile::privateProfile()),
             QString());

    const QString id2 = create();
    QWebEngineProfile *profile2 = manager->profileFor(id2);
    QVERIFY(profile2 != profile);
    QVERIFY(profile2->persistentStoragePath()
            != profile->persistentStoragePath());
}

// profileFor routes through BrowserApplication::prepareProfile — the
// container profile carries the same services the normal profile
// does: custom scheme handlers, the SEC05 permissions pin and a
// profile-bound cookie jar.
void tst_ContainerManager::preparedServices()
{
    const QString id = create();
    QWebEngineProfile *profile = ContainerManager::instance()->profileFor(id);
    QVERIFY(profile);

    // installAll ran — the arora-file directory-listing scheme has a
    // handler on this profile.
    QVERIFY(profile->urlSchemeHandler("arora-file") != nullptr);
    // applySettings pinned the permissions policy to the broker.
    QCOMPARE(profile->persistentPermissionsPolicy(),
             QWebEngineProfile::PersistentPermissionsPolicy::AskEveryTime);
    // The per-profile cookie jar exists and is bound to this profile.
    QCOMPARE(CookieJar::instance(profile)->profile(), profile);
}

// Cookies a page on container A receives stay inside A's store — the
// other container's jar and the default jar never see them.
void tst_ContainerManager::cookieIsolation()
{
    const QString idA = create();
    const QString idB = create();
    QWebEngineProfile *profileA = ContainerManager::instance()->profileFor(idA);
    QWebEngineProfile *profileB = ContainerManager::instance()->profileFor(idB);
    QVERIFY(profileA && profileB && profileA != profileB);

    const QString cookieName = QLatin1String("cont01-a-marker");
    m_server->setCookie = cookieName.toUtf8() + "=1; Path=/";

    WebPage pageA(profileA);
    QVERIFY(loadSync(&pageA, m_server->url(QLatin1String("/cont01-a.html"))));

    CookieJar *jarA = CookieJar::instance(profileA);
    QTRY_VERIFY(hasCookie(jarA, cookieName));
    // Let the store settle, then prove the cookie never leaked.
    QTest::qWait(400);
    QVERIFY(!hasCookie(CookieJar::instance(profileB), cookieName));
    QVERIFY(!hasCookie(CookieJar::instance(), cookieName));
}

// The container's profile data survives a restart.  A faithful restart
// needs the seeding process to have fully exited: a browser context's
// storage partition releases its on-disk handles asynchronously and a
// live sibling process holding them makes a cold load unreliable.
// This test therefore orchestrates two real process lifecycles —
// `persistenceSeed` (CONT01_STAGE=seed) writes a cookie and exits,
// `persistenceVerify` (CONT01_STAGE=verify) cold-loads it.
void tst_ContainerManager::persistenceAcrossProfileRestart()
{
    const QString binary = QCoreApplication::applicationFilePath();
    auto runStage = [binary](const QString &stage, const QString &function) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QLatin1String("CONT01_STAGE"), stage);
        env.insert(QLatin1String("QT_QPA_PLATFORM"), QLatin1String("offscreen"));
        QProcess child;
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        child.start(binary, QStringList() << function);
        QTRY_VERIFY_WITH_TIMEOUT(child.state() == QProcess::NotRunning, 120000);
        QCOMPARE(child.exitCode(), 0);
    };
    runStage(QLatin1String("seed"), QLatin1String("persistenceSeed"));
    runStage(QLatin1String("verify"), QLatin1String("persistenceVerify"));
}

// Standalone child process (CONT01_STAGE=seed): create a container,
// land a persistent cookie through a real page load, hand the id to
// the verify stage via QSettings, then make sure the cookie row is
// committed before exiting — the exit is what fully releases the
// profile's on-disk handles.
void tst_ContainerManager::persistenceSeed()
{
    if (qEnvironmentVariable("CONT01_STAGE") != QLatin1String("seed"))
        QSKIP("seed stage runs as a child of persistenceAcrossProfileRestart");

    const QString id = create();
    ContainerManager *manager = ContainerManager::instance();
    QWebEngineProfile *profile = manager->profileFor(id);
    QVERIFY(profile);
    const QString storagePath = profile->persistentStoragePath();
    QVERIFY(storagePath.contains(QLatin1String("/containers/") + id));

    const QString cookieName = QLatin1String("cont01-persist");
    // Max-Age matters: session cookies are never flushed to disk, so
    // only a persistent cookie can prove on-disk persistence.
    m_server->setCookie = cookieName.toUtf8() + "=1; Path=/; Max-Age=3600";
    {
        WebPage page(profile);
        QVERIFY(loadSync(&page, m_server->url(QLatin1String("/cont01-p.html"))));
    }
    QTRY_VERIFY(hasCookie(CookieJar::instance(profile), cookieName));

    QSettings().setValue(QLatin1String("cont01-seed/id"), id);
    QSettings().setValue(QLatin1String("cont01-seed/cookie"), cookieName);

    // Chromium's cookie commit runs on a background sequence, so wait
    // for the row to land in the sqlite db rather than assuming
    // ~QWebEngineProfile flushed synchronously.
    manager->releaseProfileFor(id);
    const QString cookieDb = storagePath + QLatin1String("/Cookies");
    auto dbContains = [&cookieDb](const QByteArray &needle) {
        QFile file(cookieDb);
        return file.open(QIODevice::ReadOnly) && file.readAll().contains(needle);
    };
    QTRY_VERIFY_WITH_TIMEOUT(dbContains(cookieName.toUtf8()), 15000);
}

// Standalone child process (CONT01_STAGE=verify): everything it checks
// comes from a cold ContainerManager + QWebEngineProfile pair — the
// registry read back from QSettings and the cookie store from disk.
void tst_ContainerManager::persistenceVerify()
{
    if (qEnvironmentVariable("CONT01_STAGE") != QLatin1String("verify"))
        QSKIP("verify stage runs as a child of persistenceAcrossProfileRestart");

    QSettings settings;
    const QString id = settings.value(QLatin1String("cont01-seed/id")).toString();
    const QString cookieName =
        settings.value(QLatin1String("cont01-seed/cookie")).toString();
    QVERIFY(!id.isEmpty() && !cookieName.isEmpty());

    ContainerManager *manager = ContainerManager::instance();
    QVERIFY(manager->isContainerId(id));
    QWebEngineProfile *profile = manager->profileFor(id);
    QVERIFY(profile);
    QVERIFY(profile->persistentStoragePath().contains(
        QLatin1String("/containers/") + id));
    // The strongest proof of persistence: a fresh request on the
    // cold-loaded profile carries the cookie the seeding process wrote.
    {
        WebPage page(profile);
        QVERIFY(loadSync(&page, m_server->url(QLatin1String("/cont01-v.html"))));
    }
    QVERIFY(m_server->cookieHeaders.join(QLatin1Char(';'))
            .contains(cookieName.toUtf8() + "=1"));

    // The seeded state is consumed — remove it so no orphan storage
    // lingers for later runs.
    QVERIFY(manager->deleteContainer(id));
    settings.remove(QLatin1String("cont01-seed"));
}

// Deleting a container drops its registry entry, destroys the
// materialized profile and removes the on-disk storage + cache trees.
void tst_ContainerManager::deleteContainerRemoves()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString id = create();
    QWebEngineProfile *profile = manager->profileFor(id);
    QVERIFY(profile);
    const QString storagePath = profile->persistentStoragePath();
    const QString cachePath = profile->cachePath();
    QVERIFY(QDir(storagePath).exists());
    const bool cacheExisted = QDir(cachePath).exists();

    // The default container and unknown ids can never be deleted.
    QVERIFY(!manager->deleteContainer(QString()));
    QVERIFY(!manager->deleteContainer(QLatin1String("nonexistent")));

    QVERIFY(manager->deleteContainer(id));
    m_created.removeAll(id);
    QVERIFY(!manager->isContainerId(id));
    QVERIFY(manager->containers().isEmpty());
    QVERIFY(manager->profileIfCreated(id) == nullptr);
    QVERIFY(manager->profileFor(id) == nullptr);
    QVERIFY(!QDir(storagePath).exists());
    if (cacheExisted)
        QVERIFY(!QDir(cachePath).exists());

    // The registry is gone for a fresh manager too.
    ContainerManager fresh;
    QVERIFY(fresh.containers().isEmpty());
}

// Tor mode fails closed: no container — not even the default — hands
// out a persistent profile on the tor profile's process.
void tst_ContainerManager::torModeRefuses()
{
    const QString id = create();
    BrowserApplication::setTorMode(true);
    QVERIFY(ContainerManager::instance()->profileFor(id) == nullptr);
    QVERIFY(ContainerManager::instance()->profileFor(QString()) == nullptr);
    BrowserApplication::setTorMode(false);
    QVERIFY(ContainerManager::instance()->profileFor(id) != nullptr);
}

// reapplySettings reaches materialized container profiles — the same
// path the settings dialog and loadSettings use.
void tst_ContainerManager::reapplySettingsCoversContainers()
{
    const QString id = create();
    QWebEngineProfile *profile = ContainerManager::instance()->profileFor(id);
    QVERIFY(profile);
    QVERIFY(profile->settings()->testAttribute(QWebEngineSettings::AutoLoadImages));

    QSettings().setValue(QLatin1String("websettings/enableImages"), false);
    ContainerManager::instance()->reapplySettings();
    QVERIFY(!profile->settings()->testAttribute(QWebEngineSettings::AutoLoadImages));
}

// The adblock + privacy composite interceptor that prepareProfile
// installs blocks subresources on a container profile exactly like on
// the normal one.
void tst_ContainerManager::adblockAppliesOnContainerProfile()
{
    AdBlockManager *manager = AdBlockManager::instance();
    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), manager);
    subscription->setEnabled(true);
    manager->addSubscription(subscription);
    AdBlockRule rule(QLatin1String("/cont01-blocked.png"));
    rule.setEnabled(true);
    subscription->addRule(rule);
    manager->network()->rebuildRules();

    const QString id = create();
    QWebEngineProfile *profile = ContainerManager::instance()->profileFor(id);
    QVERIFY(profile);

    m_server->indexHtml =
        "<html><body><img src=\"/cont01-blocked.png\"></body></html>";
    WebPage page(profile);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/cont01-index.html"))));

    QVERIFY(m_server->requests.contains(QLatin1String("/cont01-index.html")));
    QVERIFY(!m_server->requests.contains(QLatin1String("/cont01-blocked.png")));

    manager->removeSubscription(subscription);
}

// CONT02: a container tab binds its page to the container's profile,
// a plain new tab inherits the CURRENT tab's container, and unknown
// ids degrade to the default container.
void tst_ContainerManager::tabContainerBinding()
{
    const QString id = create();
    ContainerManager *manager = ContainerManager::instance();
    QWebEngineProfile *containerProfile = manager->profileFor(id);
    QVERIFY(containerProfile);

    TabWidget widget;
    widget.newTab();
    WebView *defaultTab = widget.currentWebView();
    QVERIFY(defaultTab);
    QCOMPARE(defaultTab->page()->profile(),
             BrowserApplication::webEngineProfile());
    QCOMPARE(widget.containerIdForTab(0), QString());
    QCOMPARE(defaultTab->containerId(), QString());

    WebView *containerTab = widget.makeNewTabInContainer(id, true);
    QVERIFY(containerTab);
    QCOMPARE(containerTab->page()->profile(), containerProfile);
    QCOMPARE(containerTab->containerId(), id);
    QCOMPARE(widget.containerIdForTab(1), id);

    // Child-tab inheritance — a plain new tab from inside the
    // container stays inside it.
    WebView *child = widget.makeNewTab(true);
    QVERIFY(child);
    QCOMPARE(child->page()->profile(), containerProfile);
    QCOMPARE(child->containerId(), id);

    // The explicit default id escapes the current tab's container.
    WebView *escape = widget.makeNewTabInContainer(
        ContainerManager::defaultContainerId(), true);
    QVERIFY(escape);
    QCOMPARE(escape->page()->profile(), BrowserApplication::webEngineProfile());
    QCOMPARE(escape->containerId(), QString());

    // Unknown/deleted ids degrade to the default container.
    WebView *bogus = widget.makeNewTabInContainer(QLatin1String("bogus"), false);
    QVERIFY(bogus);
    QCOMPARE(bogus->page()->profile(), BrowserApplication::webEngineProfile());
}

// CONT02: reopen-in-container swaps the tab's profile in place —
// same strip index, same count, url carried over — and no-op or
// invalid targets leave the tab alone.
void tst_ContainerManager::reopenInContainer()
{
    const QString id = create();
    ContainerManager *manager = ContainerManager::instance();

    TabWidget widget;
    widget.newTab();
    WebView *tab = widget.currentWebView();
    QVERIFY(loadSync(tab->page(), m_server->url(QLatin1String("/cont02.html"))));
    const QUrl url = tab->url();
    QCOMPARE(widget.count(), 1);

    // Into the container: new page, new profile, same slot.
    widget.reopenTabInContainer(0, id);
    QCOMPARE(widget.count(), 1);
    WebView *moved = widget.webView(0);
    QVERIFY(moved);
    QVERIFY(moved != tab);
    QCOMPARE(moved->page()->profile(), manager->profileFor(id));
    QCOMPARE(widget.containerIdForTab(0), id);
    QTRY_VERIFY_WITH_TIMEOUT(
        moved->url() == url || moved->page()->requestedUrl() == url, 15000);

    // And back out to the default container.
    widget.reopenTabInContainer(0, QString());
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.webView(0)->page()->profile(),
             BrowserApplication::webEngineProfile());
    QCOMPARE(widget.containerIdForTab(0), QString());

    // Same-container and bogus targets are no-ops.
    WebView *stable = widget.webView(0);
    widget.reopenTabInContainer(0, QString());
    widget.reopenTabInContainer(0, QLatin1String("bogus"));
    widget.reopenTabInContainer(-1, id);
    widget.reopenTabInContainer(7, id);
    QCOMPARE(widget.webView(0), stable);
    QCOMPARE(widget.count(), 1);
}

// CONT02: two live tabs in different containers keep cookies apart —
// the container tab's request never carries the other container's
// cookie over the wire.
void tst_ContainerManager::tabCookieIsolation()
{
    const QString idA = create();
    const QString idB = create();
    ContainerManager *manager = ContainerManager::instance();

    TabWidget widget;
    WebView *tabA = widget.makeNewTabInContainer(idA, true);
    WebView *tabB = widget.makeNewTabInContainer(idB, false);
    QVERIFY(tabA && tabB);
    QVERIFY(tabA->page()->profile() != tabB->page()->profile());

    // The jars must exist before the loads — a jar created afterwards
    // races the cookie store's commit (its loadAllCookies snapshot can
    // run before the network-set cookie lands and the cookieAdded
    // signal it missed is never replayed).
    CookieJar *jarA = CookieJar::instance(manager->profileFor(idA));
    CookieJar *jarB = CookieJar::instance(manager->profileFor(idB));

    const QString cookieName = QLatin1String("cont02-marker");
    m_server->setCookie = cookieName.toUtf8() + "=1; Path=/";
    QVERIFY(loadSync(tabA->page(),
                     m_server->url(QLatin1String("/cont02-a.html"))));
    QTRY_VERIFY(hasCookie(jarA, cookieName));

    // The same url loaded from the other container's tab must not see
    // the cookie — neither on the wire nor in its jar.  setCookie is
    // cleared first so B's own response can't legitimately seed jar B,
    // and the wire check looks only at B's request line — an in-flight
    // favicon fetch on A's profile may still carry the marker.
    m_server->setCookie.clear();
    m_server->cookieHeaders.clear();
    QVERIFY(loadSync(tabB->page(),
                     m_server->url(QLatin1String("/cont02-b.html"))));
    QTest::qWait(300);
    const QString marker = QLatin1String("/cont02-b.html ")
        + cookieName;
    QVERIFY(!m_server->cookieHeaders.join(QLatin1Char(';'))
             .contains(marker));
    QVERIFY(!hasCookie(jarB, cookieName));
}

// CONT02: the session blob records each tab's container id (format
// v2) and restoreState binds restored tabs to the same containers; a
// v1 blob still restores into the default container.
void tst_ContainerManager::sessionRestoresContainers()
{
    const QString id = create();
    ContainerManager *manager = ContainerManager::instance();

    QByteArray state;
    {
        TabWidget widget;
        widget.newTab();
        QVERIFY(loadSync(widget.webView(0)->page(),
                         m_server->url(QLatin1String("/cont02-s0.html"))));
        WebView *containerTab = widget.makeNewTabInContainer(id, true);
        QVERIFY(loadSync(containerTab->page(),
                         m_server->url(QLatin1String("/cont02-s1.html"))));
        // A plain new tab inherits the container — the saved blob must
        // carry that binding too.
        widget.newTab();
        QVERIFY(loadSync(widget.webView(2)->page(),
                         m_server->url(QLatin1String("/cont02-s2.html"))));
        QCOMPARE(widget.count(), 3);
        state = widget.saveState();
    }

    TabWidget restored;
    QVERIFY(restored.restoreState(state));
    QCOMPARE(restored.count(), 3);
    QCOMPARE(restored.containerIdForTab(0), QString());
    QCOMPARE(restored.containerIdForTab(1), id);
    QCOMPARE(restored.containerIdForTab(2), id);
    QCOMPARE(restored.webView(1)->page()->profile(),
             manager->profileFor(id));
    QCOMPARE(restored.webView(2)->page()->profile(),
             manager->profileFor(id));
    QCOMPARE(restored.webView(0)->page()->profile(),
             BrowserApplication::webEngineProfile());

    // Backward compatibility — a hand-serialized v1 blob (no container
    // tail) restores into the default container.
    QByteArray v1;
    {
        QDataStream out(&v1, QIODevice::WriteOnly);
        out << qint32(0xaa) << qint32(1);
        out << (QStringList() << QLatin1String("data:text/plain,v1"));
        out << qint32(0);
        QList<QByteArray> noHistory;
        noHistory << QByteArray();
        out << noHistory;
    }
    TabWidget legacy;
    QVERIFY(legacy.restoreState(v1));
    QCOMPARE(legacy.count(), 1);
    QCOMPARE(legacy.containerIdForTab(0), QString());
}

// CONT02: the tab-strip indicator — a container tab reserves chip
// height in its size hint, paints the container's accent color, and
// names the container in the tab tooltip.  The widget is deliberately
// never shown: an offscreen show() pulls a container-profile WebView
// into the compositor's GL path, which traps headless — tabRect and
// grab() lay out and render the bar without it.
void tst_ContainerManager::containerChipIndicator()
{
    const QString id = create(QLatin1String("ChipTest"));
    ContainerManager::instance()->setContainerColor(
        id, QColor(0x12, 0x34, 0x56));

    TabWidget widget;
    widget.newTab();
    TabBar *bar = widget.tabBar();
    widget.resize(400, 60);
    const int plainHeight = bar->tabRect(0).height();
    QVERIFY(plainHeight > 0);

    WebView *containerTab = widget.makeNewTabInContainer(id, false);
    QVERIFY(containerTab);

    // QTabBar lays out every tab at the strip's max height, so the
    // chip reservation shows as the whole strip growing taller once
    // the container tab lands.
    QVERIFY(bar->tabRect(1).isValid());
    QVERIFY(bar->tabRect(1).height() > plainHeight);
    QCOMPARE(bar->sizeHint().height(), bar->tabRect(1).height());

    // The accent strip lands in the painted output.
    const QImage image = bar->grab().toImage();
    bool accentFound = false;
    for (int y = 0; y < image.height() && !accentFound; ++y) {
        for (int x = 0; x < image.width() && !accentFound; ++x) {
            if (image.pixelColor(x, y) == QColor(0x12, 0x34, 0x56))
                accentFound = true;
        }
    }
    QVERIFY(accentFound);

    // The container name rides the tab tooltip.
    QVERIFY(loadSync(containerTab->page(),
                     m_server->url(QLatin1String("/cont02-c.html"))));
    QTRY_VERIFY_WITH_TIMEOUT(
        bar->tabToolTip(1).contains(QLatin1String("ChipTest")), 5000);
}

QTEST_MAIN(tst_ContainerManager)
#include "tst_containermanager.moc"
