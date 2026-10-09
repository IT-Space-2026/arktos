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
#include <qhash.h>
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
    , m_streamCircuitId(-1)
    , m_circuitQueryInFlight(false)
    , m_circuitRefreshTimer(nullptr)
    , m_process(nullptr)
    , m_control(nullptr)
    , m_portFileTimer(nullptr)
{
    // Needed by QSignalSpy/queued consumers of circuitsChanged.
    qRegisterMetaType<TorCircuit>("TorCircuit");
    qRegisterMetaType<QList<TorCircuit> >("QList<TorCircuit>");
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
    // OR into (never replace) the type defaults — setCapabilities()
    // wholesale-overwrites TunnelingCapability, which QNAM requires to
    // even attempt a CONNECT through the proxy.
    proxy.setCapabilities(proxy.capabilities()
                          | QNetworkProxy::HostNameLookupCapability);
    return proxy;
}

quint16 TorManager::socksPort() const
{
    return m_socksPort;
}

QList<TorCircuit> TorManager::circuits() const
{
    return m_circuits;
}

// A circuit line is "<CircID> <Status> [CircPath] KEY=VALUE..." where
// the path is a single comma-separated ServerID token ("$FP~Nick" /
// "$FP=Nick" / "$FP" / bare nickname).  Lines that do not start with a
// numeric id — the "circuit-status=" header and the trailing "OK" the
// reply collector includes — are skipped.
QList<TorCircuit> TorManager::parseCircuitStatus(const QStringList &lines)
{
    QList<TorCircuit> circuits;
    for (const QString &line : lines) {
        const QStringList parts =
            line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            continue;
        bool ok = false;
        const int id = parts.at(0).toInt(&ok);
        if (!ok)
            continue;
        TorCircuit circuit;
        circuit.id = id;
        circuit.status = parts.at(1);
        for (int i = 2; i < parts.size(); ++i) {
            const QString &field = parts.at(i);
            // A NAME=value keyword — except "$FP=Nick", which is a
            // path token (starts with '$'/'~' or carries ',').
            if (field.indexOf(QLatin1Char('=')) > 0
                && !field.startsWith(QLatin1Char('$'))
                && !field.startsWith(QLatin1Char('~'))) {
                if (field.startsWith(QLatin1String("PURPOSE=")))
                    circuit.purpose = field.mid(8);
                continue;
            }
            const QStringList servers =
                field.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &server : servers) {
                TorCircuitHop hop;
                int split = server.indexOf(QLatin1Char('~'));
                const int eq = server.indexOf(QLatin1Char('='));
                if (split < 0 || (eq >= 0 && eq < split))
                    split = eq;
                if (split >= 0) {
                    hop.fingerprint = server.left(split);
                    hop.nickname = server.mid(split + 1);
                } else if (server.startsWith(QLatin1Char('$'))) {
                    hop.fingerprint = server;
                } else {
                    hop.nickname = server;
                }
                if (hop.fingerprint.startsWith(QLatin1Char('$')))
                    hop.fingerprint.remove(0, 1);
                circuit.hops << hop;
            }
        }
        circuits << circuit;
    }
    return circuits;
}

int TorManager::displayCircuitId() const
{
    const TorCircuit *byStreams = nullptr;
    const TorCircuit *newestBuiltGeneral = nullptr;
    const TorCircuit *newestBuilt = nullptr;
    for (const TorCircuit &circuit : m_circuits) {
        if (circuit.status != QLatin1String("BUILT"))
            continue;
        if (circuit.id == m_streamCircuitId)
            byStreams = &circuit;
        if (!newestBuilt || circuit.id > newestBuilt->id)
            newestBuilt = &circuit;
        if (circuit.purpose == QLatin1String("GENERAL")
            && (!newestBuiltGeneral
                || circuit.id > newestBuiltGeneral->id))
            newestBuiltGeneral = &circuit;
    }
    if (byStreams)
        return byStreams->id;
    if (newestBuiltGeneral)
        return newestBuiltGeneral->id;
    return newestBuilt ? newestBuilt->id : -1;
}

void TorManager::requestCircuitInfo()
{
    if (!m_control || !m_control->isConnected()
        || m_circuitQueryInFlight)
        return;
    m_circuitQueryInFlight = true;
    m_control->getInfo(QLatin1String("circuit-status"),
                     [this](int code, const QStringList &lines) {
        m_circuits = (code == 250) ? parseCircuitStatus(lines)
                                   : QList<TorCircuit>();
        // stream-status maps StreamID -> CircID so the display prefers
        // the circuit the tabs' traffic actually rides; it queues
        // behind the circuit-status reply on the same connection.
        m_control->getInfo(QLatin1String("stream-status"),
                         [this](int code, const QStringList &lines) {
            m_streamCircuitId = -1;
            if (code == 250) {
                QHash<int, int> attached;
                for (const QString &line : lines) {
                    const QStringList parts = line.split(
                        QLatin1Char(' '), Qt::SkipEmptyParts);
                    // "<StreamID> <Status> <CircID> <Target>"
                    if (parts.size() < 4)
                        continue;
                    bool ok = false;
                    const int circId = parts.at(2).toInt(&ok);
                    if (ok && circId > 0)
                        attached[circId] += 1;
                }
                int best = 0;
                for (auto it = attached.constBegin();
                     it != attached.constEnd(); ++it) {
                    if (it.value() > best) {
                        best = it.value();
                        m_streamCircuitId = it.key();
                    }
                }
            }
            m_circuitQueryInFlight = false;
            fillHopCountries();
            emit circuitsChanged(m_circuits);
            // TOR05: look up hop countries for the displayed circuit
            // — queued on the same control connection, results land
            // through finishHopCountryQuery -> circuitsChanged.
            resolveHopCountries();
        });
    });
}

