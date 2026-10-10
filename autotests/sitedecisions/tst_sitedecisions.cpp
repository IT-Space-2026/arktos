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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// SITED01: the consolidated per-site decision store.
//
// Under ARORA_RUSTCORE this exercises the SiteDecisionStore façade
// end to end — round-trips, whole-kind replace, the suffix lookup,
// the IO-thread snapshot, corrupt-row tolerance and, above all, the
// one-shot migration of every legacy QSettings/INI store into the
// Rust store.
//
// Under CONFIG+=no-rust it instead asserts the legacy QSettings paths
// still carry the decisions — the fallback contract the flag exists
// for.

#include <QtTest/QtTest>

#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsettings.h>
#include <qstandardpaths.h>
#include <qurl.h>
#include <qwebengineprofile.h>

#include "browserpaths.h"
#include "containermanager.h"
#include "cookiejar.h"
#include "popupblocker.h"
#include "privacyrequestinterceptor.h"
#include "scriptcontrolmanager.h"
#include "webpermissionmanager.h"
#include "adblockmanager.h"
#include "adblockrule.h"
#include "adblocksubscription.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

class tst_SiteDecisions : public QObject
{
    Q_OBJECT

public:
    tst_SiteDecisions();
    ~tst_SiteDecisions();

private slots:
    void initTestCase();
    void init();
    void cleanup();

#if defined(ARORA_RUSTCORE)
    void storeRoundTrip();
    void replaceAndLookup();
    void snapshotReflectsChange();
    void corruptFileTolerance();
    void legacyMigration();
    void persistenceAcrossReload();
#else
    void noRustKeepsQSettings();
#endif
};

tst_SiteDecisions::tst_SiteDecisions()
{
}

tst_SiteDecisions::~tst_SiteDecisions()
{
}

void tst_SiteDecisions::initTestCase()
{
    QCoreApplication::setApplicationName("tst_sitedecisions");
    QStandardPaths::setTestModeEnabled(true);
}

void tst_SiteDecisions::init()
{
    // Every test starts from an empty store AND an empty legacy
    // profile — wipe both before constructing anything, so a manager
    // that hydrates in its constructor sees a clean slate.
    QSettings settings;
    settings.remove(QLatin1String("webpermissions"));
    settings.remove(QLatin1String("scriptcontrol"));
    settings.remove(QLatin1String("popupExceptions"));
    settings.remove(QLatin1String("containers"));
    settings.remove(QLatin1String("cookies"));
    settings.remove(QLatin1String("privacy"));
    settings.remove(QLatin1String("AdBlock"));
#if defined(ARORA_RUSTCORE)
    SiteDecisionStore::reset();
#endif
}

void tst_SiteDecisions::cleanup()
{
}

#if defined(ARORA_RUSTCORE)

void tst_SiteDecisions::storeRoundTrip()
{
    QVERIFY(!SiteDecisionStore::storePresent());

    QString value;
    QVERIFY(!SiteDecisionStore::get(SiteDecisionStore::KindPopup,
                                    QLatin1String("pop.example"), &value));
    QVERIFY(SiteDecisionStore::set(SiteDecisionStore::KindPopup,
                                   QLatin1String("pop.example"),
                                   QLatin1String("allow")));
    QVERIFY(SiteDecisionStore::storePresent());
    QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindPopup,
                                   QLatin1String("pop.example"), &value));
    QCOMPARE(value, QLatin1String("allow"));

    const QHash<QString, QString> rows =
        SiteDecisionStore::entries(SiteDecisionStore::KindPopup);
    QCOMPARE(rows.value(QLatin1String("pop.example")),
             QLatin1String("allow"));

    QVERIFY(SiteDecisionStore::remove(SiteDecisionStore::KindPopup,
                                      QLatin1String("pop.example")));
    QVERIFY(!SiteDecisionStore::get(SiteDecisionStore::KindPopup,
                                    QLatin1String("pop.example"), &value));
    // Idempotent remove.
    QVERIFY(SiteDecisionStore::remove(SiteDecisionStore::KindPopup,
                                      QLatin1String("pop.example")));

    QVERIFY(SiteDecisionStore::set(SiteDecisionStore::KindJavaScript,
                                   QLatin1String("js.example"),
                                   QLatin1String("block")));
    QVERIFY(SiteDecisionStore::clear(SiteDecisionStore::KindJavaScript));
    QVERIFY(SiteDecisionStore::entries(SiteDecisionStore::KindJavaScript)
                .isEmpty());
}

