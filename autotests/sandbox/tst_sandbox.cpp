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

#include "bwrapgenerator.h"
#include "platformgenerators.h"
#include "sandboxmanager.h"
#include "sandboxpolicy.h"

// SAND01: unit tests for the declarative policy and every backend
// generator.  All generators are pure — they are verified here on
// Linux even though their apply paths run on other platforms.  The
// end-to-end wrap/fallback checks live in --sandbox-smoke.
class tst_Sandbox : public QObject
{
    Q_OBJECT

private slots:
    void denylistContents();
    void defaultPolicyBucketing();
    void policyDropsMissingPaths();
    void bwrapArgv();
    void bwrapArgvOrdering();
    void bwrapLauncherScript();
    void bwrapLauncherMatchesCommittedFile();
    void seatbeltProfile();
    void appContainerManifest();
    void openBsdPlan();
    void managerStatus();
    // SAND02: the restrictive --download-worker wrap generators.
    void bwrapDownloadWorkerArgv();
    void bwrapDownloadWorkerRemapsHome();
    void seatbeltDownloadWorkerProfile();
    void appContainerDownloadWorkerManifest();
    void openBsdDownloadWorkerPlan();
};

void tst_Sandbox::denylistContents()
{
    const QStringList denied = SandboxPolicy::homeRelativeDeniedPaths();
    QVERIFY(denied.size() >= 10);
    // Entries are $HOME-relative — backends expand them per-platform.
    for (const QString &entry : denied) {
        QVERIFY(!entry.startsWith(QLatin1Char('/')));
        QVERIFY(!entry.contains(QLatin1Char('\n')));
    }
    // The credential/browser-profile anchors must be present.
    QVERIFY(denied.contains(QLatin1String(".ssh")));
    QVERIFY(denied.contains(QLatin1String(".gnupg")));
    QVERIFY(denied.contains(QLatin1String(".mozilla")));
    QVERIFY(denied.contains(QLatin1String(".config/google-chrome")));
    // Arora's own config/data dirs must never be denied — that would
    // hide the profile the browser writes.
    for (const QString &entry : denied) {
        QVERIFY2(!entry.contains(QLatin1String("Arora"),
                                 Qt::CaseInsensitive),
                 qPrintable(entry));
    }
}

void tst_Sandbox::defaultPolicyBucketing()
{
    const SandboxPolicy policy = SandboxPolicy::defaultPolicy();
    QCOMPARE(policy.homeDir, QDir::homePath());

    // Every surviving denylist entry must exist on the host and be
    // bucketed by kind — directories mask with tmpfs, everything else
    // with a /dev/null bind.
    for (const QString &dir : policy.deniedDirs) {
        QVERIFY2(QFileInfo(dir).isDir(), qPrintable(dir));
        QVERIFY2(dir.startsWith(policy.homeDir), qPrintable(dir));
    }
    for (const QString &file : policy.deniedFiles) {
        const QFileInfo info(file);
        QVERIFY2(info.exists() || info.isSymLink(), qPrintable(file));
        QVERIFY2(!info.isDir(), qPrintable(file));
    }
    for (const QString &root : policy.readOnlyRoots) {
        QVERIFY2(QFileInfo(root).isDir(), qPrintable(root));
        QVERIFY2(root.startsWith(QLatin1Char('/')), qPrintable(root));
    }
}

void tst_Sandbox::policyDropsMissingPaths()
{
    // A denylist entry that does not exist must not reach the policy:
    // bwrap creates missing mount points on the real filesystem, so
    // masking an absent path would actively create it.
    const QStringList denied = SandboxPolicy::homeRelativeDeniedPaths();
    const QStringList all = SandboxPolicy::defaultPolicy().deniedDirs
        + SandboxPolicy::defaultPolicy().deniedFiles;
    for (const QString &relative : denied) {
        const QString absolute =
            QDir::homePath() + QLatin1Char('/') + relative;
        if (!QFileInfo::exists(absolute))
            QVERIFY2(!all.contains(absolute), qPrintable(absolute));
    }
}

