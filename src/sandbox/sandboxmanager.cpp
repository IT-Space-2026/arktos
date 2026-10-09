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

#include "sandboxmanager.h"

#include "bwrapgenerator.h"
#include "platformgenerators.h"
#include "sandboxpolicy.h"

#include <qdebug.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qsettings.h>
#include <qstandardpaths.h>

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS) || defined(Q_OS_OPENBSD)
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace {

// The environment marker a wrapped process carries.  Any value means
// "already inside" — the value itself names the backend.
const char kSandboxedEnv[] = "ARORA_SANDBOXED";

// Dev-only escape hatches: the env var, the --no-sandbox flag and the
// sandbox/enabled QSettings key (documented in README; not in the UI).
bool disabledByEnvOrFlag(int argc, char **argv)
{
    if (!qgetenv("ARORA_NO_SANDBOX").isEmpty())
        return true;
    for (int i = 1; i < argc; ++i) {
        if (QLatin1String(argv[i]) == QLatin1String("--no-sandbox"))
            return true;
    }
    return false;
}

bool disabledBySettings()
{
    const QSettings settings(QStringLiteral("Arora"),
                             QStringLiteral("Arora"));
    return !settings.value(QStringLiteral("sandbox/enabled"), true)
            .toBool();
}

} // namespace

QString SandboxManager::bwrapPath()
{
    const QByteArray overridePath = qgetenv("ARORA_BWRAP");
    if (!overridePath.isEmpty()) {
        const QString path = QString::fromLocal8Bit(overridePath);
        return QFileInfo(path).isExecutable() ? path : QString();
    }
    return QStandardPaths::findExecutable(QStringLiteral("bwrap"));
}

namespace {

// Browsing launches are arora [url...] and arora --tor — anything else
// carrying an option flag is a dev/smoke/utility run that keeps its
// deterministic unsandboxed environment.  ARORA_SANDBOX_FORCE=1 wraps
// flagged runs too (the smoke harness uses it to prove the fallback).
bool launchIsBrowsing(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const QLatin1String arg(argv[i]);
        if (arg.startsWith(QLatin1String("--"))
                && arg != QLatin1String("--tor"))
            return false;
    }
    return true;
}

} // namespace

bool SandboxManager::isSandboxed()
{
    return !qgetenv(kSandboxedEnv).isEmpty();
}

QString SandboxManager::backendName()
{
    const QByteArray marker = qgetenv(kSandboxedEnv);
    if (!marker.isEmpty())
        return QString::fromLocal8Bit(marker);
#if defined(Q_OS_LINUX)
    return bwrapPath().isEmpty() ? QStringLiteral("none")
                                  : QStringLiteral("bwrap");
#elif defined(Q_OS_MACOS)
    return QStandardPaths::findExecutable(QStringLiteral("sandbox-exec"))
            .isEmpty()
        ? QStringLiteral("none") : QStringLiteral("seatbelt");
#elif defined(Q_OS_WIN)
    return QStringLiteral("appcontainer");
#elif defined(Q_OS_OPENBSD)
    return QStringLiteral("unveil");
#else
    return QStringLiteral("none");
#endif
}

QString SandboxManager::statusReport()
{
    QStringList lines;
    lines << QStringLiteral("Arora sandbox status");
    lines << QStringLiteral("  backend: %1").arg(backendName());
    lines << QStringLiteral("  this process sandboxed: %1")
                 .arg(isSandboxed() ? QStringLiteral("yes")
                                    : QStringLiteral("no"));
    lines << QStringLiteral("  disabled by settings (sandbox/enabled=false): %1")
                 .arg(disabledBySettings() ? QStringLiteral("yes")
                                           : QStringLiteral("no"));
#if defined(Q_OS_LINUX)
    lines << QStringLiteral("  bwrap resolved: %1")
                 .arg(bwrapPath().isEmpty()
                          ? QStringLiteral("(not found — would run unsandboxed)")
                          : bwrapPath());
#endif
    const SandboxPolicy policy = SandboxPolicy::defaultPolicy();
    lines << QStringLiteral("  masked directories (%1):")
                 .arg(policy.deniedDirs.size());
    for (const QString &dir : policy.deniedDirs)
        lines << QStringLiteral("    %1").arg(dir);
    lines << QStringLiteral("  masked files (%1):").arg(policy.deniedFiles.size());
    for (const QString &file : policy.deniedFiles)
        lines << QStringLiteral("    %1").arg(file);
    lines << QStringLiteral("  read-only roots: %1")
                 .arg(policy.readOnlyRoots.join(QLatin1String(", ")));
    return lines.join(QLatin1Char('\n'));
}

