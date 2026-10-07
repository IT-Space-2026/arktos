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

#include "tormanager.h"

#include "browserpaths.h"
#include "torcontrol.h"

#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qprocess.h>
#include <qregularexpression.h>
#include <qstandardpaths.h>
#include <qtimer.h>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

TorManager::TorManager(QObject *parent)
    : QObject(parent)
    , m_state(Stopped)
    , m_bootstrapProgress(-1)
    , m_socksPort(0)
    , m_process(nullptr)
    , m_control(nullptr)
    , m_portFileTimer(nullptr)
{
}

TorManager::~TorManager()
{
    if (m_state != Stopped)
        stop();
}

QStringList TorManager::binarySearchPaths()
{
    QStringList dirs;
#ifdef Q_OS_UNIX
    dirs << QStringLiteral("/usr/bin")
         << QStringLiteral("/usr/local/bin")
         << QStringLiteral("/usr/sbin");
#endif
    // Bundled expert-bundle layouts: a tor/ dir shipped beside the
    // binary, one level up, or under lib/.
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
        dirs << appDir + QLatin1String("/tor")
             << appDir + QLatin1String("/../tor")
             << appDir + QLatin1String("/../lib/tor");
    // BuildProcess/fetch-tor.sh installs the per-user bundle here.
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty())
        dirs << dataDir + QLatin1String("/tor");
    return dirs;
}

QString TorManager::resolveBinary()
{
    const QByteArray env = qgetenv("ARORA_TOR_BINARY");
    if (!env.isEmpty()) {
        const QString path = QFile::decodeName(env);
        return QFileInfo(path).isExecutable() ? path : QString();
    }
    for (const char *name : {"tor", "tor.exe"}) {
        const QString found =
            QStandardPaths::findExecutable(QLatin1String(name));
        if (!found.isEmpty())
            return found;
    }
    const QStringList dirs = binarySearchPaths();
    for (const QString &dir : dirs) {
        for (const char *name : {"tor", "tor.exe"}) {
            const QString candidate = dir + QLatin1Char('/')
                + QLatin1String(name);
            if (QFileInfo(candidate).isExecutable())
                return candidate;
        }
    }
    return QString();
}

QString TorManager::binaryPath() const
{
    return m_binaryPath;
}

void TorManager::setBinaryPath(const QString &path)
{
    m_binaryPath = path;
}

QString TorManager::dataDirectory() const
{
    if (!m_dataDirectory.isEmpty())
        return m_dataDirectory;
    return BrowserPaths::dataFilePath(QLatin1String("tor"));
}

void TorManager::setDataDirectory(const QString &dir)
{
    m_dataDirectory = dir;
}

TorManager::State TorManager::state() const
{
    return m_state;
}

bool TorManager::isReady() const
{
    return m_state == Ready;
}

bool TorManager::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

QString TorManager::errorString() const
{
    return m_errorString;
}

int TorManager::bootstrapProgress() const
{
    return m_bootstrapProgress;
}

QString TorManager::bootstrapSummary() const
{
    return m_bootstrapSummary;
}

QNetworkProxy TorManager::socksProxy() const
{
    if (m_state != Ready)
        return QNetworkProxy();
    QNetworkProxy proxy(QNetworkProxy::Socks5Proxy,
                        m_socksHost.toString(), m_socksPort);
    // DNS goes to the resolver at the exit, not to the local resolver —
    // SOCKS5 host-name forwarding is what keeps lookups off the ISP.
    proxy.setCapabilities(QNetworkProxy::HostNameLookupCapability);
    return proxy;
}

quint16 TorManager::socksPort() const
{
    return m_socksPort;
}

void TorManager::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void TorManager::fail(const QString &reason)
{
    m_errorString = reason;
    setState(Failed);
    emit failed(reason);
}