void tst_Sandbox::bwrapArgv()
{
    SandboxPolicy policy;
    policy.homeDir = QStringLiteral("/home/tester");
    policy.deniedDirs << QStringLiteral("/home/tester/.ssh");
    policy.deniedFiles << QStringLiteral("/home/tester/.netrc");
    policy.readOnlyRoots << QStringLiteral("/etc")
                         << QStringLiteral("/usr");

    const QStringList argv = BwrapGenerator::commandLine(
        policy, QStringLiteral("/usr/bin/arora"),
        {QStringLiteral("--quit-after-load")});

    QVERIFY(argv.contains(QStringLiteral("--die-with-parent")));
    // The marker env var keeps the wrapped child from re-wrapping.
    const int setenv = argv.indexOf(QStringLiteral("--setenv"));
    QVERIFY(setenv != -1);
    QCOMPARE(argv.at(setenv + 1), QStringLiteral("ARORA_SANDBOXED"));
    QCOMPARE(argv.at(setenv + 2), QStringLiteral("bwrap"));
    // Permissive base: the whole tree is dev-bound.
    const int devbind = argv.indexOf(QStringLiteral("--dev-bind"));
    QVERIFY(devbind != -1);
    QCOMPARE(argv.at(devbind + 1), QStringLiteral("/"));
    QCOMPARE(argv.at(devbind + 2), QStringLiteral("/"));
    // Directory masks are tmpfs; file masks are /dev/null binds.
    const int tmpfs = argv.indexOf(QStringLiteral("--tmpfs"));
    QVERIFY(tmpfs != -1);
    QCOMPARE(argv.at(tmpfs + 1), QStringLiteral("/home/tester/.ssh"));
    QVERIFY(argv.contains(QStringLiteral("/dev/null")));
    QVERIFY(argv.contains(QStringLiteral("/home/tester/.netrc")));
    // Read-only roots are ro-bind pairs.
    const int ro = argv.indexOf(QStringLiteral("--ro-bind"));
    QVERIFY(ro != -1);
    // No network/pid namespace teardown — browsing needs them shared.
    QVERIFY(!argv.contains(QStringLiteral("--unshare-net")));
    QVERIFY(!argv.contains(QStringLiteral("--unshare-all")));
}

void tst_Sandbox::bwrapArgvOrdering()
{
    SandboxPolicy policy;
    policy.homeDir = QStringLiteral("/home/tester");
    policy.deniedDirs << QStringLiteral("/home/tester/.ssh");
    const QStringList argv = BwrapGenerator::commandLine(
        policy, QStringLiteral("/opt/arora/bin/arora"),
        {QStringLiteral("https://example.com")});

    // `--` separates bwrap's options from the wrapped command, and the
    // program + its args are the tail.
    const int sep = argv.indexOf(QStringLiteral("--"));
    QVERIFY(sep != -1);
    QCOMPARE(argv.at(sep + 1), QStringLiteral("/opt/arora/bin/arora"));
    QCOMPARE(argv.at(sep + 2), QStringLiteral("https://example.com"));
    QCOMPARE(argv.size(), sep + 3);
    // The sandbox marker must land before the separator.
    QVERIFY(argv.indexOf(QStringLiteral("--setenv")) < sep);
    QVERIFY(argv.indexOf(QStringLiteral("--tmpfs")) < sep);
}

void tst_Sandbox::bwrapLauncherScript()
{
    const QString script = BwrapGenerator::launcherScript();
    QVERIFY(script.startsWith(QLatin1String("#!/bin/sh\n")));
    QVERIFY(script.contains(QStringLiteral("ARORA_SANDBOXED")));
    QVERIFY(script.contains(QStringLiteral("--tmpfs")));
    QVERIFY(script.contains(QStringLiteral("--dev-bind / /"))
            || script.contains(QStringLiteral("--dev-bind")));
    QVERIFY(script.contains(QStringLiteral("ARORA_NO_SANDBOX")));
    QVERIFY(script.contains(QStringLiteral("ARORA_BWRAP")));
    // Every denylist entry is baked in, $HOME-relative.
    for (const QString &entry : SandboxPolicy::homeRelativeDeniedPaths())
        QVERIFY2(script.contains(entry), qPrintable(entry));

    // The script must parse under any POSIX sh on the host.
    QProcess sh;
    sh.start(QStringLiteral("sh"),
             {QStringLiteral("-n")});
    QVERIFY(sh.waitForStarted());
    sh.write(script.toUtf8());
    sh.closeWriteChannel();
    QVERIFY(sh.waitForFinished(10000));
    QCOMPARE(sh.exitCode(), 0);
}

void tst_Sandbox::bwrapLauncherMatchesCommittedFile()
{
    // The installed arora-sandbox script must be byte-identical to the
    // generator output — regenerate it with --write-sandbox-launcher
    // instead of editing by hand.
    QFile committed(QStringLiteral("../../src/sandbox/arora-sandbox"));
    QVERIFY(committed.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(committed.readAll()),
             BwrapGenerator::launcherScript());
}