void SandboxManager::maybeReexec(int argc, char **argv)
{
    if (isSandboxed())
        return;
    if (disabledByEnvOrFlag(argc, argv) || disabledBySettings())
        return;
    const bool forced = qgetenv("ARORA_SANDBOX_FORCE") == "1";
    if (!forced && !launchIsBrowsing(argc, argv))
        return;

#if defined(Q_OS_LINUX)
    const QString bwrap = bwrapPath();
    if (bwrap.isEmpty()) {
        // Never hard-fail: the sandbox is defense-in-depth, not a
        // launch requirement.
        qWarning("Arora: bwrap not found — running without the "
                 "filesystem sandbox (ARORA_NO_SANDBOX=1 silences this)");
        return;
    }

    const QString program =
        QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
    if (program.isEmpty()) {
        qWarning("Arora: cannot resolve /proc/self/exe — running "
                 "unsandboxed");
        return;
    }
    QStringList programArgs;
    for (int i = 1; i < argc; ++i)
        programArgs << QString::fromLocal8Bit(argv[i]);
    const QStringList command = BwrapGenerator::commandLine(
        SandboxPolicy::defaultPolicy(), program, programArgs);

    QVector<QByteArray> storage;
    storage.reserve(command.size());
    for (const QString &piece : command)
        storage.append(piece.toLocal8Bit());
    storage[0] = bwrap.toLocal8Bit(); // honour the ARORA_BWRAP override
    QVector<char *> execArgv;
    execArgv.reserve(storage.size() + 1);
    for (QByteArray &piece : storage)
        execArgv.append(piece.data());
    execArgv.append(nullptr);

    // execvp only returns on failure — warn and continue unsandboxed.
    if (execvp(execArgv[0], execArgv.data()) == -1)
        qWarning("Arora: failed to enter bwrap sandbox: %s — running "
                 "unsandboxed", strerror(errno));
    return;
#elif defined(Q_OS_MACOS)
    // UNTESTED-ON-TARGET: generated profile is unit-tested on Linux;
    // this apply path compiles only on macOS.
    const QString sandboxExec = QStandardPaths::findExecutable(
        QStringLiteral("sandbox-exec"));
    if (sandboxExec.isEmpty()) {
        qWarning("Arora: sandbox-exec not found — running unsandboxed");
        return;
    }
    const QString profile =
        SeatbeltGenerator::profile(SandboxPolicy::defaultPolicy());
    QStringList programArgs;
    for (int i = 1; i < argc; ++i)
        programArgs << QString::fromLocal8Bit(argv[i]);
    const QString program = QString::fromLocal8Bit(argv[0]);
    QStringList command{QStringLiteral("-p"), profile, program};
    command << programArgs;
    QVector<QByteArray> storage;
    for (const QString &piece : command)
        storage.append(piece.toLocal8Bit());
    QVector<char *> execArgv;
    for (QByteArray &piece : storage)
        execArgv.append(piece.data());
    execArgv.append(nullptr);
    execvp(sandboxExec.toLocal8Bit().constData(), execArgv.data());
    qWarning("Arora: failed to enter seatbelt sandbox — running "
             "unsandboxed");
    return;
#elif defined(Q_OS_OPENBSD)
    // UNTESTED-ON-TARGET: in-process unveil(2)/pledge(2).  The plan is
    // generated by OpenBsdGenerator (unit-tested on Linux); the calls
    // below are the trivial mechanical apply.
    const OpenBsdPlan plan =
        OpenBsdGenerator::plan(SandboxPolicy::defaultPolicy());
    bool failed = false;
    for (const QString &rule : plan.unveilRules) {
        const int split = rule.lastIndexOf(QLatin1Char('|'));
        const QByteArray path = rule.left(split).toLocal8Bit();
        const QByteArray perms = rule.mid(split + 1).toLocal8Bit();
        if (unveil(path.constData(), perms.constData()) == -1)
            failed = true;
    }
    if (unveil(nullptr, nullptr) == -1 || failed)
        qWarning("Arora: unveil() failed — continuing with partial "
                 "sandbox");
    if (pledge(plan.pledgePromises.toLocal8Bit().constData(), nullptr) == -1)
        qWarning("Arora: pledge() failed — continuing unsandboxed");
    return;
#elif defined(Q_OS_WIN)
    // UNTESTED-ON-TARGET: AppContainer cannot wrap the running process
    // — the launcher must spawn a child inside the container via
    // CreateAppContainerProfile + the extended startup attribute list.
    // The generated manifest (AppContainerGenerator, unit-tested on
    // Linux) is the launcher's rule source; the CreateProcess wiring
    // is deliberately not landed blind — the stub fails soft.
    qWarning("Arora: AppContainer sandbox backend is compiled but the "
             "re-exec launcher is not implemented on this platform yet "
             "— running unsandboxed");
    return;
#elif defined(Q_OS_FREEBSD)
    // Known gap: Capsicum's cap_enter() drops a process into
    // capability mode but cannot retroactively mask filesystem paths
    // — there is no unveil(2) equivalent on FreeBSD.  A real backend
    // needs a launcher split (spawn a child inside a capsicum'd
    // environment), which is not implemented; fail soft with a clear
    // warning instead of pretending.
    qWarning("Arora: no sandbox backend on FreeBSD — Capsicum needs a "
             "launcher split that is not implemented yet; running "
             "unsandboxed");
    return;
#else
    qWarning("Arora: no sandbox backend on this platform — running "
             "unsandboxed");
    return;
#endif
}
