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

// CRASH02: CrashReporter — the fatal-signal/std::terminate reporter
// that turns silent exits into marker + backtrace files.

#include <QtTest/QtTest>

#include <crashreporter.h>

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>

#include "qtest_arora.h"

#if defined(Q_OS_UNIX)
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdexcept>
#endif

class tst_CrashReporter : public QObject
{
    Q_OBJECT

private slots:
    void installPath();
    void envOverride();

#if defined(Q_OS_UNIX)
    void segvProducesReport();
    void abortProducesReport();
    void terminateProducesReport();
#endif
};

void tst_CrashReporter::installPath()
{
    const QString dir =
        QDir::temp().filePath(QLatin1String("arora-crashreporter-test"));
    const QString want = dir + QLatin1String("/crashreport.log");
    const QString got = CrashReporter::install(want);
    QCOMPARE(got, want);
    QCOMPARE(CrashReporter::logPath(), want);
    QVERIFY(QFileInfo::exists(dir));
}

void tst_CrashReporter::envOverride()
{
    const QString override_ = QDir::temp().filePath(
        QLatin1String("arora-crashreporter-env.log"));
    qputenv("ARORA_CRASH_LOG", QFile::encodeName(override_));
    const QString got = CrashReporter::install(
        QStringLiteral("/should/not/be/used.log"));
    QCOMPARE(got, override_);
    QCOMPARE(CrashReporter::logPath(), override_);
    qunsetenv("ARORA_CRASH_LOG");
}

#if defined(Q_OS_UNIX)

// Forks a child that dies by the chosen fault; the parent asserts the
// death is signaled and the report file carries the marker.
static int forkAndWaitForDeath(const char *kind)
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        // Child: async-signal-safe calls only — raise/abort/terminate
        // and _exit.
        if (!::strcmp(kind, "abrt")) {
            ::abort();
        } else if (!::strcmp(kind, "terminate")) {
            struct Rethrower {
                [[noreturn]] static void go() noexcept
                {
                    throw std::runtime_error(
                        "crashreporter-test uncaught");
                }
            };
            Rethrower::go();
        } else {
            ::raise(SIGSEGV);
        }
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}

static QByteArray readLog(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

void tst_CrashReporter::segvProducesReport()
{
    const QString log = QDir::temp().filePath(
        QLatin1String("arora-crash-segv.log"));
    QFile::remove(log);
    qputenv("ARORA_CRASH_LOG", QFile::encodeName(log));
    CrashReporter::install(QString());
    const int status = forkAndWaitForDeath("segv");
    QVERIFY(WIFSIGNALED(status));
    QCOMPARE(WTERMSIG(status), SIGSEGV);
    const QByteArray report = readLog(log);
    QVERIFY(report.contains("ARORA CRASH"));
    QVERIFY(report.contains("signal=11 (SIGSEGV)"));
    // Marker lines + a non-empty backtrace — one line is no report.
    QVERIFY(report.count('\n') >= 3);
}

void tst_CrashReporter::abortProducesReport()
{
    const QString log = QDir::temp().filePath(
        QLatin1String("arora-crash-abrt.log"));
    QFile::remove(log);
    qputenv("ARORA_CRASH_LOG", QFile::encodeName(log));
    CrashReporter::install(QString());
    const int status = forkAndWaitForDeath("abrt");
    QVERIFY(WIFSIGNALED(status));
    QCOMPARE(WTERMSIG(status), SIGABRT);
    QVERIFY(readLog(log).contains("signal=6 (SIGABRT)"));
}

void tst_CrashReporter::terminateProducesReport()
{
    const QString log = QDir::temp().filePath(
        QLatin1String("arora-crash-term.log"));
    QFile::remove(log);
    qputenv("ARORA_CRASH_LOG", QFile::encodeName(log));
    CrashReporter::install(QString());
    const int status = forkAndWaitForDeath("terminate");
    QVERIFY(WIFSIGNALED(status));
    QCOMPARE(WTERMSIG(status), SIGABRT);
    const QByteArray report = readLog(log);
    QVERIFY(report.contains("ARORA CRASH"));
    QVERIFY(report.contains("crashreporter-test uncaught"));
}

#endif // Q_OS_UNIX

QTEST_MAIN(tst_CrashReporter)
#include "tst_crashreporter.moc"
