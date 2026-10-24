/*
 * Copyright 2026 Arktos Browser - IT-Space
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

// MB03 probe: the rustcore namespaced event lane — the opt-in
// "domain/verb" notification channel next to the legacy
// (module, profile) callback.  AcEventNotifier bridges the Rust
// callback into a queued Qt storeChanged(topic, payload) signal.
//
// Under ARKTOS_RUSTCORE: an FFI history write through a
// facade-bound handle emits "history/changed" carrying the v1
// envelope {"v","topic","profile","op"} — observed on the Qt side
// as a QSignalSpy record — while the legacy emit keeps firing its
// flat "history" topic unchanged.  Filters scope delivery, the
// destructor unsubscribes, and ac_events_topics() reports the
// declared registry.
//
// Under CONFIG+=no-rust: the lane is compiled out — nothing to
// drive; the leg documents that.

#include "qtest_arktos.h"

#include <qbytearray.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsignalspy.h>
#include <qtemporarydir.h>

#if defined(ARKTOS_RUSTCORE)
#include "aceventnotifier.h"
#include "rustcorebridge.h"
#include <arktos_core.h>
#endif

class tst_RustEvents : public QObject
{
    Q_OBJECT

private slots:
#if defined(ARKTOS_RUSTCORE)
    void historyWriteFiresNamespacedSignal();
    void filterScopesDelivery();
    void unsubscribeAndTeardownSilence();
    void registryIsHonest();
#else
    void noRustHasNoEventLane();
#endif
};

#if defined(ARKTOS_RUSTCORE)

namespace {

// An AcCore bound to a QTemporaryDir — minted directly rather than
// through RustCoreBridge::serving() so the test never depends on
// the singleton's root-drift guard under the renamed test app.
struct TestCore {
    explicit TestCore(const QString &root)
        : root8(root.toUtf8())
    {
        AcConfig cfg;
        cfg.struct_size = sizeof(cfg);
        cfg.api_version = AC_API_VERSION;
        cfg.data_root = root8.constData();
        cfg.app_name = "tst_rustevents";
        cfg.default_profile_name = nullptr;
        handle = ac_init(&cfg);
    }
    ~TestCore() { ac_shutdown(handle); }
    QByteArray root8; // cfg.data_root borrows this
    AcCore *handle = nullptr;
};

} // namespace

void tst_RustEvents::historyWriteFiresNamespacedSignal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TestCore core(dir.path());
    QVERIFY(core.handle);

    // Legacy lane still fires its flat "history" topic alongside —
    // the namespaced lane is additive, never a re-route.
    QStringList legacyTopics;
    ac_set_change_callback(
        core.handle,
        [](const char *module, const char *, void *ud) {
            static_cast<QStringList *>(ud)->append(
                QString::fromUtf8(module));
        },
        &legacyTopics);

    AcEventNotifier notifier(core.handle, QByteArrayLiteral("history/"));
    QVERIFY(notifier.isValid());
    QSignalSpy spy(&notifier, &AcEventNotifier::storeChanged);
    QVERIFY(spy.isValid());

    AcHistory *h = ac_history(core.handle, "mbev");
    QVERIFY(h);
    QCOMPARE(ac_hist_add(h, "https://ev.example/", "EV", 1000), AC_OK);
    ac_hist_close(h);

    // The emit is synchronous Rust-side; delivery crosses threads
    // queued — pump until the signal lands.
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QLatin1String("history/changed"));
    const QByteArray payload = args.at(1).toByteArray();
    const QJsonObject doc =
        QJsonDocument::fromJson(payload).object();
    QCOMPARE(doc.value(QLatin1String("v")).toInt(), 1);
    QCOMPARE(doc.value(QLatin1String("topic")).toString(),
             QLatin1String("history/changed"));
    QCOMPARE(doc.value(QLatin1String("profile")).toString(),
             QLatin1String("mbev"));
    QCOMPARE(doc.value(QLatin1String("op")).toString(),
             QLatin1String("visit"));

    // The flat "history" topic still fired alongside — the legacy
    // lane is additive, never re-routed.  The test binary runs a
    // full BrowserApplication whose own stores may emit unrelated
    // topics into every registered core callback, so assert
    // presence rather than exact list equality.
    QVERIFY2(legacyTopics.contains(QStringLiteral("history")),
             qPrintable(QStringLiteral("legacyTopics=%1")
                            .arg(legacyTopics.join(QLatin1Char(',')))));
    ac_set_change_callback(core.handle, nullptr, nullptr);
}

void tst_RustEvents::filterScopesDelivery()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TestCore core(dir.path());
    QVERIFY(core.handle);

    // A namespace subscriber and an unrelated exact-topic
    // subscriber over the same core — only the matching filter
    // delivers.
    AcEventNotifier hist(core.handle, QByteArrayLiteral("history/"));
    AcEventNotifier settings(core.handle,
                             QByteArrayLiteral("settings/changed"));
    AcEventNotifier all(core.handle);
    QVERIFY(hist.isValid());
    QVERIFY(settings.isValid());
    QVERIFY(all.isValid());
    QSignalSpy histSpy(&hist, &AcEventNotifier::storeChanged);
    QSignalSpy settingsSpy(&settings, &AcEventNotifier::storeChanged);
    QSignalSpy allSpy(&all, &AcEventNotifier::storeChanged);

    AcHistory *h = ac_history(core.handle, nullptr);
    QVERIFY(h);
    QCOMPARE(ac_hist_add(h, "https://scope.example/", "S", 2000),
             AC_OK);
    ac_hist_close(h);

    QTRY_VERIFY_WITH_TIMEOUT(histSpy.count() >= 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(allSpy.count() >= 1, 5000);
    QTest::qWait(100);
    QCOMPARE(settingsSpy.count(), 0);

    // A NULL profile id binds the facade's effective "default"
    // profile (BrowserPaths::profileDataDir semantics) — the
    // envelope stamps "default"; null is only for mutations on the
    // unscoped legacy rc_* lane.
    const QList<QVariant> args = histSpy.takeFirst();
    const QJsonObject doc =
        QJsonDocument::fromJson(args.at(1).toByteArray()).object();
    QCOMPARE(doc.value(QLatin1String("profile")).toString(),
             QLatin1String("default"));
}

void tst_RustEvents::unsubscribeAndTeardownSilence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TestCore core(dir.path());
    QVERIFY(core.handle);

    quint64 id = 0;
    {
        AcEventNotifier notifier(core.handle);
        QVERIFY(notifier.isValid());
        id = notifier.subscriptionId();
        QVERIFY(id != 0);
        // Destruction unsubscribes — a history write afterwards must
        // not touch the dead notifier (the leaked guard's dead flag
        // drops any in-flight delivery).
    }

    AcHistory *h = ac_history(core.handle, nullptr);
    QVERIFY(h);
    QCOMPARE(ac_hist_add(h, "https://dead.example/", "D", 3000),
             AC_OK);
    ac_hist_close(h);
    QTest::qWait(100); // let any (illegal) late delivery surface

    // The subscription id is gone from the registry — the dtor ran
    // ac_events_unsubscribe.
    QCOMPARE(ac_events_unsubscribe(core.handle, id), AC_ERR_NOT_FOUND);
}

void tst_RustEvents::registryIsHonest()
{
    char *raw = ac_events_topics();
    QVERIFY(raw);
    const QByteArray text = QByteArray(raw);
    ac_string_free(raw);
    const QJsonArray rows = QJsonDocument::fromJson(text).array();
    QVERIFY(!rows.isEmpty());

    QJsonObject hist;
    for (const QJsonValue &row : rows) {
        const QJsonObject o = row.toObject();
        QVERIFY(o.contains(QLatin1String("summary")));
        QVERIFY(o.value(QLatin1String("fields")).isArray());
        if (o.value(QLatin1String("topic")).toString()
            == QLatin1String("history/changed"))
            hist = o;
    }
    QVERIFY(!hist.isEmpty());
    // The produced flag is the honesty bit — history/changed is the
    // one topic an in-tree emit site fires today.
    QCOMPARE(hist.value(QLatin1String("produced")).toBool(), true);
    QVERIFY(hist.contains(QLatin1String("seen")));
}

#else

void tst_RustEvents::noRustHasNoEventLane()
{
    QVERIFY2(true,
             "rustcore compiled out — the namespaced event lane is "
             "part of the crate");
}

#endif

QTEST_MAIN(tst_RustEvents)
#include "tst_rustevents.moc"
