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

// CONT01-05: Firefox-style containers — the ContainerManager registry
// (id/name/color persisted in QSettings), lazy per-container
// QWebEngineProfiles under containers/<id>/, the shared
// prepareProfile() service attach, cross-container cookie/DOM-storage/
// auth/cache isolation, profile persistence across a simulated
// restart, deletion and clear-data hygiene for containers never
// materialized this session, and the off-the-record refusals.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qbuffer.h>
#include <qcheckbox.h>
#include <qdatastream.h>
#include <qdir.h>
#include <qimage.h>
#include <qprocess.h>
#include <qsettings.h>
#include <qpointer.h>
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
#include "clearprivatedata.h"
#include "containermanager.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif
#include "cookiejar.h"
#include "historymanager.h"
#include "opensearchmanager.h"
#include "privacyrequestinterceptor.h"
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
    // CONT05: "<target> <authorization>" for every request carrying
    // credentials — http-auth isolation is checked on the wire.
    QStringList authHeaders;
    QByteArray indexHtml;
    QByteArray setCookie;
    // CONT05: Cache-Control response header, e.g. "public, max-age=3600".
    QByteArray cacheControl;
    // CONT05: a path that answers 401 Basic until it sees credentials.
    QString protectedPath;
    // CONT04: target path -> absolute Location for 302 answers, so a
    // test can drive a mid-chain redirect across hosts.
    QHash<QString, QString> redirects;

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
            if (line.startsWith("Authorization:"))
                authHeaders.append(QString::fromUtf8(target + ' '
                                     + line.mid(14).trimmed()));
        }

        if (!protectedPath.isEmpty()
            && QString::fromUtf8(target).startsWith(protectedPath)
            && !request.contains("\nAuthorization:")) {
            const QByteArray denied = "HTTP/1.0 401 Unauthorized\r\n"
                "WWW-Authenticate: Basic realm=\"cont05\"\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            socket->write(denied);
            socket->disconnectFromHost();
            return;
        }

        const QByteArray location =
            redirects.value(QString::fromUtf8(target)).toUtf8();
        if (!location.isEmpty()) {
            const QByteArray response = "HTTP/1.0 302 Found\r\nLocation: "
                + location + "\r\nContent-Length: 0\r\n"
                + "Connection: close\r\n\r\n";
            socket->write(response);
            socket->disconnectFromHost();
            return;
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
        } else if (target.endsWith(".js")) {
            // CONT05: a service-worker script — any valid JS registers.
            mimeType = "text/javascript";
            body = "self.addEventListener('fetch',function(){});\n";
        }

        QByteArray response = "HTTP/1.0 200 OK\r\nContent-Type: " + mimeType
            + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (!setCookie.isEmpty())
            response += "Set-Cookie: " + setCookie + "\r\n";
        if (!cacheControl.isEmpty())
            response += "Cache-Control: " + cacheControl + "\r\n";
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

// CONT05: runJavaScript, synchronous — invalid QVariant on timeout.
static QVariant evalJs(QWebEnginePage *page, const QString &script)
{
    QVariant result;
    bool done = false;
    page->runJavaScript(script,
        [&result, &done](const QVariant &v) { result = v; done = true; });
    for (int waited = 0; !done && waited < 15000; waited += 50)
        QTest::qWait(50);
    return result;
}

// CONT05: poll a page-side expression (e.g. "String(window.__flag||'')")
// until it reports a non-empty string — the async IndexedDB/service-
// worker probes resolve after loadFinished.
static QString evalJsUntil(QWebEnginePage *page, const QString &script,
                           int timeout = 15000)
{
    for (int waited = 0; waited < timeout; waited += 100) {
        const QString value = evalJs(page, script).toString();
        if (!value.isEmpty())
            return value;
        QTest::qWait(100);
    }
    return QString();
}

static bool writeMarker(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
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

    // CONT06: the two-level container strip — per-container level-1
    // headers, filtered level-2 rows, live counts, cross-level rebind,
    // empty-level removal, session round-trip and group nesting.
    void twoLevelStripBasics();
    void twoLevelStripEmptyLevelFallsBack();
    void twoLevelStripReopenAcrossLevels();
    void twoLevelStripSessionRoundTrip();
    void twoLevelStripGroupStaysNested();
    void twoLevelStripHeaderReorder();

    // CONT04: site->container "always open" rules — persistence,
    // matching, and the navigation-time diversion.
    void siteRuleCrud();
    void siteRuleDiversion();
    void siteRuleRedirectChain();
    void siteRuleGates();

    // CONT05: the isolation audit — an off-the-record session can
    // never hold a container profile; every storage class stays
    // partitioned per profile; delete/clear reach containers never
    // materialized this session; and the deliberately app-global
    // surfaces (history) are pinned so a regression makes noise.
    void privateModeRefuses();
    void domStorageIsolation();
    void httpAuthIsolation();
    void httpCacheIsolation();
    void derivedPathCleanup();
    void unmaterializedClear();
    void clearDialogCoversUnmaterialized();
    void historyIsGlobalByDesign();

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
        // CONT06: the pre-CONT06 tests below were written against the
        // single-row (inline chips) strip — pin that mode; the
        // two-level tests opt back in explicitly.
        settings.setValue(QLatin1String("tabs/containerDisplay"), 0);
#if defined(ARORA_RUSTCORE)
        // Container site rules moved into the decision store.
        SiteDecisionStore::reset();
#endif
    }
    m_server->requests.clear();
    m_server->cookieHeaders.clear();
    m_server->authHeaders.clear();
    m_server->indexHtml.clear();
    m_server->setCookie.clear();
    m_server->cacheControl.clear();
    m_server->protectedPath.clear();
    m_server->redirects.clear();
}

