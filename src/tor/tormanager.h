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

#include <qelapsedtimer.h>
#include <qhash.h>
#include <qhostaddress.h>
#include <qnetworkproxy.h>
#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>

#include <functional>

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
    QString country;       // TOR05: ISO-3166 code resolved via the
                           // control port (ns/id -> ip-to-country);
                           // empty while unresolved or unresolvable
    QString address;       // TOR08: OR address from the same ns/id
                           // lookup that resolves the country; empty
                           // while unresolved or unresolvable
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

    void setBinaryPath(const QString &path);   // before start()

    QString dataDirectory() const;             // default:
                                               // BrowserPaths::dataFilePath("tor")
    void setDataDirectory(const QString &dir); // before start()

    State state() const;
    bool isReady() const;
    // TOR18: ms since the SOCKS listener went live (state Ready), -1
    // otherwise.  PROGRESS=100 only means the listener answers — the
    // first exit circuits are still being built for a stretch after
    // it, so navigations inside this window still race cold circuits.
    qint64 readyElapsed() const;
    bool isRunning() const;                    // daemon process alive
    QString errorString() const;
    int bootstrapProgress() const;             // -1 until reported
    QString bootstrapSummary() const;
    QNetworkProxy socksProxy() const;          // valid when Ready
    quint16 socksPort() const;                 // 0 until known
    // TOR10: whether the control connection is actually live —
    // state() can sit at Ready after the daemon drops the link, and
    // the circuit surfaces must degrade honestly rather than serve a
    // stale snapshot.
    bool controlConnected() const;

    // TOR04: last parsed circuit-status snapshot (populated by
    // requestCircuitInfo(); the control connection also fires a
    // debounced refresh on every 650 CIRC event).
    QList<TorCircuit> circuits() const;
    // Parses a circuit-status reply — tolerates the "key=" header and
    // the trailing "OK" the reply collector includes.  Static for
    // unit tests.
    static QList<TorCircuit> parseCircuitStatus(const QStringList &lines);
    // TOR05: parses the 'r' line of a ns/id/<fp> reply and returns the
    // relay's IPv4 OR address ("r nick identity digest date time addr
    // orport dirport").  Static for unit tests.
    static QString parseNsAddress(const QStringList &lines);
    // The circuit the UI should surface: the BUILT circuit carrying
    // the most streams, else the newest BUILT PURPOSE=GENERAL, else
    // the newest BUILT at all; -1 when none qualify.
    int displayCircuitId() const;
    // TOR12: whether the snapshot carries a circuit a user stream can
    // ride.  state() == Ready only means the SOCKS listener answers —
    // the first usable exit path can land seconds (or a stretch)
    // after it, and a navigation dispatched into that window is the
    // "first Enter never loads" report, so the navigation gate waits
    // for this rather than `ready` alone.
    bool hasUsableCircuit() const;
    static bool isUsableCircuit(const TorCircuit &circuit);

    // TOR15: SIGNAL NEWNYM — tor marks live circuits dirty and builds
    // clean ones; new streams get a fresh exit/middle while the entry
    // guard persists.  The callback fires with the control reply code
    // (250 on success) or -1 when there is no live control
    // connection.  The replacement circuits land through the CIRC
    // events -> circuitsChanged like any other rotation.  (A plain
    // method, not a slot — moc cannot parse the std::function.)
    void signalNewNym(
            const std::function<void(int code,
                                     const QStringList &lines)> &callback);

public slots:
    void start();
    void stop();    // SIGNAL SHUTDOWN -> terminate -> kill, all bounded
    // TOR04: queues GETINFO circuit-status + stream-status on the
    // control connection (serialized with everything else — no second
    // connection); coalesced while a query is in flight.
    void requestCircuitInfo();

#ifdef AUTOTESTS
    // Test seam: drives the state machine without a live daemon —
    // the bootstrap navigation-gate tests flip Ready/Bootstrapping to
    // exercise the queue and warm-up retry paths.
    void setStateForTesting(State state) { setState(state); }
    // Test seam: installs a circuit snapshot and fires
    // circuitsChanged — the TOR12 warm-up gate tests drive the
    // post-ready cold-circuit window without a live daemon.
    void setCircuitsForTesting(const QList<TorCircuit> &circuits)
    {
        m_circuits = circuits;
        emit circuitsChanged(m_circuits);
    }
#endif

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
    // TOR05: kicks ns/id + ip-to-country lookups for the displayed
    // circuit's uncached hops; fillHopDetails copies cache values
    // into m_circuits and reports whether anything changed.
    void resolveHopCountries();
    bool fillHopDetails();
    void finishHopCountryQuery(const QString &fingerprint,
                               const QString &address,
                               const QString &country);

    QString m_binaryPath;
    QString m_dataDirectory;
    QString m_controlPortFile;
    State m_state;
    QString m_errorString;
    int m_bootstrapProgress;
    QString m_bootstrapSummary;
    QHostAddress m_socksHost;
    quint16 m_socksPort;
    QElapsedTimer m_readyClock;   // TOR18: running only while Ready

    // TOR17: an early daemon exit (or a failed exec) before
    // bootstrap strands the window on the fail-closed proxy — every
    // tab dies at a raw proxy error with nothing behind it.  A
    // transient loader/exec race is worth a bounded self-heal:
    // retryOrFail respawns up to a few times, 1.5s apart, before the
    // failure is declared.
    void retryOrFail(const QString &reason);

    QList<TorCircuit> m_circuits;
    int m_streamCircuitId;          // busiest attached circuit, -1
    bool m_circuitQueryInFlight;
    QTimer *m_circuitRefreshTimer;  // debounces bursts of 650 CIRC lines
    int m_startRetries;             // respawns used in this launch series
    QTimer *m_retryTimer;           // spaces the respawns

    // TOR05: fingerprint -> ISO country code (a cached empty value is
    // a known-unresolvable hop — it is not re-queried).  Circuits
    // rotate every ~10min, so the cache is FIFO-capped via the order
    // list.  TOR08: the parallel OR-address cache shares the order
    // list so both evict together.
    QHash<QString, QString> m_countryCache;
    QHash<QString, QString> m_addressCache;
    QStringList m_countryCacheOrder;
    QSet<QString> m_countryQueriesInFlight;

    QProcess *m_process;
    TorControl *m_control;
    QTimer *m_portFileTimer;
};

Q_DECLARE_METATYPE(TorCircuit)
Q_DECLARE_METATYPE(QList<TorCircuit>)

#endif // TORMANAGER_H
