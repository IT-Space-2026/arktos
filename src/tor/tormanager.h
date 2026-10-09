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

#ifndef TORMANAGER_H
#define TORMANAGER_H

#include <qhostaddress.h>
#include <qnetworkproxy.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>

class QProcess;
class QTimer;
class TorControl;

// TOR04: one parsed line of GETINFO circuit-status — the path column
// is the hop chain (guard -> middle -> exit).  ServerIDs arrive as
// "$FP~Nick" / "$FP=Nick" / "$FP" / bare nickname; the '$' is
// stripped into fingerprint.
struct TorCircuitHop
{
    QString fingerprint;   // 40-hex, without the leading '$'
    QString nickname;      // may be empty
};

struct TorCircuit
{
    int id = -1;               // CircID
    QString status;            // LAUNCHED/EXTENDED/BUILT/FAILED/CLOSED
    QString purpose;           // PURPOSE= value, empty when absent
    QList<TorCircuitHop> hops;
};

// TOR01: lifecycle manager for a `tor` child daemon.
//
// QtWebEngine has no per-tab/per-profile proxy API (verified in the
// 6.11.3 headers — QNetworkProxy::setApplicationProxy is
// process-global), so Tor routing has to happen in a separate Arora
// process that sets the application proxy to this daemon's SOCKS5
// listener.  That is also the privacy-correct design: Tor and clearnet
// traffic can never share a profile.
//
// The daemon is spawned with:
//   DataDirectory <dataDirectory()>   (private, under the app data dir)
//   SocksPort    auto                 (tor picks a loopback port)
//   ControlPort  auto                 + ControlPortWriteToFile, so the
//                                     manager learns the port race-free
//   CookieAuthentication 1            (control_auth_cookie, 0600)
// and is driven over the real tor control protocol: AUTHENTICATE with
// the cookie, TAKEOWNERSHIP (tor exits if our control connection dies
// — the daemon can never be orphaned by a crash), SETEVENTS
// STATUS_CLIENT for bootstrap progress, GETINFO net/listeners/socks
// for the SOCKS endpoint.  state() becomes Ready only after bootstrap
// PROGRESS=100.
class TorManager : public QObject
{
    Q_OBJECT

public:
    enum State {
        Stopped,
        Starting,      // process spawned, waiting for the control-port file
        Connecting,    // control connection + authentication in flight
        Bootstrapping, // authenticated, PROGRESS < 100
        Ready,         // bootstrapped; socksProxy() is usable
        Stopping,
        Failed
    };
    Q_ENUM(State)

    explicit TorManager(QObject *parent = nullptr);
    ~TorManager() override;

    // Binary resolution order:
    //   1. $ARORA_TOR_BINARY (explicit override — a set-but-missing
    //      path is an error, not a silent fallback)
    //   2. tor / tor.exe on PATH
    //   3. system locations (/usr/bin, /usr/local/bin, /usr/sbin)
    //   4. bundled copies: <appdir>/tor, <appdir>/../tor,
    //      <appdir>/../lib/tor (packaging layout)
    //   5. the per-user expert bundle that BuildProcess/fetch-tor.sh
    //      installs under the application data directory
    // Empty when nothing resolves.
    static QString resolveBinary();
    // Directories searched after PATH — for diagnostics/tests.
    static QStringList binarySearchPaths();

    QString binaryPath() const;
    void setBinaryPath(const QString &path);   // before start()

    QString dataDirectory() const;             // default:
                                               // BrowserPaths::dataFilePath("tor")
    void setDataDirectory(const QString &dir); // before start()

    State state() const;
    bool isReady() const;
    bool isRunning() const;                    // daemon process alive
    QString errorString() const;
    int bootstrapProgress() const;             // -1 until reported
    QString bootstrapSummary() const;
    QNetworkProxy socksProxy() const;          // valid when Ready
    quint16 socksPort() const;                 // 0 until known

    // TOR04: last parsed circuit-status snapshot (populated by
    // requestCircuitInfo(); the control connection also fires a
    // debounced refresh on every 650 CIRC event).
    QList<TorCircuit> circuits() const;
    // Parses a circuit-status reply — tolerates the "key=" header and
    // the trailing "OK" the reply collector includes.  Static for
    // unit tests.
    static QList<TorCircuit> parseCircuitStatus(const QStringList &lines);
    // The circuit the UI should surface: the BUILT circuit carrying
    // the most streams, else the newest BUILT PURPOSE=GENERAL, else
    // the newest BUILT at all; -1 when none qualify.
    int displayCircuitId() const;

public slots:
    void start();
    void stop();    // SIGNAL SHUTDOWN -> terminate -> kill, all bounded
    // TOR04: queues GETINFO circuit-status + stream-status on the
    // control connection (serialized with everything else — no second
    // connection); coalesced while a query is in flight.
    void requestCircuitInfo();

signals:
    void stateChanged(TorManager::State state);
    void bootstrapProgressChanged(int progress, const QString &summary);
    void ready(const QNetworkProxy &proxy);
    void failed(const QString &reason);
    void stopped();
    void logLine(const QString &line);         // notice-level tor output
    void circuitsChanged(const QList<TorCircuit> &circuits);

private:
    void setState(State state);
    void fail(const QString &reason);
    void onControlPortFile();
    void onControlConnected();
    void onAsyncEvent(const QString &line);
    void onBootstrapLine(const QString &line);
    void querySocksListener();

    QString m_binaryPath;
    QString m_dataDirectory;
    QString m_controlPortFile;
    State m_state;
    QString m_errorString;
    int m_bootstrapProgress;
    QString m_bootstrapSummary;
    QHostAddress m_socksHost;
    quint16 m_socksPort;

    QList<TorCircuit> m_circuits;
    int m_streamCircuitId;          // busiest attached circuit, -1
    bool m_circuitQueryInFlight;
    QTimer *m_circuitRefreshTimer;  // debounces bursts of 650 CIRC lines

    QProcess *m_process;
    TorControl *m_control;
    QTimer *m_portFileTimer;
};

Q_DECLARE_METATYPE(TorCircuit)
Q_DECLARE_METATYPE(QList<TorCircuit>)

#endif // TORMANAGER_H