void tst_SiteDecisions::replaceAndLookup()
{
    QHash<QString, QString> rules;
    rules.insert(QLatin1String("example.com"), QLatin1String("allow"));
    rules.insert(QLatin1String("sub.other.org"), QLatin1String("block"));
    QVERIFY(SiteDecisionStore::replace(SiteDecisionStore::KindCookie, rules));
    QCOMPARE(SiteDecisionStore::entries(SiteDecisionStore::KindCookie),
             rules);

    // The suffix walk: a rule on example.com governs www.example.com.
    QString value, matched;
    QVERIFY(SiteDecisionStore::lookup(SiteDecisionStore::KindCookie,
                                      QLatin1String("www.example.com"),
                                      &value, &matched));
    QCOMPARE(value, QLatin1String("allow"));
    QCOMPARE(matched, QLatin1String("example.com"));
    QVERIFY(!SiteDecisionStore::lookup(SiteDecisionStore::KindCookie,
                                       QLatin1String("notexample.com"),
                                       &value));

    // Replace shrinks the kind wholesale.
    QHash<QString, QString> fewer;
    fewer.insert(QLatin1String("sub.other.org"), QLatin1String("block"));
    QVERIFY(SiteDecisionStore::replace(SiteDecisionStore::KindCookie, fewer));
    QVERIFY(!SiteDecisionStore::lookup(SiteDecisionStore::KindCookie,
                                       QLatin1String("www.example.com"),
                                       &value));
}

void tst_SiteDecisions::snapshotReflectsChange()
{
    QJsonDocument doc =
        QJsonDocument::fromJson(SiteDecisionStore::snapshot());
    QVERIFY(doc.object().value(QLatin1String("kinds")).isObject());
    QVERIFY(doc.object()
                .value(QLatin1String("kinds")).toObject()
                .value(QLatin1String("popup")).isUndefined());

    // A change is visible to a fresh snapshot without any restart —
    // the IO-thread contract.
    SiteDecisionStore::set(SiteDecisionStore::KindPopup,
                           QLatin1String("now.example"),
                           QLatin1String("allow"));
    doc = QJsonDocument::fromJson(SiteDecisionStore::snapshot());
    const QJsonObject kinds =
        doc.object().value(QLatin1String("kinds")).toObject();
    QCOMPARE(kinds.value(QLatin1String("popup")).toObject()
                 .value(QLatin1String("now.example")).toString(),
             QLatin1String("allow"));
}

void tst_SiteDecisions::corruptFileTolerance()
{
    // A file with a mix of valid and malformed rows keeps the valid
    // ones; a wholly unparsable file degrades to empty — never to an
    // error wedging every per-site check behind it.
    const QString path =
        BrowserPaths::dataFilePath(QLatin1String("sitedecisions.json"));
    QFile file(path);
    QVERIFY(file.open(QFile::WriteOnly));
    file.write("{\"version\":1,\"kinds\":{"
               "\"js\":{\"ok.example\":\"allow\",\"bad\":7,\"\":\"x\"},"
               "\"oops\":42,"
               "\"popup\":{\"pop.example\":\"allow\"}}}");
    file.close();
    QVERIFY(SiteDecisionStore::reload());

    QString value;
    QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindJavaScript,
                                   QLatin1String("ok.example"), &value));
    QCOMPARE(value, QLatin1String("allow"));
    QVERIFY(!SiteDecisionStore::get(SiteDecisionStore::KindJavaScript,
                                    QLatin1String("bad"), &value));
    QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindPopup,
                                   QLatin1String("pop.example"), &value));

    QVERIFY(file.open(QFile::WriteOnly));
    file.write("{ this is not json");
    file.close();
    QVERIFY(SiteDecisionStore::reload());
    QVERIFY(!SiteDecisionStore::get(SiteDecisionStore::KindPopup,
                                    QLatin1String("pop.example"), &value));

    // The store heals itself on the next write.
    SiteDecisionStore::set(SiteDecisionStore::KindPopup,
                           QLatin1String("back.example"),
                           QLatin1String("allow"));
    QVERIFY(SiteDecisionStore::reload());
    QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindPopup,
                                   QLatin1String("back.example"), &value));
}