void tst_Sandbox::seatbeltProfile()
{
    SandboxPolicy policy;
    policy.homeDir = QStringLiteral("/Users/tester");
    policy.deniedDirs << QStringLiteral("/Users/tester/.ssh");
    policy.deniedFiles << QStringLiteral("/Users/tester/.netrc");

    const QString profile = SeatbeltGenerator::profile(policy);
    QVERIFY(profile.contains(QLatin1String("(version 1)")));
    QVERIFY(profile.contains(QLatin1String("(allow default)")));
    // Directories mask the whole subtree; files mask the literal path.
    QVERIFY(profile.contains(QLatin1String(
        "(deny file-read* file-write* (subpath \"/Users/tester/.ssh\"))")));
    QVERIFY(profile.contains(QLatin1String(
        "(deny file-read* file-write* (literal \"/Users/tester/.netrc\"))")));
}

void tst_Sandbox::appContainerManifest()
{
    SandboxPolicy policy;
    policy.homeDir = QStringLiteral("C:\\Users\\tester");
    policy.deniedDirs << QStringLiteral("C:\\Users\\tester\\.ssh");
    policy.readOnlyRoots << QStringLiteral("C:\\Windows");

    const QString xml = AppContainerGenerator::manifestXml(policy);
    QXmlStreamReader reader(xml);
    bool sawInternet = false;
    bool sawDeny = false;
    bool sawReadOnly = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        const auto attrs = reader.attributes();
        if (reader.name() == QLatin1String("capability")
            && attrs.value(QLatin1String("name"))
                == QLatin1String("internetClient"))
            sawInternet = true;
        if (reader.name() == QLatin1String("rule")
            && attrs.value(QLatin1String("access"))
                == QLatin1String("deny")
            && attrs.value(QLatin1String("path"))
                == QLatin1String("C:\\Users\\tester\\.ssh"))
            sawDeny = true;
        if (reader.name() == QLatin1String("rule")
            && attrs.value(QLatin1String("access"))
                == QLatin1String("read")
            && attrs.value(QLatin1String("path"))
                == QLatin1String("C:\\Windows"))
            sawReadOnly = true;
    }
    QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    QVERIFY(sawInternet);
    QVERIFY(sawDeny);
    QVERIFY(sawReadOnly);
}

void tst_Sandbox::openBsdPlan()
{
    SandboxPolicy policy;
    policy.homeDir = QStringLiteral("/home/tester");
    policy.deniedDirs << QStringLiteral("/home/tester/.ssh");
    policy.deniedFiles << QStringLiteral("/home/tester/.netrc");
    policy.readOnlyRoots << QStringLiteral("/etc");

    const OpenBsdPlan plan = OpenBsdGenerator::plan(policy);
    QVERIFY(!plan.unveilRules.isEmpty());
    // The broad permissive grant comes first; the denies and the
    // read-only narrowing come after it (last unveil wins).
    QCOMPARE(plan.unveilRules.first(), QStringLiteral("/|rwc"));
    QVERIFY(plan.unveilRules.contains(QStringLiteral("/etc|r")));
    QVERIFY(plan.unveilRules.contains(
        QStringLiteral("/home/tester/.ssh|")));
    QVERIFY(plan.unveilRules.contains(
        QStringLiteral("/home/tester/.netrc|")));
    const int rootIndex =
        plan.unveilRules.indexOf(QStringLiteral("/|rwc"));
    QVERIFY(plan.unveilRules.indexOf(
                QStringLiteral("/home/tester/.ssh|")) > rootIndex);
    // A browser needs nearly every promise class — documented as
    // near-vacuous hardening, but it must still be non-empty.
    QVERIFY(plan.pledgePromises.contains(QLatin1String("inet")));
    QVERIFY(plan.pledgePromises.contains(QLatin1String("stdio")));
}

void tst_Sandbox::managerStatus()
{
    // The test itself runs unwrapped.
    QVERIFY(!SandboxManager::isSandboxed());
#if defined(Q_OS_LINUX)
    QCOMPARE(SandboxManager::backendName(), QStringLiteral("bwrap"));
#endif
    const QString report = SandboxManager::statusReport();
    QVERIFY(report.contains(QLatin1String("backend:")));
    QVERIFY(report.contains(QLatin1String("sandboxed: no")));
    QVERIFY(report.contains(QLatin1String("masked directories")));
    QVERIFY(report.contains(QLatin1String("read-only roots")));
}