void TorManager::start()
{
    if (m_state != Stopped && m_state != Failed)
        return;
    m_errorString.clear();
    m_bootstrapProgress = -1;
    m_bootstrapSummary.clear();
    m_socksPort = 0;

    // tor itself refuses to run as root unless coerced; enforce it here
    // too so the check survives config drift.
#ifdef Q_OS_UNIX
    if (::geteuid() == 0) {
        fail(QLatin1String("refusing to launch tor as root"));
        return;
    }
#endif

    const QString binary = m_binaryPath.isEmpty()
        ? resolveBinary() : m_binaryPath;
    if (binary.isEmpty() || !QFileInfo(binary).isExecutable()) {
        fail(QLatin1String("no tor binary found — set ARORA_TOR_BINARY, "
                           "install tor, or run BuildProcess/fetch-tor.sh "
                           "(searched: ")
             + binarySearchPaths().join(QLatin1String(", "))
             + QLatin1String(")"));
        return;
    }

    const QString dataDir = dataDirectory();
    QDir dir;
    if (!dir.mkpath(dataDir)) {
        fail(QLatin1String("cannot create data directory ") + dataDir);
        return;
    }
#ifdef Q_OS_UNIX
    QFile::setPermissions(dataDir, QFile::ReadOwner | QFile::WriteOwner
                          | QFile::ExeOwner);
#endif
    m_controlPortFile = dataDir + QLatin1String("/control-port");
    QFile::remove(m_controlPortFile);

    if (!m_process) {
        m_process = new QProcess(this);
        connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
            const QString lines =
                QString::fromLocal8Bit(m_process->readAllStandardError());
            const QStringList split = lines.split(QLatin1Char('\n'));
            for (const QString &line : split) {
                if (!line.isEmpty())
                    emit logLine(line);
            }
        });
        connect(m_process, &QProcess::errorOccurred,
                this, [this](QProcess::ProcessError) {
            if (m_state != Stopped && m_state != Stopping)
                fail(QLatin1String("failed to launch tor: ")
                     + m_process->errorString());
        });
        connect(m_process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus) {
            if (m_state == Starting || m_state == Connecting
                || m_state == Bootstrapping) {
                fail(QStringLiteral("tor exited before bootstrap "
                                    "(exit code %1)").arg(exitCode));
            } else if (m_state != Stopping) {
                setState(Stopped);
                emit stopped();
            }
        });
    }

    QStringList args;
    args << QLatin1String("--DataDirectory") << QDir::toNativeSeparators(dataDir)
         << QLatin1String("--ControlPort") << QLatin1String("auto")
         << QLatin1String("--ControlPortWriteToFile") << QDir::toNativeSeparators(m_controlPortFile)
         << QLatin1String("--CookieAuthentication") << QLatin1String("1")
         << QLatin1String("--SocksPort") << QLatin1String("auto")
         << QLatin1String("--Log") << QLatin1String("notice stderr")
         << QLatin1String("--RunAsDaemon") << QLatin1String("0")
         << QLatin1String("--AvoidDiskWrites") << QLatin1String("1")
         << QLatin1String("--ClientOnly") << QLatin1String("1")
         << QLatin1String("--SafeLogging") << QLatin1String("1");

    // The expert bundle ships geoip data in data/ next to tor/ — point
    // tor at it when present so it stops warning about missing files.
    const QString binaryDir =
        QFileInfo(binary).canonicalPath();
    const QString geoip = binaryDir + QLatin1String("/../data/geoip");
    const QString geoip6 = binaryDir + QLatin1String("/../data/geoip6");
    if (QFile::exists(geoip))
        args << QLatin1String("--GeoIPFile")
             << QDir::toNativeSeparators(QFileInfo(geoip).canonicalFilePath());
    if (QFile::exists(geoip6))
        args << QLatin1String("--GeoIPv6File")
             << QDir::toNativeSeparators(QFileInfo(geoip6).canonicalFilePath());

    m_process->setProgram(binary);
    m_process->setArguments(args);
    setState(Starting);
    m_process->start();

    // tor writes the control-port file as soon as the listener is up —
    // normally within milliseconds.  A plain poll is more reliable
    // than QFileSystemWatcher here: the file can legitimately appear
    // before a watcher is armed or inside a just-created directory.
    if (!m_portFileTimer) {
        m_portFileTimer = new QTimer(this);
        connect(m_portFileTimer, &QTimer::timeout,
                this, &TorManager::onControlPortFile);
    }
    m_portFileTimer->start(100);
}