void tst_ContainerManager::cleanup()
{
    BrowserApplication::setTorMode(false);
    BrowserApplication::setPrivate(false);
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
    // CONT05: the composite privacy+adblock request interceptor is
    // attached (profile-parented; there is no getter) — the adblock
    // e2e proof is adblockAppliesOnContainerProfile below.
    QVERIFY(profile->findChild<PrivacyRequestInterceptor*>() != nullptr);
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

// CONT04: rule CRUD — set/list/remove/reassign, normalization
// (case, trailing dot, www fold, public-suffix guard), subdomain
// matching, persistence across a fresh manager, and rule death
// alongside the deleted container.
void tst_ContainerManager::siteRuleCrud()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString idA = create(QLatin1String("RuleA"));
    const QString idB = create(QLatin1String("RuleB"));
    QSignalSpy spy(manager, &ContainerManager::siteRulesChanged);

    // Refusals: empty host, non-host input, unknown container ids.
    QVERIFY(!manager->setSiteRule(QString(), idA));
    QVERIFY(!manager->setSiteRule(QLatin1String("not a host/"), idA));
    QVERIFY(!manager->setSiteRule(QLatin1String("example.com"),
                                 QLatin1String("bogus")));
    QVERIFY(!manager->removeSiteRule(QLatin1String("example.com")));

    // www folds to the apex: the stored key covers both spellings.
    QVERIFY(manager->setSiteRule(QLatin1String("WWW.Example.COM."), idA));
    QCOMPARE(manager->siteRules(idA),
             QStringList() << QLatin1String("example.com"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(manager->containerIdForHost(QLatin1String("www.example.com")),
             idA);
    // Subdomain coverage — deep names match the apex rule.
    QCOMPARE(manager->containerIdForHost(QLatin1String("a.b.example.com")),
             idA);
    // ...but siblings and lookalikes do not.
    QCOMPARE(manager->containerIdForHost(QLatin1String("notexample.com")),
             QString());
    QCOMPARE(manager->containerIdForHost(QLatin1String("example.com.evil.org")),
             QString());
    // A subdomain rule does not claim the apex.
    QVERIFY(manager->setSiteRule(QLatin1String("sub.example.org"), idA));
    QCOMPARE(manager->containerIdForHost(QLatin1String("example.org")),
             QString());
    QCOMPARE(manager->containerIdForHost(QLatin1String("sub.example.org")),
             idA);
    QCOMPARE(manager->containerIdForHost(QLatin1String("deep.sub.example.org")),
             idA);

    // Reassignment moves the host between containers — the old
    // owner's list loses it.
    QVERIFY(manager->setSiteRule(QLatin1String("example.com"), idB));
    QVERIFY(manager->siteRules(idA).isEmpty()
            || !manager->siteRules(idA).contains(QLatin1String("example.com")));
    QCOMPARE(manager->containerIdForHost(QLatin1String("example.com")), idB);
    QCOMPARE(manager->siteRules(idB),
             QStringList() << QLatin1String("example.com"));

    // Persistence: a fresh manager (the restart proxy) reads the
    // rules back from QSettings.
    ContainerManager fresh;
    QCOMPARE(fresh.containerIdForHost(QLatin1String("www.example.com")),
             idB);
    QCOMPARE(fresh.containerIdForHost(QLatin1String("sub.example.org")),
             idA);

    // Removal frees the host; the signal fires on every mutation.
    QVERIFY(manager->removeSiteRule(QLatin1String("example.com")));
    QCOMPARE(manager->containerIdForHost(QLatin1String("example.com")),
             QString());
    QVERIFY(!manager->removeSiteRule(QLatin1String("example.com")));

    // Deleting a container takes its remaining rules with it.
    QVERIFY(manager->deleteContainer(idA));
    m_created.removeAll(idA);
    QCOMPARE(manager->containerIdForHost(QLatin1String("sub.example.org")),
             QString());
}

// CONT04: the enforcement path — a navigation to a ruled host on the
// wrong profile is refused and reopened on the ruled container's
// profile in a fresh tab; removing the rule restores plain loading.
void tst_ContainerManager::siteRuleDiversion()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString id = create();
    QWebEngineProfile *containerProfile = manager->profileFor(id);
    QVERIFY(containerProfile);

    TabWidget widget;
    widget.newTab();
    WebView *tab = widget.currentWebView();
    QVERIFY(tab);
    QCOMPARE(tab->page()->profile(), BrowserApplication::webEngineProfile());

    // Assign the test server host to the container, then navigate
    // the default tab — the load diverts instead of committing.
    QVERIFY(manager->setSiteRule(QLatin1String("127.0.0.1"), id));
    const QUrl url = m_server->url(QLatin1String("/cont04.html"));
    tab->load(url);

    QTRY_VERIFY_WITH_TIMEOUT(widget.count() == 2, 15000);
    WebView *diverted = widget.webView(1);
    QVERIFY(diverted);
    QCOMPARE(diverted->page()->profile(), containerProfile);
    QCOMPARE(widget.containerIdForTab(1), id);
    // The source tab never committed — its page still sits on the
    // empty starting document, not the refused url.
    QVERIFY(tab->url().isEmpty()
            || tab->url() == QUrl(QLatin1String("about:blank")));

    // The diverted tab loads the url on the right profile and the
    // server sees exactly one request for it (the refused original
    // navigation never reached the network).
    QTRY_VERIFY_WITH_TIMEOUT(
        m_server->requests.contains(QLatin1String("/cont04.html")),
        15000);
    QCOMPARE(m_server->requests.count(QLatin1String("/cont04.html")), 1);
    QTRY_VERIFY_WITH_TIMEOUT(diverted->url() == url, 15000);

    // Unassigning restores plain loading on the default container.
    QVERIFY(manager->removeSiteRule(QLatin1String("127.0.0.1")));
    widget.closeTab(1);
    QVERIFY(loadSync(tab->page(), m_server->url(QLatin1String("/cont04-b.html"))));
    QCOMPARE(tab->page()->profile(), BrowserApplication::webEngineProfile());
    QCOMPARE(widget.count(), 1);
}

// CONT04: a mid-chain redirect hop onto a differently-ruled host
// re-diverts — localhost ruled to A serving a 302 to 127.0.0.1 ruled
// to B must end with the final page living on B's profile.
void tst_ContainerManager::siteRuleRedirectChain()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString idA = create();
    const QString idB = create();
    QWebEngineProfile *profileA = manager->profileFor(idA);
    QWebEngineProfile *profileB = manager->profileFor(idB);
    QVERIFY(profileA && profileB && profileA != profileB);

    QVERIFY(manager->setSiteRule(QLatin1String("localhost"), idA));
    QVERIFY(manager->setSiteRule(QLatin1String("127.0.0.1"), idB));

    const QUrl finalUrl = m_server->url(QLatin1String("/cont04-final.html"));
    m_server->redirects.insert(QLatin1String("/cont04-hop"),
                               finalUrl.toString());
    const QUrl startUrl(QString::fromLatin1(
        "http://localhost:%1/cont04-hop").arg(finalUrl.port()));

    TabWidget widget;
    widget.newTab();
    widget.currentWebView()->load(startUrl);

    // default -> divert localhost -> A tab -> 302 -> divert -> B tab.
    QTRY_VERIFY_WITH_TIMEOUT(widget.count() == 3, 20000);
    QCOMPARE(widget.webView(1)->page()->profile(), profileA);
    QCOMPARE(widget.webView(2)->page()->profile(), profileB);
    QTRY_VERIFY_WITH_TIMEOUT(
        m_server->requests.contains(QLatin1String("/cont04-final.html")),
        15000);
    QCOMPARE(m_server->requests.count(QLatin1String("/cont04-hop")), 1);
    QCOMPARE(m_server->requests.count(QLatin1String("/cont04-final.html")), 1);
    QTRY_VERIFY_WITH_TIMEOUT(widget.webView(2)->url() == finalUrl, 15000);
}

