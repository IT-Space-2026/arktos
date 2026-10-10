/**
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <qtest.h>
#include <qsignalspy.h>
#include <qtry.h>

#include "adblockdialog.h"
#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "adblockpresets.h"
#include "adblockpresetsdialog.h"
#include "adblocksubscription.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

#include <qcheckbox.h>
#include <qdebug.h>
#include <qset.h>
#include <qsettings.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qurlquery.h>

class tst_AdBlockManager : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void adblockmanager_data();
    void adblockmanager();

    void addSubscription();
    void customRules();
    void isEnabled_data();
    void isEnabled();
    void load();
    void removeSubscription();
    void showDialog();
    void rulesChanged();

    void presetCatalog();
    void subscribeRemoteList();
    void presetsDialog();
    void engineSelection();
};

// Subclass that exposes the protected functions.
class SubAdBlockManager : public AdBlockManager
{
public:
    ~SubAdBlockManager() {
        QList<AdBlockSubscription*> list = subscriptions();
        for (AdBlockSubscription *s : list)
            removeSubscription(s);
        setEnabled(false);
    }

    void call_rulesChanged()
        { return SubAdBlockManager::rulesChanged(); }
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_AdBlockManager::initTestCase()
{
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_AdBlockManager::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_AdBlockManager::init()
{
    // AdBlockManager::load() seeds several default subscriptions when no
    // setting exists; persist a list containing just the custom-rules
    // subscription so subscriptions() is deterministic.  The temporary
    // manager's destructor persists enabled=false plus an emptied list,
    // so the seed is written only after it is gone.
    QSettings settings;
    settings.clear();
#if defined(ARORA_RUSTCORE)
    // The site whitelist moved out of the custom-rules file into the
    // decision store — start each test from an empty set.
    SiteDecisionStore::clear(SiteDecisionStore::KindAdBlock);
#endif
    QString customUrl;
    {
        SubAdBlockManager manager;
        customUrl = QString::fromUtf8(manager.customRules()->url().toEncoded());
    }
    settings.beginGroup(QLatin1String("AdBlock"));
    settings.setValue(QLatin1String("enabled"), true);
    settings.setValue(QLatin1String("subscriptions"), QStringList() << customUrl);
}

// This will be called after every test function.
void tst_AdBlockManager::cleanup()
{
}

void tst_AdBlockManager::adblockmanager_data()
{
}

void tst_AdBlockManager::adblockmanager()
{
    SubAdBlockManager manager;
    manager.addSubscription((AdBlockSubscription*)0);
    QVERIFY(manager.customRules());
    QVERIFY(manager.instance() != (AdBlockManager*)0);
    QCOMPARE(manager.isEnabled(), true);
    manager.load();
    QVERIFY(manager.network());
    QVERIFY(manager.page());
    manager.removeSubscription((AdBlockSubscription*)0);
    manager.setEnabled(false);
    QVERIFY(manager.showDialog());
    QList<AdBlockSubscription*> list;
    list.append(manager.customRules());
    QCOMPARE(manager.subscriptions(), list);
}

// public void addSubscription(AdBlockSubscription *subscription)
void tst_AdBlockManager::addSubscription()
{
    SubAdBlockManager manager;

    QList<AdBlockSubscription*> list = manager.subscriptions();

    QSignalSpy spy0(&manager, SIGNAL(rulesChanged()));

    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), &manager);
    manager.addSubscription(subscription);
    QCOMPARE(manager.subscriptions(), (list += subscription));

    QCOMPARE(spy0.count(), 1);
}

// public AdBlockSubscription *customRules()
void tst_AdBlockManager::customRules()
{
    SubAdBlockManager manager;
    QSignalSpy spy0(&manager, SIGNAL(rulesChanged()));

    AdBlockSubscription *subscription = manager.customRules();
    QVERIFY(subscription);
    QVERIFY(!subscription->title().isEmpty());
    QVERIFY(subscription->allRules().isEmpty());

    QCOMPARE(spy0.count(), 1);

    subscription = manager.customRules();
    QCOMPARE(spy0.count(), 1);
}

void tst_AdBlockManager::isEnabled_data()
{
    QTest::addColumn<bool>("isEnabled");
    QTest::newRow("true") << true;
    QTest::newRow("false") << false;
}

// public bool isEnabled() const
void tst_AdBlockManager::isEnabled()
{
    QFETCH(bool, isEnabled);

    SubAdBlockManager manager;

    QSignalSpy spy0(&manager, SIGNAL(rulesChanged()));

    bool before = manager.isEnabled();

    manager.setEnabled(isEnabled);
    manager.setEnabled(isEnabled);
    QCOMPARE(manager.isEnabled(), isEnabled);

    QCOMPARE(spy0.count(), before == isEnabled ? 0 : 1);
}

// public void load()
void tst_AdBlockManager::load()
{
    SubAdBlockManager manager;

    QSignalSpy spy0(&manager, SIGNAL(rulesChanged()));

    manager.load();

    QCOMPARE(spy0.count(), 0);
}

// public void removeSubscription(AdBlockSubscription *subscription)
void tst_AdBlockManager::removeSubscription()
{
    SubAdBlockManager manager;

    QSignalSpy spy0(&manager, SIGNAL(rulesChanged()));

    QList<AdBlockSubscription*> list = manager.subscriptions();
    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), &manager);
    manager.addSubscription(subscription);
    manager.removeSubscription(subscription);
    QCOMPARE(manager.subscriptions(), list);

    QCOMPARE(spy0.count(), 2);
}


// public AdBlockDialog *showDialog()
void tst_AdBlockManager::showDialog()
{
    SubAdBlockManager manager;

    QSignalSpy spy0(&manager, SIGNAL(rulesChanged()));

    AdBlockDialog *dialog = manager.showDialog();
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
}

void tst_AdBlockManager::rulesChanged()
{
    SubAdBlockManager manager;

    QSignalSpy spy0(&manager, SIGNAL(rulesChanged()));


    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), &manager);
    manager.addSubscription(subscription);
    subscription->setEnabled(true);
    subscription->addRule(AdBlockRule());

    QCOMPARE(spy0.count(), 3);
}

// ADB03: the preset catalog must be non-empty, deduplicated, and
// https-only so ticking a box never silently fetches over plaintext.
void tst_AdBlockManager::presetCatalog()
{
    const QList<AdBlockListPreset> presets = AdBlockPresets::all();
    QVERIFY(presets.count() >= 10);

    QSet<QString> locations;
    QSet<QString> categories;
    for (const AdBlockListPreset &preset : presets) {
        QVERIFY(!preset.category.isEmpty());
        QVERIFY(!preset.title.isEmpty());
        QVERIFY(!preset.description.isEmpty());
        const QUrl location(preset.location);
        QVERIFY2(location.isValid(), qPrintable(preset.location));
        QCOMPARE(location.scheme(), QLatin1String("https"));
        QVERIFY2(!locations.contains(preset.location),
                 qPrintable(preset.location));
        locations.insert(preset.location);
        categories.insert(preset.category);
    }
    // Security (phishing/malware) and anti-mining coverage are part of
    // the task, not optional.
    QVERIFY(presets.count() >= 5);
    QVERIFY(categories.count() >= 4);
}

// ADB03: subscribeRemoteList adds the subscription, grants the TELEM01
// remote-list consent, and kicks the fetch; the downloaded rules land
// in the matcher snapshot — the same rebuildRules() path the Rust
// engine serializes under CONFIG+=adblock_rust.
void tst_AdBlockManager::subscribeRemoteList()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QUrl location(QStringLiteral("http://127.0.0.1:%1/preset.txt")
                        .arg(server.serverPort()));
    auto serveList = [&server]() {
        QVERIFY(server.waitForNewConnection(8000));
        QTcpSocket *client = server.nextPendingConnection();
        QTRY_VERIFY(client->bytesAvailable() > 0);
        QVERIFY(client->readAll().startsWith("GET /preset.txt"));
        const QByteArray body =
            "[Adblock Plus 0.7.1]\n||preset-smoke.invalid^\n";
        client->write("HTTP/1.1 200 OK\r\nContent-Length: "
                      + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n\r\n" + body);
        client->disconnectFromHost();
    };

    AdBlockManager::setRemoteListsConsent(
        AdBlockManager::RemoteListsUndecided);

    // The matcher (network()->rebuildRules) snapshots the singleton's
    // subscription list, so this test drives the real instance() —
    // cleaned up at the end.
    AdBlockManager *manager = AdBlockManager::instance();
    manager->load();
    const int before = manager->subscriptions().count();

    AdBlockSubscription *subscription =
        manager->subscribeRemoteList(location, QLatin1String("Preset Test"));
    QVERIFY(subscription);
    QCOMPARE(subscription->location(), location);
    QCOMPARE(subscription->title(), QLatin1String("Preset Test"));
    QVERIFY(subscription->isEnabled());
    QCOMPARE(manager->subscriptions().count(), before + 1);
    QCOMPARE(manager->subscriptionForLocation(location), subscription);
    QVERIFY(!manager->subscriptionForLocation(
        QUrl(QLatin1String("http://127.0.0.1:1/none.txt"))));
    QCOMPARE(AdBlockManager::remoteListsConsent(),
             AdBlockManager::RemoteListsGranted);

    serveList();
    QTRY_VERIFY(subscription->lastUpdate().isValid());
    QVERIFY(subscription->allRules().count() >= 1);

    manager->network()->rebuildRules();
    QVERIFY(manager->network()->shouldBlock(
        QUrl(QLatin1String("http://preset-smoke.invalid/banner.js"))));
    QVERIFY(!manager->network()->shouldBlock(
        QUrl(QLatin1String("http://other.invalid/banner.js"))));

    // rulesDownloaded() emits changed() when the fetch finishes —
    // wait on it so the next updateNow() is not dropped as "already
    // downloading".
    QSignalSpy changedSpy(subscription, SIGNAL(changed()));

    // Idempotent: re-subscribing an already-enabled location does not
    // duplicate or refetch (the presets dialog ticks it on open).
    QCOMPARE(manager->subscribeRemoteList(location,
                                          QLatin1String("Preset Test")),
             subscription);
    QCOMPARE(manager->subscriptions().count(), before + 1);
    QCOMPARE(manager->subscriptions().count(subscription), 1);
    QVERIFY(!server.waitForNewConnection(500));

    // Re-ticking a disabled preset re-enables it, still no duplicate —
    // and that path does refetch (setEnabled itself emits changed(),
    // so the fetch brings the spy to 3).
    subscription->setEnabled(false);
    manager->subscribeRemoteList(location, QLatin1String("Preset Test"));
    QVERIFY(subscription->isEnabled());
    QCOMPARE(manager->subscriptions().count(), before + 1);
    serveList();
    QTRY_VERIFY(changedSpy.count() >= 3);

    // The subscription persists into the QSettings list.
    QMetaObject::invokeMethod(manager, "save", Qt::DirectConnection);
    const QStringList stored = QSettings()
        .value(QLatin1String("AdBlock/subscriptions")).toStringList();
    bool found = false;
    for (const QString &entry : stored) {
        const QUrlQuery query(QUrl::fromEncoded(entry.toUtf8()));
        if (QUrl(query.queryItemValue(QLatin1String("location"),
                                      QUrl::FullyDecoded)) == location)
            found = true;
    }
    QVERIFY(found);

    manager->removeSubscription(subscription);
    AdBlockManager::setRemoteListsConsent(
        AdBlockManager::RemoteListsUndecided);
}

// ADB03: the presets dialog shows one checkbox per catalog entry; a
// subscription that already exists (here seeded declined-consent so
// the constructor stays offline) shows ticked, and unticking disables
// it without removing it.
void tst_AdBlockManager::presetsDialog()
{
    AdBlockManager::setRemoteListsConsent(
        AdBlockManager::RemoteListsDeclined);
    AdBlockManager *manager = AdBlockManager::instance();
    manager->load();

    const QList<AdBlockListPreset> presets = AdBlockPresets::all();
    const AdBlockListPreset preset = presets.at(0);
    const QUrl location(preset.location);
    QUrl url;
    url.setScheme(QLatin1String("abp"));
    url.setPath(QLatin1String("subscribe"));
    QUrlQuery query;
    query.addQueryItem(QLatin1String("location"),
                     QString::fromUtf8(location.toEncoded()));
    query.addQueryItem(QLatin1String("title"), preset.title);
    url.setQuery(query);
    AdBlockSubscription *subscription =
        new AdBlockSubscription(url, manager);
    manager->addSubscription(subscription);

    AdBlockPresetsDialog dialog;
    const QList<QCheckBox*> boxes = dialog.findChildren<QCheckBox*>();
    QCOMPARE(boxes.count(), presets.count());

    QCheckBox *box = nullptr;
    for (QCheckBox *candidate : boxes) {
        if (candidate->text() == preset.title)
            box = candidate;
    }
    QVERIFY(box);
    QVERIFY(box->isChecked());
    // Merely showing an already-subscribed preset as ticked must not
    // grant consent or start a fetch.
    QCOMPARE(AdBlockManager::remoteListsConsent(),
             AdBlockManager::RemoteListsDeclined);

    box->setChecked(false);
    QVERIFY(!subscription->isEnabled());
    QCOMPARE(manager->subscriptionForLocation(location), subscription);

    manager->removeSubscription(subscription);
    AdBlockManager::setRemoteListsConsent(
        AdBlockManager::RemoteListsUndecided);
}

// ADB06: the runtime engine pick persists under AdBlock/engine,
// defaults to Built-in, emits rulesChanged only on a real change, and
// drives which matcher answers.  The Rust pick is only effective in
// CONFIG+=adblock_rust builds; elsewhere it round-trips as a stored
// value while the native matcher keeps answering.
void tst_AdBlockManager::engineSelection()
{
    AdBlockManager *manager = AdBlockManager::instance();
    manager->load();
    QCOMPARE(int(AdBlockManager::storedEngine()),
             int(AdBlockManager::NativeEngine));
    QCOMPARE(int(manager->engine()), int(AdBlockManager::NativeEngine));
#if defined(ARORA_ADBLOCK_RUST)
    QVERIFY(AdBlockManager::rustEngineAvailable());
#else
    QVERIFY(!AdBlockManager::rustEngineAvailable());
#endif

    QSignalSpy spy(manager, SIGNAL(rulesChanged()));
    manager->setEngine(AdBlockManager::RustEngine);
    QCOMPARE(QSettings().value(QLatin1String("AdBlock/engine")).toString(),
             QLatin1String("rust"));
    QCOMPARE(int(AdBlockManager::storedEngine()),
             int(AdBlockManager::RustEngine));
    QCOMPARE(spy.count(), 1);

#if defined(ARORA_ADBLOCK_RUST)
    QCOMPARE(int(manager->engine()), int(AdBlockManager::RustEngine));
    // The engine only builds when there is rule text to parse — feed
    // the custom subscription a rule (emits rulesChanged itself), then
    // the pick is live in the rebuilt snapshot.
    manager->customRules()->addRule(
        AdBlockRule(QLatin1String("||engine-smoke.invalid^")));
    manager->network()->rebuildRules();
    QVERIFY(manager->network()->rustEngineActive());
    QVERIFY(manager->rustEngineInUse());
    manager->setEngine(AdBlockManager::NativeEngine);
    QVERIFY(!manager->rustEngineInUse());
    QVERIFY(!manager->network()->rustEngineActive());
#else
    // The stored pick survives but the effective engine clamps to the
    // native matcher — no dead delegation.
    QCOMPARE(int(manager->engine()), int(AdBlockManager::NativeEngine));
    QVERIFY(!manager->rustEngineInUse());
    QVERIFY(!manager->network()->rustEngineActive());
    manager->setEngine(AdBlockManager::NativeEngine);
#endif
    QCOMPARE(QSettings().value(QLatin1String("AdBlock/engine")).toString(),
             QLatin1String("cpp"));
    // No signal when the pick does not actually change.
    const int settled = spy.count();
    manager->setEngine(AdBlockManager::NativeEngine);
    QCOMPARE(spy.count(), settled);
}

QTEST_MAIN(tst_AdBlockManager)
#include "tst_adblockmanager.moc"