// ---- SAND02: the restrictive --download-worker wrap ----------------

void tst_Sandbox::bwrapDownloadWorkerArgv()
{
    const QStringList argv = BwrapGenerator::downloadWorkerCommandLine(
        QStringLiteral("/opt/arora/arora"),
        {QStringLiteral("--download-worker")},
        QStringLiteral("/home/t/.local/share/Arora/downloads-parts/w-abc"),
        QStringLiteral("/home/t/Downloads"));

    QCOMPARE(argv.first(), QStringLiteral("bwrap"));
    QVERIFY(argv.contains(QLatin1String("--clearenv")));
    QVERIFY(argv.contains(QLatin1String("--unshare-pid")));
    QVERIFY(argv.contains(QLatin1String("--die-with-parent")));
    // The writable surface: the per-download work dir and the
    // destination dir — and nothing else.
    const int binds = argv.count(QLatin1String("--bind"));
    QVERIFY(binds >= 2);
    const int wi = argv.indexOf(QStringLiteral(
        "/home/t/.local/share/Arora/downloads-parts/w-abc"));
    QVERIFY(wi > 0 && argv.at(wi - 1) == QLatin1String("--bind"));
    const int di = argv.indexOf(QStringLiteral("/home/t/Downloads"));
    QVERIFY(di > 0 && argv.at(di - 1) == QLatin1String("--bind"));
    // $HOME must NOT appear anywhere — nothing under it is mounted.
    QVERIFY(!argv.contains(QStringLiteral("/home/t")));
    // The worker binary is a read-only mount and the payload follows
    // `--` verbatim.
    const int dd = argv.indexOf(QStringLiteral("--"));
    QVERIFY(dd > 0);
    QCOMPARE(argv.at(dd + 1), QStringLiteral("/opt/arora/arora"));
    QCOMPARE(argv.at(dd + 2), QStringLiteral("--download-worker"));
    // The wrap marker children see.
    const int marker = argv.indexOf(QStringLiteral("ARORA_SANDBOXED"));
    QVERIFY(marker > 0
            && argv.at(marker + 1) == QLatin1String("bwrap-dl"));
    // Networking must NOT be unshared — a downloader needs it.
    QVERIFY(!argv.contains(QLatin1String("--unshare-net")));
    QVERIFY(!argv.contains(QLatin1String("--unshare-all")));
    // The Qt runtime prefix this build reports is bound read-only.
    QVERIFY(argv.contains(QLatin1String("--ro-bind-try")));
}

void tst_Sandbox::bwrapDownloadWorkerRemapsHome()
{
    // A worker binary/Qt prefix under the REAL $HOME must not be
    // bound at its own path — bwrap creates every bind target's
    // parent dirs, which would leave a readable ~ skeleton inside
    // the namespace.  Home-resident inputs are remapped under the
    // private /arora-rt tmpfs and the exec target follows.
    const QString homeProg =
        QDir::homePath() + QStringLiteral("/build/arora");
    const QStringList mapped =
        BwrapGenerator::downloadWorkerCommandLine(
            homeProg, {QStringLiteral("--download-worker")},
            QStringLiteral("/tmp/arora-w"), QStringLiteral("/tmp/arora-d"));

    const int dd = mapped.indexOf(QStringLiteral("--"));
    QVERIFY(dd > 0);
    QVERIFY(mapped.at(dd + 1).startsWith(QStringLiteral("/arora-rt/")));
    QCOMPARE(mapped.at(dd + 2), QStringLiteral("--download-worker"));

    // The real binary path appears only as a bind SOURCE.
    const int pi = mapped.indexOf(homeProg);
    QVERIFY(pi > 0
            && mapped.at(pi - 1) == QLatin1String("--ro-bind-try"));

    // No mount TARGET may live under $HOME — that is what would
    // resurrect the skeleton.  Bind flags take src dest pairs; tmpfs
    // takes a single dest.
    const QString home = QDir::homePath();
    for (int i = 0; i + 1 < mapped.size(); ++i) {
        const QString &flag = mapped.at(i);
        QString target;
        if (flag == QLatin1String("--bind")
                || flag == QLatin1String("--ro-bind-try")
                || flag == QLatin1String("--ro-bind")
                || flag == QLatin1String("--dev")
                || flag == QLatin1String("--proc"))
            target = mapped.value(i + 2);
        else if (flag == QLatin1String("--tmpfs"))
            target = mapped.value(i + 1);
        if (!target.isEmpty())
            QVERIFY2(!target.startsWith(home + QLatin1Char('/'))
                         && target != home,
                     qPrintable(target));
    }
    // And no --setenv leaks an unmapped home path either (the value
    // sits at i + 2, after the variable name).
    for (int i = 0; i + 2 < mapped.size(); ++i) {
        if (mapped.at(i) == QLatin1String("--setenv"))
            QVERIFY2(!mapped.at(i + 2).contains(home),
                     qPrintable(mapped.at(i + 2)));
    }
}