// CONT04: the diversion gates — private and tor contexts have no
// containers and load in place; non-http(s) urls are exempt; a page
// without a tab strip simply refuses instead of loading wrongly.
void tst_ContainerManager::siteRuleGates()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString id = create();
    QWebEngineProfile *containerProfile = manager->profileFor(id);
    QVERIFY(containerProfile);
    QVERIFY(manager->setSiteRule(QLatin1String("127.0.0.1"), id));

    // Private mode: the navigation proceeds on the OTR profile — a
    // persistent container could never take a private tab anyway.
    BrowserApplication::setPrivate(true);
    {
        TabWidget widget;
        widget.newTab();
        WebView *tab = widget.currentWebView();
        QVERIFY(tab);
        QVERIFY(loadSync(tab->page(),
                         m_server->url(QLatin1String("/cont04-priv.html"))));
        QCOMPARE(widget.count(), 1);
        QVERIFY(tab->page()->profile()->isOffTheRecord());
    }
    BrowserApplication::setPrivate(false);

    // about:/data: urls carry no host and never divert.
    {
        TabWidget widget;
        widget.newTab();
        WebView *tab = widget.currentWebView();
        QVERIFY(tab);
        QVERIFY(loadSync(tab->page(),
                         QUrl(QLatin1String("data:text/plain,hello"))));
        QCOMPARE(widget.count(), 1);
    }

    // A bare page on the default profile has no chrome to divert
    // into — the ruled load refuses instead of committing on the
    // wrong profile.  The refusal surfaces through the notfound.html
    // substitution (which itself can finish "successfully"), so the
    // honest check is on the wire: the request must never leave.
    {
        WebPage page(BrowserApplication::webEngineProfile());
        std::shared_ptr<bool> done(new bool(false));
        QMetaObject::Connection connection = QObject::connect(
            &page, &QWebEnginePage::loadFinished, &page,
            [done](bool) { *done = true; });
        page.load(m_server->url(QLatin1String("/cont04-refuse.html")));
        QVERIFY(waitFor(done));
        QObject::disconnect(connection);
        QVERIFY(!m_server->requests.contains(
            QLatin1String("/cont04-refuse.html")));
    }

    // A page already on the ruled profile commits normally — the
    // rule only fires across containers.
    {
        WebPage page(containerProfile);
        QVERIFY(loadSync(&page,
                         m_server->url(QLatin1String("/cont04-here.html"))));
        QVERIFY(m_server->requests.contains(
            QLatin1String("/cont04-here.html")));
    }
}

// CONT05 (c): private mode fails just as closed as tor — profileFor
// refuses every id including the default, "new tab in container"
// falls back to the single private profile, and reopen-in-container
// is a no-op.  A persistent container inside a private window would
// write to disk where the user expects nothing to be written.
void tst_ContainerManager::privateModeRefuses()
{
    const QString id = create();
    ContainerManager *manager = ContainerManager::instance();
    // Materialize first — the refusal must cover a live profile too.
    QVERIFY(manager->profileFor(id));

    BrowserApplication::setPrivate(true);
    QVERIFY(manager->profileFor(id) == nullptr);
    QVERIFY(manager->profileFor(QString()) == nullptr);
    // The private profile stays singular — every caller gets the one
    // OTR instance.
    QCOMPARE(BrowserApplication::webEngineProfile(),
             BrowserProfile::privateProfile());

    TabWidget widget;
    WebView *tab = widget.makeNewTabInContainer(id, true);
    QVERIFY(tab);
    QVERIFY(tab->page()->profile()->isOffTheRecord());
    QCOMPARE(tab->page()->profile(), BrowserProfile::privateProfile());
    QCOMPARE(tab->containerId(), QString());

    const int count = widget.count();
    widget.reopenTabInContainer(0, id);
    QCOMPARE(widget.count(), count);
    QCOMPARE(widget.containerIdForTab(0), QString());
    QCOMPARE(widget.webView(0)->page()->profile(),
             BrowserProfile::privateProfile());

    BrowserApplication::setPrivate(false);
    QVERIFY(manager->profileFor(id) != nullptr);
}