void tst_SiteDecisions::legacyMigration()
{
    // Seed every legacy per-site store the way a pre-SITED01 profile
    // would leave it, then touch each manager — every grant must land
    // in the Rust store and the legacy key must be retired.
    QSettings settings;

    // Web permissions: "<enc-origin>/<Type>" rows under a group.
    const QUrl origin(QStringLiteral("https://perm.example:8443"));
    QUrl normalized = origin;
    normalized.setUserInfo(QString());
    normalized.setPath(QString());
    normalized.setQuery(QString());
    normalized.setFragment(QString());
    const QString originKey = QString::fromUtf8(
        normalized.toEncoded().toPercentEncoding());
    settings.setValue(QLatin1String("webpermissions/")
                      + originKey + QLatin1String("/Notifications"),
                      QLatin1String("grant"));
    settings.setValue(QLatin1String("webpermissions/")
                      + originKey + QLatin1String("/Geolocation"),
                      QLatin1String("deny"));

    // Script control: the two host lists.
    settings.setValue(QLatin1String("scriptcontrol/allowed"),
                      QStringList() << QLatin1String("js-ok.example"));
    settings.setValue(QLatin1String("scriptcontrol/blocked"),
                      QStringList() << QLatin1String("js-bad.example"));

    // Pop-up exceptions.
    settings.setValue(QLatin1String("popupExceptions/allowed"),
                      QStringList() << QLatin1String("pop.example"));

    // Container site rules: inside the container's own group.
    const QString containerId = QLatin1String("c0ffee01c0ffee01c0ffee01c0ffee01");
    settings.setValue(QLatin1String("containers/order"),
                      QStringList() << containerId);
    settings.setValue(QLatin1String("containers/") + containerId
                      + QLatin1String("/name"), QLatin1String("Work"));
    settings.setValue(QLatin1String("containers/") + containerId
                      + QLatin1String("/sites"),
                      QStringList() << QLatin1String("WWW.Contained.Example"));

    // Cookie exceptions: three lists, three rule kinds.
    settings.setValue(QLatin1String("cookies/exceptions/block"),
                      QStringList() << QLatin1String("bad.example"));
    settings.setValue(QLatin1String("cookies/exceptions/allow"),
                      QStringList() << QLatin1String("good.example"));
    settings.setValue(QLatin1String("cookies/exceptions/allowForSession"),
                      QStringList() << QLatin1String("sess.example"));

    // HTTPS-only allowances.
    settings.setValue(QLatin1String("privacy/httpsOnlyExceptions"),
                      QStringList() << QLatin1String("insecure.example"));

    // Adblock site whitelist: a canonical document exception in the
    // custom-rules file, plus the subscription entry that loads it.
    const QString customFile = BrowserPaths::dataFilePath(
        QLatin1String("adblock_subscription_custom"));
    {
        QFile file(customFile);
        QVERIFY(file.open(QFile::WriteOnly));
        file.write("[Adblock Plus 2.0]\n"
                   "@@||whitelisted.example^$document\n"
                   "! a comment stays put\n");
        file.close();
    }
    const QUrl customLocation = QUrl::fromLocalFile(customFile);
    const QString customSub = QLatin1String(
        "abp:subscribe?location=%1&title=Custom%20Rules")
        .arg(QString::fromUtf8(customLocation.toEncoded()));
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
                      QStringList() << customSub);
    settings.sync();

    // -- the migrations ------------------------------------------------

    // WebPermissionManager migrates on construction.
    {
        WebPermissionManager manager;
        const QList<WebPermissionManager::Entry> entries = manager.entries();
        QCOMPARE(entries.count(), 2);
        int granted = 0, denied = 0;
        for (const WebPermissionManager::Entry &entry : entries) {
            if (entry.origin == origin
                && entry.type
                       == QWebEnginePermission::PermissionType::Notifications
                && entry.granted)
                ++granted;
            if (entry.origin == origin
                && entry.type
                       == QWebEnginePermission::PermissionType::Geolocation
                && !entry.granted)
                ++denied;
        }
        QCOMPARE(granted, 1);
        QCOMPARE(denied, 1);
    }
    QVERIFY(!settings.contains(QLatin1String("webpermissions")));

    // ScriptControlManager migrates + hydrates on construction.
    {
        ScriptControlManager scripts;
        QCOMPARE(scripts.ruleForHost(QLatin1String("js-ok.example")),
                 ScriptControlManager::Allow);
        QCOMPARE(scripts.ruleForHost(QLatin1String("www.js-ok.example")),
                 ScriptControlManager::Allow);
        QCOMPARE(scripts.ruleForHost(QLatin1String("js-bad.example")),
                 ScriptControlManager::Block);
    }
    QVERIFY(!settings.contains(QLatin1String("scriptcontrol")));

    // PopupBlocker.
    {
        PopupBlocker blocker;
        QVERIFY(blocker.isAllowedHost(QLatin1String("pop.example")));
        QVERIFY(blocker.isAllowedHost(QLatin1String("www.pop.example")));
    }
    QVERIFY(!settings.contains(QLatin1String("popupExceptions/allowed")));

    // ContainerManager: the site rule lands keyed to its container.
    {
        ContainerManager containers;
        QCOMPARE(containers.containerIdForHost(
                     QLatin1String("contained.example")), containerId);
        QCOMPARE(containers.containerIdForHost(
                     QLatin1String("deep.sub.contained.example")),
                 containerId);
        QCOMPARE(containers.siteRules(containerId),
                 QStringList() << QLatin1String("contained.example"));
    }
    QVERIFY(!settings.contains(QLatin1String("containers/")
                               + containerId + QLatin1String("/sites")));
    // Name survived — the registry itself was never touched.
    QCOMPARE(settings.value(QLatin1String("containers/") + containerId
                            + QLatin1String("/name")).toString(),
             QLatin1String("Work"));

    // CookieJar: the three lists hydrate from the store.
    {
        CookieJar jar;
        CookieJar::CookieRule rule = CookieJar::Allow;
        QVERIFY(jar.ruleForHost(QLatin1String("bad.example"), &rule));
        QCOMPARE(rule, CookieJar::Block);
        QVERIFY(jar.ruleForHost(QLatin1String("good.example"), &rule));
        QCOMPARE(rule, CookieJar::Allow);
        QVERIFY(jar.ruleForHost(QLatin1String("sess.example"), &rule));
        QCOMPARE(rule, CookieJar::AllowForSession);
    }
    QVERIFY(!settings.contains(QLatin1String("cookies/exceptions/block")));
    QVERIFY(!settings.contains(QLatin1String("cookies/exceptions/allow")));

    // HTTPS-only: loadSettings re-reads the persisted snapshot.
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(PrivacyRequestInterceptor::isHttpAllowedHost(
        QLatin1String("insecure.example")));
    QVERIFY(!settings.contains(QLatin1String("privacy/httpsOnlyExceptions")));

    // AdBlockManager: the custom file's document exception moves into
    // the store; a non-matching line stays put.
    AdBlockManager *adblock = AdBlockManager::instance();
    QVERIFY(adblock->isSiteWhitelisted(QLatin1String("whitelisted.example")));
    {
        const QList<AdBlockRule> rules = adblock->customRules()->allRules();
        for (const AdBlockRule &rule : rules) {
            QVERIFY(rule.filter()
                    != QLatin1String("@@||whitelisted.example^$document"));
        }
    }
    QString stored;
    QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindAdBlock,
                                   QLatin1String("whitelisted.example"),
                                   &stored));
    QCOMPARE(stored, QLatin1String("allow"));

    // Idempotent: a second manager round sees no legacy residue and
    // still returns every migrated decision.
    {
        PopupBlocker blocker;
        QVERIFY(blocker.isAllowedHost(QLatin1String("pop.example")));
    }
    QVERIFY(!settings.contains(QLatin1String("popupExceptions")));
}