void tst_Sandbox::seatbeltDownloadWorkerProfile()
{
    const QString profile = SeatbeltGenerator::downloadWorkerProfile(
        QStringLiteral("/Library/Caches/Arora/w-abc"),
        QStringLiteral("/Users/t/Downloads"));
    QVERIFY(profile.contains(QLatin1String("(version 1)")));
    QVERIFY(profile.contains(QLatin1String("(deny default)")));
    QVERIFY(profile.contains(QLatin1String("(allow network-outbound)")));
    QVERIFY(profile.contains(QLatin1String(
        "(allow file-read* file-write* (subpath "
        "\"/Library/Caches/Arora/w-abc\"))")));
    QVERIFY(profile.contains(QLatin1String(
        "(allow file-read* file-write* (subpath "
        "\"/Users/t/Downloads\"))")));
    // Restrictive: no permissive catch-all, and the only read/write
    // subpaths are /dev + the two granted dirs.
    QVERIFY(!profile.contains(QLatin1String("(allow default)")));
    QCOMPARE(profile.count(
                 QLatin1String("(allow file-read* file-write* "
                               "(subpath")),
             3);
}

void tst_Sandbox::appContainerDownloadWorkerManifest()
{
    const QString xml = AppContainerGenerator::downloadWorkerManifestXml(
        QStringLiteral("C:\\Users\\t\\AppData\\w-abc"),
        QStringLiteral("C:\\Users\\t\\Downloads"));
    QXmlStreamReader reader(xml);
    bool sawInternet = false;
    bool sawWorkWrite = false;
    bool sawDestWrite = false;
    int capabilities = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        const auto attrs = reader.attributes();
        if (reader.name() == QLatin1String("capability")) {
            ++capabilities;
            if (attrs.value(QLatin1String("name"))
                == QLatin1String("internetClient"))
                sawInternet = true;
        }
        if (reader.name() == QLatin1String("rule")
            && attrs.value(QLatin1String("access"))
                == QLatin1String("write")) {
            const QStringView path =
                attrs.value(QLatin1String("path"));
            if (path == QLatin1String("C:\\Users\\t\\AppData\\w-abc"))
                sawWorkWrite = true;
            if (path == QLatin1String("C:\\Users\\t\\Downloads"))
                sawDestWrite = true;
        }
    }
    QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    QVERIFY(sawInternet);
    QCOMPARE(capabilities, 1); // internetClient only — no LAN/private
    QVERIFY(sawWorkWrite);
    QVERIFY(sawDestWrite);
}

void tst_Sandbox::openBsdDownloadWorkerPlan()
{
    const OpenBsdPlan plan = OpenBsdGenerator::downloadWorkerPlan(
        QStringLiteral("/home/t/.local/share/Arora/w-abc"),
        QStringLiteral("/home/t/Downloads"));
    // Allowlist only — no broad "/" grant.
    QVERIFY(!plan.unveilRules.contains(QStringLiteral("/|rwc")));
    QVERIFY(plan.unveilRules.contains(
        QStringLiteral("/home/t/.local/share/Arora/w-abc|rwc")));
    QVERIFY(plan.unveilRules.contains(
        QStringLiteral("/home/t/Downloads|rwc")));
    // A worker that never forks drops the whole exec/proc family —
    // word-level compare so prot_exec (needed for mapped libs) does
    // not trip the check.
    const QStringList promises = plan.pledgePromises.split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    QVERIFY(!promises.contains(QLatin1String("exec")));
    QVERIFY(!promises.contains(QLatin1String("proc")));
    QVERIFY(!promises.contains(QLatin1String("audio")));
    QVERIFY(!promises.contains(QLatin1String("video")));
    QVERIFY(promises.contains(QLatin1String("inet")));
    QVERIFY(promises.contains(QLatin1String("wpath")));
}

QTEST_MAIN(tst_Sandbox)
#include "tst_sandbox.moc"