void TorManager::onControlPortFile()
{
    if (m_state != Starting)
        return;
    QFile file(m_controlPortFile);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly))
        return;
    // Format: one "PORT=addr:port" line per listener ("auto" resolves
    // to 127.0.0.1).
    const QString content = QString::fromLatin1(file.readAll());
    const QRegularExpression re(
        QLatin1String("PORT=([0-9a-fA-F.:]+):(\\d+)"));
    const QRegularExpressionMatch match = re.match(content);
    if (!match.hasMatch())
        return; // partially written file — next tick catches it
    m_portFileTimer->stop();

    if (!m_control) {
        m_control = new TorControl(this);
        connect(m_control, &TorControl::asyncEvent,
                this, &TorManager::onAsyncEvent);
        connect(m_control, &TorControl::connected,
                this, &TorManager::onControlConnected);
        connect(m_control, &TorControl::disconnected, this, [this]() {
            // After TAKEOWNERSHIP a dropped control connection means
            // tor is already exiting — the process finished handler
            // settles the state.
            if (m_state == Connecting)
                fail(QLatin1String("tor dropped the control connection "
                                   "during setup"));
        });
    }
    const QString host = match.captured(1);
    const quint16 port = quint16(match.captured(2).toUInt());
    setState(Connecting);
    m_control->connectToHost(QHostAddress(host), port);
}

void TorManager::onControlConnected()
{
    // AUTHENTICATE -> TAKEOWNERSHIP -> SETEVENTS -> GETINFO bootstrap.
    // Everything is ordered through the command queue; a failure at any
    // step fails the launch.
    const QString cookieFile = dataDirectory()
        + QLatin1String("/control_auth_cookie");
    m_control->authenticateCookieFile(cookieFile, [this](int code,
                                                         const QStringList &) {
        if (code != 250) {
            fail(QLatin1String("tor control authentication failed"));
            return;
        }
        m_control->takeOwnership();
        m_control->setEvents(QStringList()
                             << QLatin1String("STATUS_CLIENT"));
        m_control->getInfo(QLatin1String("status/bootstrap-phase"),
                           [this](int code, const QStringList &lines) {
            if (code == 250 && !lines.isEmpty())
                onBootstrapLine(lines.first());
            if (m_state == Connecting) {
                setState(Bootstrapping);
                if (m_bootstrapProgress == 100)
                    querySocksListener();
            }
        });
    });
}

void TorManager::onAsyncEvent(const QString &line)
{
    // e.g. "STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=42 TAG=...
    //        SUMMARY=...""
    if (line.startsWith(QLatin1String("STATUS_CLIENT ")))
        onBootstrapLine(line.mid(14));
}

void TorManager::onBootstrapLine(const QString &line)
{
    const QRegularExpression progressRe(
        QLatin1String("BOOTSTRAP PROGRESS=(\\d+)"));
    const QRegularExpression summaryRe(
        QLatin1String("SUMMARY=\"([^\"]*)\""));
    const QRegularExpressionMatch progress = progressRe.match(line);
    if (!progress.hasMatch())
        return;
    m_bootstrapProgress = progress.captured(1).toInt();
    const QRegularExpressionMatch summary = summaryRe.match(line);
    if (summary.hasMatch())
        m_bootstrapSummary = summary.captured(1);
    emit bootstrapProgressChanged(m_bootstrapProgress,
                                  m_bootstrapSummary);
    if (m_bootstrapProgress >= 100
        && (m_state == Bootstrapping || m_state == Connecting))
        querySocksListener();
}

void TorManager::querySocksListener()
{
    if (!m_control || !m_control->isConnected())
        return;
    m_control->getInfo(QLatin1String("net/listeners/socks"),
                       [this](int code, const QStringList &lines) {
        if (code != 250 || lines.isEmpty()) {
            fail(QLatin1String("tor did not report a socks listener"));
            return;
        }
        // "net/listeners/socks=\"127.0.0.1:39237\"" — may list several.
        const QRegularExpression re(
            QLatin1String("\"?([0-9a-fA-F.:]+):(\\d+)\"?"));
        const QRegularExpressionMatch match = re.match(lines.first());
        if (!match.hasMatch()) {
            fail(QLatin1String("cannot parse tor socks listener"));
            return;
        }
        m_socksHost = QHostAddress(match.captured(1));
        m_socksPort = quint16(match.captured(2).toUInt());
        setState(Ready);
        emit ready(socksProxy());
    });
}

void TorManager::stop()
{
    if (m_state == Stopped)
        return;
    setState(Stopping);
    if (m_portFileTimer)
        m_portFileTimer->stop();
    if (m_control && m_control->isConnected())
        m_control->signalShutdown();
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // Bounded escalation — tor exits on SHUTDOWN within a second
        // normally; the fallbacks keep a wedged daemon from hanging
        // application quit.
        if (!m_process->waitForFinished(5000)) {
            m_process->terminate();
            if (!m_process->waitForFinished(2000)) {
                m_process->kill();
                m_process->waitForFinished(2000);
            }
        }
    }
    setState(Stopped);
    emit stopped();
}