void tst_SiteDecisions::persistenceAcrossReload()
{
    SiteDecisionStore::set(SiteDecisionStore::KindContainer,
                           QLatin1String("persist.example"),
                           QLatin1String("some-id"));
    // reload() re-reads the file — a write survives it.
    QVERIFY(SiteDecisionStore::reload());
    QString value;
    QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindContainer,
                                   QLatin1String("persist.example"), &value));
    QCOMPARE(value, QLatin1String("some-id"));
    // And the file itself is what a restart would see.
    const QString path =
        BrowserPaths::dataFilePath(QLatin1String("sitedecisions.json"));
    QVERIFY(QFile::exists(path));
    QFile file(path);
    QVERIFY(file.open(QFile::ReadOnly));
    const QJsonObject doc =
        QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(doc.value(QLatin1String("kinds")).toObject()
                 .value(QLatin1String("container")).toObject()
                 .value(QLatin1String("persist.example")).toString(),
             QLatin1String("some-id"));
}

#else // !ARORA_RUSTCORE — the fallback contract

void tst_SiteDecisions::noRustKeepsQSettings()
{
    // The no-rust build must keep writing the same QSettings keys the
    // pre-SITED01 code did — the migration path depends on them still
    // being readable.
    PopupBlocker blocker;
    blocker.allowHost(QLatin1String("legacy.example"));
    QVERIFY(QSettings()
                .value(QLatin1String("popupExceptions/allowed"))
                .toStringList()
                .contains(QLatin1String("legacy.example")));
    blocker.removeAllowedHost(QLatin1String("legacy.example"));
    QVERIFY(!QSettings()
                 .value(QLatin1String("popupExceptions/allowed"))
                 .toStringList()
                 .contains(QLatin1String("legacy.example")));

    ScriptControlManager scripts;
    scripts.setRuleForHost(QLatin1String("legacy-js.example"),
                           ScriptControlManager::Block, true);
    QVERIFY(QSettings()
                .value(QLatin1String("scriptcontrol/blocked"))
                .toStringList()
                .contains(QLatin1String("legacy-js.example")));
    scripts.clearPersistentRules();
}

#endif

QTEST_MAIN(tst_SiteDecisions)
#include "tst_sitedecisions.moc"