// CONT05 (a): DOM storage is profile-partitioned — localStorage,
// IndexedDB and service-worker registrations planted on container A
// are invisible to container B and to the default profile on the SAME
// origin (the test server answers every profile identically).
void tst_ContainerManager::domStorageIsolation()
{
    const QString idA = create();
    const QString idB = create();
    ContainerManager *manager = ContainerManager::instance();
    QWebEngineProfile *profileA = manager->profileFor(idA);
    QWebEngineProfile *profileB = manager->profileFor(idB);
    QVERIFY(profileA && profileB && profileA != profileB);

    // The seed page writes all three storage classes and raises
    // __cont05seeded once IndexedDB committed and the service-worker
    // registration resolved (or proved unsupported — __cont05sw then
    // stays 'none' and the reader's sw check is skipped).
    m_server->indexHtml = QByteArray(
        "<html><body><script>"
        "try{localStorage.setItem('cont05-ls','containerA')}catch(e){}"
        "window.__cont05sw='none';"
        "var cont05Left=2;"
        "function cont05Done(){if(--cont05Left===0)window.__cont05seeded='yes'}"
        "try{var rq=indexedDB.open('cont05-db',1);"
        "rq.onupgradeneeded=function(e){e.target.result.createObjectStore('kv')};"
        "rq.onsuccess=function(e){var tx=e.target.result.transaction('kv','readwrite');"
        "tx.objectStore('kv').put('containerA','marker');"
        "tx.oncomplete=cont05Done;tx.onerror=cont05Done};"
        "rq.onerror=cont05Done}catch(e){cont05Done()}"
        "try{if(navigator.serviceWorker&&navigator.serviceWorker.register){"
        "navigator.serviceWorker.register('cont05-sw.js').then("
        "function(){window.__cont05sw='yes';cont05Done()},"
        "function(){cont05Done()})}else{cont05Done()}}"
        "catch(e){cont05Done()}"
        "</script></body></html>");

    WebPage pageA(profileA);
    QVERIFY(loadSync(&pageA, m_server->url(QLatin1String("/cont05-a.html"))));
    QCOMPARE(evalJsUntil(&pageA,
                         QStringLiteral("String(window.__cont05seeded||'')")),
             QLatin1String("yes"));
    const QString swSeed = evalJs(&pageA,
        QStringLiteral("String(window.__cont05sw||'none')")).toString();
    qInfo() << "cont05 service-worker seed state:" << swSeed;
    QCOMPARE(evalJs(&pageA,
        QStringLiteral("String(localStorage.getItem('cont05-ls'))")).toString(),
        QLatin1String("containerA"));

    // Read back on another profile of the same origin: "idb:fresh"
    // means open() ran its upgrade — the database did not exist there;
    // "sw:0" means no service-worker registrations are visible.
    static const char probeScript[] =
        "(function(){"
        "var r='ls:'+String(localStorage.getItem('cont05-ls'));"
        "var left=2;"
        "function fin(){if(--left===0)window.__cont05probe=r}"
        "try{var rq=indexedDB.open('cont05-db',1);"
        "rq.onupgradeneeded=function(){r+=',idb:fresh'};"
        "rq.onsuccess=function(e){if(r.indexOf('idb:')<0)r+=',idb:'+"
        "(e.target.result.objectStoreNames.contains('kv')?'leaked':'empty');"
        "fin()};"
        "rq.onerror=function(){r+=',idb:error';fin()}"
        "}catch(e){r+=',idb:throw';fin()}"
        "try{if(navigator.serviceWorker&&navigator.serviceWorker.getRegistrations){"
        "navigator.serviceWorker.getRegistrations().then("
        "function(rs){r+=',sw:'+rs.length;fin()},"
        "function(){r+=',sw:error';fin()})}else{r+=',sw:none';fin()}}"
        "catch(e){r+=',sw:throw';fin()}"
        "})();'go'";

    m_server->indexHtml.clear();
    const auto probeProfile = [&](QWebEngineProfile *profile,
                                  const char *path) -> QString {
        WebPage page(profile);
        if (!loadSync(&page, m_server->url(QLatin1String(path))))
            return QStringLiteral("load-failed");
        evalJs(&page, QLatin1String(probeScript));
        return evalJsUntil(&page,
            QStringLiteral("String(window.__cont05probe||'')"));
    };

    const QString fromB = probeProfile(profileB, "/cont05-b.html");
    QVERIFY2(fromB.contains(QLatin1String("ls:null")), qPrintable(fromB));
    QVERIFY2(fromB.contains(QLatin1String(",idb:fresh")), qPrintable(fromB));
    if (swSeed == QLatin1String("yes"))
        QVERIFY2(fromB.contains(QLatin1String(",sw:0")), qPrintable(fromB));

    const QString fromDefault =
        probeProfile(BrowserApplication::webEngineProfile(), "/cont05-d.html");
    QVERIFY2(fromDefault.contains(QLatin1String("ls:null")),
             qPrintable(fromDefault));
    QVERIFY2(fromDefault.contains(QLatin1String(",idb:fresh")),
             qPrintable(fromDefault));
    if (swSeed == QLatin1String("yes"))
        QVERIFY2(fromDefault.contains(QLatin1String(",sw:0")),
                 qPrintable(fromDefault));
}