// 'r' line: "r nickname identity digest YYYY-MM-DD HH:MM:SS address
// orport dirport" — the OR address is field 6.
QString TorManager::parseNsAddress(const QStringList &lines)
{
    for (const QString &line : lines) {
        if (!line.startsWith(QLatin1String("r ")))
            continue;
        const QStringList parts =
            line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 7)
            return parts.at(6);
    }
    return QString();
}

bool TorManager::fillHopCountries()
{
    bool changed = false;
    for (TorCircuit &circuit : m_circuits) {
        for (TorCircuitHop &hop : circuit.hops) {
            const QString cached =
                m_countryCache.value(hop.fingerprint);
            if (hop.country != cached) {
                hop.country = cached;
                changed = true;
            }
        }
    }
    return changed;
}

void TorManager::resolveHopCountries()
{
    if (!m_control || !m_control->isConnected())
        return;
    const int displayId = displayCircuitId();
    const TorCircuit *circuit = nullptr;
    for (const TorCircuit &candidate : m_circuits) {
        if (candidate.id == displayId) {
            circuit = &candidate;
            break;
        }
    }
    if (!circuit)
        return;
    for (const TorCircuitHop &hop : circuit->hops) {
        const QString fingerprint = hop.fingerprint;
        if (fingerprint.isEmpty()
            || m_countryCache.contains(fingerprint)
            || m_countryQueriesInFlight.contains(fingerprint))
            continue;
        m_countryQueriesInFlight.insert(fingerprint);
        m_control->getInfo(QLatin1String("ns/id/") + fingerprint,
            [this, fingerprint](int code, const QStringList &lines) {
            const QString address =
                (code == 250) ? parseNsAddress(lines) : QString();
            if (address.isEmpty()) {
                finishHopCountryQuery(fingerprint, QString());
                return;
            }
            m_control->getInfo(
                QLatin1String("ip-to-country/") + address,
                [this, fingerprint](int code,
                                    const QStringList &lines) {
                QString country;
                if (code == 250) {
                    // "ip-to-country/<addr>=<cc>" — "??" is tor's
                    // unmapped / no-geoip sentinel.
                    for (const QString &line : lines) {
                        if (!line.startsWith(
                                QLatin1String("ip-to-country/")))
                            continue;
                        const int eq = line.indexOf(QLatin1Char('='));
                        if (eq >= 0)
                            country = line.mid(eq + 1).trimmed()
                                .toUpper();
                    }
                    if (country == QLatin1String("??"))
                        country.clear();
                }
                finishHopCountryQuery(fingerprint, country);
            });
        });
    }
}

void TorManager::finishHopCountryQuery(const QString &fingerprint,
                                       const QString &country)
{
    m_countryQueriesInFlight.remove(fingerprint);
    if (!m_countryCache.contains(fingerprint)) {
        m_countryCache.insert(fingerprint, country);
        m_countryCacheOrder << fingerprint;
        const int cacheCap = 512;
        while (m_countryCacheOrder.size() > cacheCap)
            m_countryCache.remove(m_countryCacheOrder.takeFirst());
    }
    if (fillHopCountries())
        emit circuitsChanged(m_circuits);
}

void TorManager::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
    if (state == Ready)
        requestCircuitInfo();
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
    m_circuits.clear();
    m_streamCircuitId = -1;
    m_circuitQueryInFlight = false;
    // The fp->country cache survives a daemon restart (relay geography
    // does not change), but queries orphaned by a dead connection must
    // not stay marked in flight.
    m_countryQueriesInFlight.clear();

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
    // TOR05: without a GeoIPFile the control port's ip-to-country
    // lookups all answer "??", so fall back to the system database
    // (e.g. debian's tor-geoipdb) when the bundle ships none — a
    // system tor already loads its compiled-in path, passing it
    // explicitly is a no-op there.
    const QString binaryDir =
        QFileInfo(binary).canonicalPath();
    const QStringList geoipDirs = {
        binaryDir + QLatin1String("/../data"),
        binaryDir,
#ifdef Q_OS_UNIX
        QLatin1String("/usr/share/tor"),
        QLatin1String("/usr/local/share/tor"),
#endif
    };
    for (const char *name : {"geoip", "geoip6"}) {
        for (const QString &dir : geoipDirs) {
            const QString candidate =
                dir + QLatin1Char('/') + QLatin1String(name);
            if (!QFile::exists(candidate))
                continue;
            args << (QLatin1String(name) == QLatin1String("geoip6")
                         ? QLatin1String("--GeoIPv6File")
                         : QLatin1String("--GeoIPFile"))
                 << QDir::toNativeSeparators(
                        QFileInfo(candidate).canonicalFilePath());
            break;
        }
    }

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
        // CIRC events drive the circuit-chain surface (TOR04).
        m_control->setEvents(QStringList()
                             << QLatin1String("STATUS_CLIENT")
                             << QLatin1String("CIRC"));
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
    else if (line.startsWith(QLatin1String("CIRC "))) {
        // A burst of BUILT/CLOSED lines coalesces into one refresh —
        // otherwise each event would queue its own GETINFO pair.
        if (!m_circuitRefreshTimer) {
            m_circuitRefreshTimer = new QTimer(this);
            m_circuitRefreshTimer->setSingleShot(true);
            m_circuitRefreshTimer->setInterval(250);
            connect(m_circuitRefreshTimer, &QTimer::timeout,
                    this, &TorManager::requestCircuitInfo);
        }
        m_circuitRefreshTimer->start();
    }
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
    if (m_circuitRefreshTimer)
        m_circuitRefreshTimer->stop();
    m_circuits.clear();
    m_streamCircuitId = -1;
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