// CONT05 (a): Chromium's preemptive http-auth cache lives in the
// profile's network context — after container A authenticates, the
// same realm on B and on the default profile challenges again, and
// the wire never sees A's credentials leaving A's partition.
void tst_ContainerManager::httpAuthIsolation()
{
    const QString idA = create();
    const QString idB = create();
    ContainerManager *manager = ContainerManager::instance();
    QWebEngineProfile *profileA = manager->profileFor(idA);
    QWebEngineProfile *profileB = manager->profileFor(idB);
    QVERIFY(profileA && profileB);

    // The realm is a path prefix — each profile exercises its own url
    // so wire assertions can attribute Authorization headers without
    // in-flight noise (e.g. a late favicon fetch on the same origin)
    // polluting another profile's window.
    m_server->protectedPath = QLatin1String("/cont05-auth");
    const QByteArray credsA = QByteArray("aliceA:secretA").toBase64();
    const QByteArray credsB = QByteArray("bobB:secretB").toBase64();
    const QByteArray credsD = QByteArray("carolD:secretD").toBase64();
    const auto authsFor = [this](const QString &path) {
        QStringList out;
        for (const QString &entry : m_server->authHeaders) {
            if (entry.startsWith(path + QLatin1Char(' ')))
                out << entry;
        }
        return out.join(QLatin1Char(';'));
    };

    const QString pathA = QLatin1String("/cont05-auth-a.html");
    int challengesA = 0;
    {
        WebPage pageA(profileA);
        connect(&pageA, &QWebEnginePage::authenticationRequired, this,
            [&challengesA](const QUrl &, QAuthenticator *auth) {
                ++challengesA;
                auth->setUser(QLatin1String("aliceA"));
                auth->setPassword(QLatin1String("secretA"));
            });
        QVERIFY(loadSync(&pageA, m_server->url(pathA)));
        QVERIFY(challengesA >= 1);
        QVERIFY(authsFor(pathA).contains(QLatin1String(credsA)));
    }

    // A repeat load inside A answers without a challenge — the
    // credentials are cached, just inside A's partition.
    int challengesA2 = 0;
    {
        WebPage pageA2(profileA);
        connect(&pageA2, &QWebEnginePage::authenticationRequired, this,
            [&challengesA2](const QUrl &, QAuthenticator *) {
                ++challengesA2;
            });
        QVERIFY(loadSync(&pageA2, m_server->url(pathA)));
        QCOMPARE(challengesA2, 0);
    }

    // B gets challenged — a shared auth cache would have answered
    // silently with A's credentials.
    const QString pathB = QLatin1String("/cont05-auth-b.html");
    int challengesB = 0;
    {
        WebPage pageB(profileB);
        connect(&pageB, &QWebEnginePage::authenticationRequired, this,
            [&challengesB](const QUrl &, QAuthenticator *auth) {
                ++challengesB;
                auth->setUser(QLatin1String("bobB"));
                auth->setPassword(QLatin1String("secretB"));
            });
        QVERIFY(loadSync(&pageB, m_server->url(pathB)));
        QVERIFY(challengesB >= 1);
    }
    QVERIFY(authsFor(pathB).contains(QLatin1String(credsB)));
    QVERIFY(!authsFor(pathB).contains(QLatin1String(credsA)));

    // The default container challenges too, with its own credentials.
    const QString pathD = QLatin1String("/cont05-auth-d.html");
    int challengesD = 0;
    {
        WebPage pageD(BrowserApplication::webEngineProfile());
        connect(&pageD, &QWebEnginePage::authenticationRequired, this,
            [&challengesD](const QUrl &, QAuthenticator *auth) {
                ++challengesD;
                auth->setUser(QLatin1String("carolD"));
                auth->setPassword(QLatin1String("secretD"));
            });
        QVERIFY(loadSync(&pageD, m_server->url(pathD)));
        QVERIFY(challengesD >= 1);
    }
    QVERIFY(authsFor(pathD).contains(QLatin1String(credsD)));
    QVERIFY(!authsFor(pathD).contains(QLatin1String(credsA)));
    QVERIFY(!authsFor(pathD).contains(QLatin1String(credsB)));
}

// CONT05 (a): the http cache is per-profile — a resource container A
// cached fresh (max-age) is fetched again by container B rather than
// served from A's store.  A's repeat load proves the cache works at
// all, so "B fetched" is isolation, not a dead cache.
void tst_ContainerManager::httpCacheIsolation()
{
    const QString idA = create();
    const QString idB = create();
    ContainerManager *manager = ContainerManager::instance();
    QWebEngineProfile *profileA = manager->profileFor(idA);
    QWebEngineProfile *profileB = manager->profileFor(idB);
    QVERIFY(profileA && profileB);

    m_server->cacheControl = "public, max-age=3600";
    const QString path = QLatin1String("/cont05-cache.png");
    const QUrl url = m_server->url(path);
    const auto hits = [this, &path] {
        return m_server->requests.count(path);
    };

    {
        WebPage pageA(profileA);
        QVERIFY(loadSync(&pageA, url));
        QCOMPARE(hits(), 1);
    }
    {
        WebPage pageA2(profileA);
        QVERIFY(loadSync(&pageA2, url));
        QTest::qWait(300);
        QCOMPARE(hits(), 1);    // served from A's own cache
    }
    {
        WebPage pageB(profileB);
        QVERIFY(loadSync(&pageB, url));
        QCOMPARE(hits(), 2);    // A's cached copy is unreachable
    }
    {
        WebPage pageDefault(BrowserApplication::webEngineProfile());
        QVERIFY(loadSync(&pageDefault, url));
        QCOMPARE(hits(), 3);
    }
}

// CONT05: the derived on-disk paths — Qt keys a named profile's cache
// dir and default-storage residue off the storage name, not the
// overridden persistentStoragePath.  cachePathFor must predict the
// live profile's real cachePath, and deleteContainer must remove all
// three trees even when the profile was never materialized.
void tst_ContainerManager::derivedPathCleanup()
{
    ContainerManager *manager = ContainerManager::instance();

    const QString idLive = create();
    QWebEngineProfile *profile = manager->profileFor(idLive);
    QVERIFY(profile);
    QCOMPARE(manager->cachePathFor(idLive), profile->cachePath());
    QCOMPARE(manager->defaultDataPathFor(idLive),
             QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
             + QLatin1String("/QtWebEngine/") + profile->storageName());
    QVERIFY(!profile->cachePath().contains(QLatin1String("/containers/")));

    // A never-materialized container still has the on-disk residue an
    // earlier session would have left — deletion must reach all of it.
    const QString id = create();
    const QString storage = manager->storagePath(id);
    const QString cache = manager->cachePathFor(id);
    const QString dflt = manager->defaultDataPathFor(id);
    QVERIFY(!manager->profileIfCreated(id));
    QVERIFY(QDir().mkpath(storage + QLatin1String("/Local Storage")));
    QVERIFY(QDir().mkpath(cache + QLatin1String("/Cache")));
    QVERIFY(QDir().mkpath(dflt));
    QVERIFY(writeMarker(storage + QLatin1String("/Cookies")));

    QVERIFY(manager->deleteContainer(id));
    m_created.removeAll(id);
    QVERIFY(!QDir(storage).exists());
    QVERIFY(!QDir(cache).exists());
    QVERIFY(!QDir(dflt).exists());
}

// CONT05: the ClearPrivateData claim reaches containers whose profile
// was never materialized — on-disk cookies, site storage, cache and
// visited-links files go away directly, while a materialized
// container is skipped (its clears run through the live profile; the
// trees must never be ripped out from under it, HARD01).
void tst_ContainerManager::unmaterializedClear()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString id = create();
    const QString storage = manager->storagePath(id);
    const QString cache = manager->cachePathFor(id);
    QVERIFY(QDir().mkpath(storage + QLatin1String("/Network")));
    QVERIFY(writeMarker(storage + QLatin1String("/Cookies")));
    QVERIFY(writeMarker(storage + QLatin1String("/Network/Cookies")));
    QVERIFY(writeMarker(storage + QLatin1String("/Visited Links")));
    QVERIFY(QDir().mkpath(storage + QLatin1String("/Local Storage")));
    QVERIFY(QDir().mkpath(storage + QLatin1String("/IndexedDB")));
    QVERIFY(QDir().mkpath(cache + QLatin1String("/Cache")));

    // Flag granularity: site-data only leaves the cookies alone.
    const QString id2 = create();
    const QString storage2 = manager->storagePath(id2);
    QVERIFY(QDir().mkpath(storage2 + QLatin1String("/Local Storage")));
    QVERIFY(writeMarker(storage2 + QLatin1String("/Cookies")));

    const QString idLive = create();
    QWebEngineProfile *live = manager->profileFor(idLive);
    QVERIFY(live);
    QVERIFY(QDir().mkpath(live->persistentStoragePath()
                          + QLatin1String("/cont05-live-marker")));

    manager->clearUnmaterializedStorage(false, true, false, false);
    QVERIFY(!QDir(storage + QLatin1String("/Local Storage")).exists());
    QVERIFY(!QDir(storage + QLatin1String("/IndexedDB")).exists());
    QVERIFY(QFile::exists(storage + QLatin1String("/Cookies")));
    QVERIFY(QFile::exists(storage + QLatin1String("/Visited Links")));
    QVERIFY(QDir(cache).exists());
    QVERIFY(!QDir(storage2 + QLatin1String("/Local Storage")).exists());
    QVERIFY(QFile::exists(storage2 + QLatin1String("/Cookies")));

    manager->clearUnmaterializedStorage(true, false, true, true);
    QVERIFY(!QFile::exists(storage + QLatin1String("/Cookies")));
    QVERIFY(!QFile::exists(storage + QLatin1String("/Network/Cookies")));
    QVERIFY(!QFile::exists(storage + QLatin1String("/Visited Links")));
    QVERIFY(!QDir(cache).exists());

    // The materialized container's trees were skipped throughout.
    QVERIFY(QDir(live->persistentStoragePath()
                 + QLatin1String("/cont05-live-marker")).exists());

    // The clear-all-on-exit counterpart removes every derived tree
    // outright — the registry entry survives, only profile data goes.
    const QString id3 = create();
    const QString storage3 = manager->storagePath(id3);
    const QString cache3 = manager->cachePathFor(id3);
    const QString dflt3 = manager->defaultDataPathFor(id3);
    QVERIFY(QDir().mkpath(storage3 + QLatin1String("/Network")));
    QVERIFY(QDir().mkpath(cache3));
    QVERIFY(QDir().mkpath(dflt3));
    manager->wipeUnmaterializedStorage();
    QVERIFY(!QDir(storage3).exists());
    QVERIFY(!QDir(cache3).exists());
    QVERIFY(!QDir(dflt3).exists());
    QVERIFY(manager->isContainerId(id3));
}

// CONT05: end-to-end through the dialog — every checkbox ticked in
// Clear Private Data must empty a container that was never opened
// this session, not just the live profiles.
void tst_ContainerManager::clearDialogCoversUnmaterialized()
{
    ContainerManager *manager = ContainerManager::instance();
    const QString id = create();
    const QString storage = manager->storagePath(id);
    const QString cache = manager->cachePathFor(id);
    QVERIFY(writeMarker(storage + QLatin1String("/Cookies")));
    QVERIFY(writeMarker(storage + QLatin1String("/Visited Links")));
    QVERIFY(QDir().mkpath(storage + QLatin1String("/Local Storage")));
    QVERIFY(QDir().mkpath(cache));

    ClearPrivateData dialog;
    const QList<QCheckBox*> boxes = dialog.findChildren<QCheckBox*>();
    QVERIFY(!boxes.isEmpty());
    for (QCheckBox *box : boxes)
        box->setChecked(true);
    dialog.accept();

    QVERIFY(!QFile::exists(storage + QLatin1String("/Cookies")));
    QVERIFY(!QFile::exists(storage + QLatin1String("/Visited Links")));
    QVERIFY(!QDir(storage + QLatin1String("/Local Storage")).exists());
    QVERIFY(!QDir(cache).exists());
}

// CONT05 (b): history is an application-level store — a container
// page's visit lands in the same global HistoryManager a default tab
// feeds.  Deliberate and documented (README "Containers"); the test
// pins it so a silent behavior change trips a failure.
void tst_ContainerManager::historyIsGlobalByDesign()
{
    const QString id = create();
    QWebEngineProfile *profile = ContainerManager::instance()->profileFor(id);
    QVERIFY(profile);
    const QUrl url = m_server->url(QLatin1String("/cont05-history.html"));
    {
        WebPage page(profile);
        QVERIFY(loadSync(&page, url));
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        HistoryManager::instance()->historyContains(url.toString()), 15000);
}

// CONT06: with the two-level strip on (the default), a window whose
// tabs span containers shows only the ACTIVE level's tabs — the rest
// detach into their own level's store.  The default "Tabs" header is
// always first, container headers appear once they own a tab, the
// counts cover visible + hidden tabs, and a background container tab
// never leaks into the wrong level.
void tst_ContainerManager::twoLevelStripBasics()
{
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    const QString id = create(QLatin1String("Work"));

    TabWidget widget;
    QVERIFY(widget.twoLevelStrip());
    widget.newTab();
    WebView *containerTab = widget.makeNewTabInContainer(id, false);
    QVERIFY(containerTab);

    // Two levels exist: the pinned default header + the container's.
    QVERIFY(widget.containerStripActive());
    QCOMPARE(widget.containerHeaders(),
             (QStringList() << QString() << id));
    QCOMPARE(widget.activeContainerHeader(), QString());
    QCOMPARE(widget.containerTabCount(QString()), 1);
    QCOMPARE(widget.containerTabCount(id), 1);
    // A background container tab joins its level's hidden row — the
    // visible strip holds only default-level tabs.
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.containerIdForTab(0), QString());

    // The level-1 band is real height on the bar and carries the
    // header names/counts for assistive tools.
    TabBar *bar = widget.tabBar();
    widget.resize(600, 400);
    QVERIFY(bar->containerStripHeight() > 0);
    QCOMPARE(bar->containerHeaderAt(QPoint(6, 2)), 0);
    QVERIFY(bar->accessibleDescription().contains(QLatin1String("Work")));

    // Switching headers swaps the visible row; the other level waits
    // in its store — no tab is lost or mixed.
    widget.setActiveContainerHeader(id);
    QCOMPARE(widget.activeContainerHeader(), id);
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.webView(0), containerTab);
    for (int i = 0; i < widget.count(); ++i)
        QCOMPARE(widget.containerIdForTab(i), id);

    widget.setActiveContainerHeader(QString());
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.containerIdForTab(0), QString());
    QCOMPARE(widget.containerTabCount(id), 1);
}

// CONT06: a container level whose last tab closes drops off the
// header row entirely, and the strip falls back to the default level —
// the lastTabClosed signal must not fire while other levels still
// hold tabs.
void tst_ContainerManager::twoLevelStripEmptyLevelFallsBack()
{
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    const QString idA = create(QLatin1String("A"));
    const QString idB = create(QLatin1String("B"));

    TabWidget widget;
    QSignalSpy closedSpy(&widget, &TabWidget::lastTabClosed);
    widget.newTab();
    widget.makeNewTabInContainer(idB, false);
    WebView *a = widget.makeNewTabInContainer(idA, true);
    QVERIFY(a);
    QCOMPARE(widget.activeContainerHeader(), idA);
    QCOMPARE(widget.containerHeaders().count(), 3);

    // Closing the only tab of the active level empties it out of the
    // strip and lands back on the default level.
    widget.closeTab(0);
    QCOMPARE(widget.containerTabCount(idA), 0);
    QVERIFY(!widget.containerHeaders().contains(idA));
    QCOMPARE(widget.activeContainerHeader(), QString());
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.containerIdForTab(0), QString());
    QVERIFY(closedSpy.isEmpty());

    // The same holds when the LAST non-default level empties — the
    // band itself disappears (single level = single row).  idB holds
    // the earlier background tab plus this one — close both.
    widget.makeNewTabInContainer(idB, true);
    QCOMPARE(widget.activeContainerHeader(), idB);
    QVERIFY(widget.containerStripActive());
    widget.closeTab(0);
    QCOMPARE(widget.containerTabCount(idB), 1);
    widget.closeTab(0);
    QCOMPARE(widget.containerHeaders(),
             QStringList() << QString());
    QVERIFY(!widget.containerStripActive());
    QCOMPARE(widget.tabBar()->containerStripHeight(), 0);
    QCOMPARE(widget.count(), 1);
    QVERIFY(closedSpy.isEmpty());
}

// CONT06: dropping a level-2 tab on a level-1 header rebinds it —
// reopen-in-container semantics: a fresh tab on the target profile,
// the old one closes inside its (now hidden) level.
void tst_ContainerManager::twoLevelStripReopenAcrossLevels()
{
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    const QString id = create();
    ContainerManager *manager = ContainerManager::instance();

    TabWidget widget;
    widget.newTab();
    widget.makeNewTabInContainer(id, false);
    QCOMPARE(widget.activeContainerHeader(), QString());
    QCOMPARE(widget.containerTabCount(id), 1);

    QPointer<WebView> orig = widget.webView(0);
    widget.reopenTabInContainer(0, id);

    // The replacement raised the strip to the target level; the old
    // tab's original level kept its remaining tabs.
    QCOMPARE(widget.activeContainerHeader(), id);
    QCOMPARE(widget.containerTabCount(id), 2);
    QCOMPARE(widget.containerTabCount(QString()), 0);
    QCOMPARE(widget.count(), 2);
    for (int i = 0; i < widget.count(); ++i) {
        QCOMPARE(widget.containerIdForTab(i), id);
        QCOMPARE(widget.webView(i)->page()->profile(),
                 manager->profileFor(id));
    }
    // The rebind was a swap, not a move — the old page is gone.
    QTRY_VERIFY_WITH_TIMEOUT(orig.isNull(), 5000);
}

// CONT06: the active level rides the session blob (v4) — a window
// saved on a container level restores onto it, every hidden level's
// tabs come back, and the saved-current tab reselects inside its
// level.
void tst_ContainerManager::twoLevelStripSessionRoundTrip()
{
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    const QString idA = create(QLatin1String("A"));
    const QString idB = create(QLatin1String("B"));

    QByteArray state;
    {
        TabWidget widget;
        widget.newTab();
        widget.makeNewTabInContainer(idA, true);
        widget.makeNewTabInContainer(idA, false);
        widget.makeNewTabInContainer(idB, false);
        QCOMPARE(widget.activeContainerHeader(), idA);
        QCOMPARE(widget.count(), 2);
        state = widget.saveState();
    }

    TabWidget restored;
    QVERIFY(restored.restoreState(state));
    QCOMPARE(restored.activeContainerHeader(), idA);
    // The visible row is exactly the saved level; nothing mixed in.
    QCOMPARE(restored.count(), 2);
    for (int i = 0; i < restored.count(); ++i)
        QCOMPARE(restored.containerIdForTab(i), idA);
    // Hidden levels survived the round trip intact.
    QCOMPARE(restored.containerTabCount(QString()), 1);
    QCOMPARE(restored.containerTabCount(idB), 1);
    QCOMPARE(restored.containerHeaders(),
             (QStringList() << QString() << idA << idB));
    // The saved current tab — a level-A tab — is current again.
    QVERIFY(restored.currentWebView());
    QCOMPARE(restored.currentWebView()->containerId(), idA);

    // The other way round: a blob saved on the DEFAULT header with the
    // container levels filtered out restores every level too.
    QByteArray defaultState;
    {
        TabWidget widget;
        widget.newTab();
        widget.makeNewTabInContainer(idB, false);
        defaultState = widget.saveState();
    }
    TabWidget restoredDefault;
    QVERIFY(restoredDefault.restoreState(defaultState));
    QCOMPARE(restoredDefault.activeContainerHeader(), QString());
    QCOMPARE(restoredDefault.count(), 1);
    QCOMPARE(restoredDefault.containerTabCount(idB), 1);
}

// CONT06: groups stay nested inside their container level — a
// collapsed group in a filtered-out level hides with it, and its
// members still serialize (the chip carries them through
// orderedWebViews' hidden-level pass).
void tst_ContainerManager::twoLevelStripGroupStaysNested()
{
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    const QString id = create();

    TabWidget widget;
    widget.newTab();
    WebView *a = widget.makeNewTabInContainer(id, true);
    WebView *b = widget.makeNewTabInContainer(id, false);
    QVERIFY(a && b);
    widget.groupTabWith(widget.webViewIndex(b), widget.webViewIndex(a));
    const QString gid = widget.tabGroupId(widget.webViewIndex(a));
    QVERIFY(!gid.isEmpty());
    widget.setTabGroupCollapsed(gid, true);
    QVERIFY(widget.hasCollapsedTabGroup());

    // Filtering the group's whole level out detaches the chip into
    // the level's store; the collapsed members stay with the group.
    widget.setActiveContainerHeader(QString());
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.containerIdForTab(0), QString());
    QCOMPARE(widget.containerTabCount(id), 2);

    // Session round-trip loses nothing — both group members come
    // back in the container's level.
    const QByteArray state = widget.saveState();
    TabWidget restored;
    QVERIFY(restored.restoreState(state));
    QCOMPARE(restored.containerTabCount(id), 2);
    QCOMPARE(restored.containerTabCount(QString()), 1);
    restored.setActiveContainerHeader(id);
    QCOMPARE(restored.count(), 2);
    const QString restoredGid = restored.tabGroupId(0);
    QVERIFY(!restoredGid.isEmpty());
    QCOMPARE(restored.tabGroupId(0), restored.tabGroupId(1));
}

// CONT06: level-1 headers drag-reorder — the default header is pinned
// first, the rest move freely.
void tst_ContainerManager::twoLevelStripHeaderReorder()
{
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    const QString idA = create(QLatin1String("A"));
    const QString idB = create(QLatin1String("B"));

    TabWidget widget;
    widget.newTab();
    widget.makeNewTabInContainer(idA, false);
    widget.makeNewTabInContainer(idB, false);
    const QStringList initial =
        QStringList() << QString() << idA << idB;
    QCOMPARE(widget.containerHeaders(), initial);

    // The pinned default header cannot move.
    widget.moveContainerHeader(0, 1);
    QCOMPARE(widget.containerHeaders(), initial);

    // Container headers reorder around it.
    widget.moveContainerHeader(2, 1);
    QCOMPARE(widget.containerHeaders(),
             (QStringList() << QString() << idB << idA));
}

QTEST_MAIN(tst_ContainerManager)
#include "tst_containermanager.moc"
