#!/usr/bin/env python3
# Fake `tor` daemon for tst_tormanager — implements just enough of the
# real daemon surface to exercise TorManager end-to-end without a tor
# binary or network access:
#   * parses --DataDirectory / --ControlPortWriteToFile
#   * writes <datadir>/control_auth_cookie (32 bytes) + the
#     control-port file ("PORT=addr:port")
#   * serves the control protocol: AUTHENTICATE (cookie hex),
#     TAKEOWNERSHIP, SETEVENTS, GETINFO, SIGNAL SHUTDOWN
#   * emits asynchronous 650 STATUS_CLIENT BOOTSTRAP events
#   * opens a SOCKS5 listener that completes the RFC 1928 handshake and
#     refuses every CONNECT with rep=0x05 (exits can't reach loopback
#     either — a well-formed refusal proves the protocol)
#   * exits on SIGNAL SHUTDOWN, or on owner disconnect after
#     TAKEOWNERSHIP (mirroring real tor behavior)
#
# MEM07: FAKETOR_SOCKS_RELAY=1 turns the SOCKS listener into a real
# loopback-only relay — CONNECTs to 127.0.0.0/8, [::1] and
# localhost/*.localhost are dialed and piped so page loads through the
# proxy actually commit (the tor-window memory leg needs committed
# documents, not refusals).  Every CONNECT target is appended to
# <DataDirectory>/socks-connect-log so tests can assert traffic really
# rode the tunnel; non-loopback targets are still refused outright so
# the harness can never reach off-box.

import ipaddress
import os
import socket
import sys
import threading

SOCKS_REP = {
    "refuse": b"\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00",
    "ok": b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00",
}

# TOR05: relay directory + geoip answers for the circuit hops below.
# ns/id/<fp> replies carry a 'r' line whose 7th field is the relay's
# OR address; ip-to-country/<addr> maps it to an ISO code.  Every
# ns/id query is appended to <datadir>/ns-query-log so tests can
# assert the fingerprint cache prevents re-queries.  FAKETOR_NOGEOIP=1
# makes every ip-to-country answer "??" like a tor without GeoIPFile.
NS_RELAYS = {
    "AA11AA11AA11AA11AA11AA11AA11AA11AA11AA11": ("GuardOne",
                                               "5.9.80.11"),
    "BB22BB22BB22BB22BB22BB22BB22BB22BB22BB22": ("MiddleTwo",
                                               "185.220.101.4"),
    "CC33CC33CC33CC33CC33CC33CC33CC33CC33CC33": ("ExitThree",
                                               "171.25.193.9"),
    "DD55DD55DD55DD55DD55DD55DD55DD55DD55DD55": ("SoloHop",
                                               "203.0.113.7"),
    # TOR15: the post-NEWNYM middle/exit — the guard persists, the
    # rest of the path rotates like a real daemon would.
    "EE66EE66EE66EE66EE66EE66EE66EE66EE66EE66": ("MiddleFour",
                                               "185.220.102.7"),
    "FF77FF77FF77FF77FF77FF77FF77FF77FF77FF77": ("ExitFour",
                                               "171.25.193.25"),
}
GEOIP = {"5.9.80.11": "de", "185.220.101.4": "nl",
         "171.25.193.9": "is", "203.0.113.7": "us",
         "185.220.102.7": "ch", "171.25.193.25": "us"}


def main():
    args = sys.argv[1:]
    data_dir = "."
    port_file = None
    i = 0
    while i < len(args):
        if args[i] == "--DataDirectory":
            data_dir = args[i + 1]
            i += 1
        elif args[i] == "--ControlPortWriteToFile":
            port_file = args[i + 1]
            i += 1
        i += 1

    os.makedirs(data_dir, exist_ok=True)
    cookie = bytes(range(32))
    cookie_path = os.path.join(data_dir, "control_auth_cookie")
    with open(cookie_path, "wb") as f:
        f.write(cookie)
    os.chmod(cookie_path, 0o600)

    control = socket.socket()
    control.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    control.bind(("127.0.0.1", 0))
    control.listen(1)
    cport = control.getsockname()[1]

    socks = socket.socket()
    socks.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    socks.bind(("127.0.0.1", 0))
    socks.listen(5)
    sport = socks.getsockname()[1]

    if port_file:
        with open(port_file, "w") as f:
            f.write("PORT=127.0.0.1:%d\n" % cport)

    sys.stderr.write(
        "[notice] FakeTor: Socks listener listening on port %d.\n"
        % sport)
    sys.stderr.write(
        "[notice] FakeTor: Control listener listening on port %d.\n"
        % cport)
    sys.stderr.flush()

    state = {"progress": 10, "owner": False, "nym": 0, "circ_id": 3}
    no_geoip = os.environ.get("FAKETOR_NOGEOIP") == "1"
    socks_relay = os.environ.get("FAKETOR_SOCKS_RELAY") == "1"
    connect_log = os.path.join(data_dir, "socks-connect-log")

    def recv_exact(conn, count):
        data = b""
        while len(data) < count:
            chunk = conn.recv(count - len(data))
            if not chunk:
                raise OSError("peer hung up")
            data += chunk
        return data

    def local_addresses():
        # Every address this machine answers for — loopback plus own
        # interface IPs.  Chromium's implicit proxy bypass covers
        # loopback only, so the memcycle fixture listens on an own
        # non-loopback address to still cross the tunnel; allowing the
        # whole local set keeps the relay strictly on-box (a LAN host
        # or external target is still refused outright).
        addrs = {"127.0.0.1", "::1"}
        try:
            import fcntl
            import struct
            for _, name in socket.if_nameindex():
                try:
                    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    req = struct.pack("256s", name[:15].encode())
                    addrs.add(socket.inet_ntoa(
                        fcntl.ioctl(s.fileno(), 0x8915, req)[20:24]))
                    s.close()
                except OSError:
                    pass
        except ImportError:
            pass
        return addrs

    own_addrs = local_addresses()

    def relay_target_allowed(host):
        # Domain-form targets come through for IP literals too (Chromium
        # prefers ATYP=3) — accept by name, then hard-verify every
        # resolved address is one of ours so a lookalike name can't
        # pivot the fixture off-box.
        lowered = host.lower().strip("[]")
        if lowered == "localhost" or lowered.endswith(".localhost"):
            return True
        try:
            return str(ipaddress.ip_address(lowered)) in own_addrs
        except ValueError:
            pass
        try:
            infos = socket.getaddrinfo(lowered, None)
        except OSError:
            return False
        return bool(infos) and all(str(ipaddress.ip_address(
            i[4][0].split("%")[0])) in own_addrs for i in infos)

    def pipe(upstream, conn):
        def fwd(src, dst):
            try:
                while True:
                    data = src.recv(65536)
                    if not data:
                        break
                    dst.sendall(data)
            except OSError:
                pass
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass
        relay = threading.Thread(target=fwd, args=(conn, upstream),
                                 daemon=True)
        relay.start()
        fwd(upstream, conn)
        relay.join(30)
        for sock in (conn, upstream):
            try:
                sock.close()
            except OSError:
                pass

    def socks_session(conn):
        try:
            conn.settimeout(15)
            head = recv_exact(conn, 2)
            if head[:1] != b"\x05":
                return
            recv_exact(conn, head[1])       # method list
            conn.sendall(b"\x05\x00")       # no-auth accepted
            req = recv_exact(conn, 4)
            if req[:3] != b"\x05\x01\x00":
                conn.sendall(SOCKS_REP["refuse"])
                return
            atyp = req[3]
            if atyp == 1:
                host = socket.inet_ntoa(recv_exact(conn, 4))
            elif atyp == 3:
                host = recv_exact(
                    conn, recv_exact(conn, 1)[0]).decode("utf-8",
                                                         "replace")
            elif atyp == 4:
                host = socket.inet_ntop(socket.AF_INET6,
                                        recv_exact(conn, 16))
            else:
                conn.sendall(SOCKS_REP["refuse"])
                return
            port = int.from_bytes(recv_exact(conn, 2), "big")
            with open(connect_log, "a") as f:
                f.write("%s:%d\n" % (host, port))
            if not socks_relay or not relay_target_allowed(host):
                conn.sendall(SOCKS_REP["refuse"])
                return
            try:
                upstream = socket.create_connection((host, port),
                                                    timeout=10)
            except OSError:
                conn.sendall(SOCKS_REP["refuse"])
                return
            conn.sendall(SOCKS_REP["ok"])
            conn.settimeout(30)
            pipe(upstream, conn)
        except OSError:
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    # TOR15: the circuit-status body served before and after SIGNAL
    # NEWNYM — same guard fingerprint, rotated middle+exit, fresh
    # CircID.  TOR13: the dirty pre-NEWNYM circuit stays listed while
    # its streams are alive — real tor marks circuits dirty rather
    # than closing them — and its streams stay pinned on it, which is
    # exactly why the display kept showing the old hops.
    def circuit_lines():
        old_circuit = ("3 BUILT "
                       "$AA11AA11AA11AA11AA11AA11AA11AA11AA11AA11"
                       "~GuardOne,"
                       "$BB22BB22BB22BB22BB22BB22BB22BB22BB22BB22"
                       "=MiddleTwo,"
                       "$CC33CC33CC33CC33CC33CC33CC33CC33CC33CC33"
                       "~ExitThree "
                       "BUILD_FLAGS=NEED_CAPACITY PURPOSE=GENERAL "
                       "TIME_CREATED=2026-10-09T00:00:00.000000")
        if state["nym"] == 0:
            return [old_circuit]
        return [old_circuit,
                "%d BUILT "
                "$AA11AA11AA11AA11AA11AA11AA11AA11AA11AA11"
                "~GuardOne,"
                "$EE66EE66EE66EE66EE66EE66EE66EE66EE66EE66"
                "~MiddleFour,"
                "$FF77FF77FF77FF77FF77FF77FF77FF77FF77FF77"
                "~ExitFour "
                "BUILD_FLAGS=NEED_CAPACITY PURPOSE=GENERAL "
                "TIME_CREATED=2026-10-09T00:00:00.000000"
                % state["circ_id"]]

    def socks_worker():
        while True:
            try:
                conn, _ = socks.accept()
            except OSError:
                return
            threading.Thread(target=socks_session, args=(conn,),
                             daemon=True).start()

    threading.Thread(target=socks_worker, daemon=True).start()

    conn, _ = control.accept()
    conn.settimeout(None)
    buf = b""

    def reply(s):
        conn.sendall(s.encode("utf-8"))

    def events():
        for progress in (50, 100):
            import time
            time.sleep(0.15)
            tag = "done" if progress == 100 else "handshake_done"
            summary = "Done" if progress == 100 else "Handshake done"
            try:
                reply('650 STATUS_CLIENT NOTICE BOOTSTRAP '
                      'PROGRESS=%d TAG=%s SUMMARY="%s"\r\n'
                      % (progress, tag, summary))
            except OSError:
                return
        state["progress"] = 100
        # A circuit event arriving after bootstrap exercises the
        # debounced circuit-status refresh path (TOR04).
        try:
            reply('650 CIRC 4 BUILT '
                  '$EE44EE44EE44EE44EE44EE44EE44EE44EE44EE44~LoneHop '
                  'PURPOSE=GENERAL\r\n')
        except OSError:
            return

    events_started = False
    while True:
        try:
            data = conn.recv(4096)
        except OSError:
            data = b""
        if not data:
            if state["owner"]:
                sys.exit(0)  # TAKEOWNERSHIP: owner died, exit like tor
            break
        buf += data
        while b"\r\n" in buf:
            line, buf = buf.split(b"\r\n", 1)
            cmd = line.decode("utf-8", "replace")
            if cmd.startswith("AUTHENTICATE"):
                if cmd.split(" ", 1)[1].strip().strip('"') == \
                        cookie.hex():
                    reply("250 OK\r\n")
                else:
                    reply("515 Authentication failed: bad cookie\r\n")
            elif cmd == "TAKEOWNERSHIP":
                state["owner"] = True
                reply("250 OK\r\n")
            elif cmd.startswith("SETEVENTS"):
                reply("250 OK\r\n")
                if not events_started:
                    events_started = True
                    threading.Thread(target=events, daemon=True).start()
            elif cmd.startswith("GETINFO status/bootstrap-phase"):
                reply("250-status/bootstrap-phase=NOTICE BOOTSTRAP "
                      'PROGRESS=%d TAG=done SUMMARY="Done"\r\n'
                      % state["progress"])
                reply("250 OK\r\n")
            elif cmd.startswith("GETINFO net/listeners/socks"):
                reply('250-net/listeners/socks="127.0.0.1:%d"\r\n'
                      % sport)
                reply("250 OK\r\n")
            elif cmd.startswith("GETINFO circuit-status"):
                # TOR13: every request is logged so tests can assert
                # the circuit dropdown's open-menu refresh stays
                # bounded instead of looping one query per reply.
                with open(os.path.join(data_dir,
                                       "circuit-status-log"),
                          "a") as f:
                    f.write("circuit-status\n")
                reply("250+circuit-status=\r\n")
                for line in circuit_lines():
                    reply(line + "\r\n")
                reply("5 EXTENDED "
                      "$DD55DD55DD55DD55DD55DD55DD55DD55DD55DD55"
                      "~SoloHop PURPOSE=HS_CLIENT_HSDIR\r\n")
                reply(".\r\n250 OK\r\n")
            elif cmd.startswith("GETINFO ns/id/"):
                fp = cmd.split("/", 2)[2].upper()
                if fp in NS_RELAYS:
                    nick, addr = NS_RELAYS[fp]
                    reply("250+ns/id/%s=\r\n" % fp)
                    reply("r %s aGVsbG8 aGVsbG8gd29ybGQg "
                          "2026-10-09 00:00:00 %s 9001 0\r\n"
                          % (nick, addr))
                    reply("s Running Stable Valid V2Dir\r\n")
                    reply(".\r\n250 OK\r\n")
                    with open(os.path.join(data_dir,
                                           "ns-query-log"),
                              "a") as f:
                        f.write(fp + "\n")
                else:
                    reply("552 Unrecognized key\r\n")
            elif cmd.startswith("GETINFO ip-to-country/"):
                addr = cmd.split("/", 1)[1]
                cc = "??" if no_geoip else GEOIP.get(addr, "??")
                reply("250-ip-to-country/%s=%s\r\n" % (addr, cc))
                reply("250 OK\r\n")
            elif cmd.startswith("GETINFO stream-status"):
                reply("250+stream-status=\r\n")
                # TOR13: streams attached before NEWNYM keep riding
                # the dirty circuit — the busiest-stream pick alone
                # would surface circuit 3 forever.  Only a fresh
                # stream (the reload the UI now drives) lands on the
                # replacement.
                reply("8 SUCCEEDED 3 127.0.0.1:80\r\n")
                reply("9 SUCCEEDED 3 127.0.0.1:443\r\n")
                if state["nym"] > 0:
                    reply("11 SUCCEEDED %d 127.0.0.1:8080\r\n"
                          % state["circ_id"])
                reply("10 NEW 5 127.0.0.1:80\r\n")
                reply(".\r\n250 OK\r\n")
            elif cmd == "SIGNAL NEWNYM":
                # TOR15: rotate to clean circuits — new CircID, new
                # middle+exit, same guard — then announce it over the
                # CIRC event channel like a real daemon does.  The id
                # stays above every prior one (ids are monotonic in a
                # real daemon; SoloHop sits at 5).
                with open(os.path.join(data_dir, "signal-log"),
                          "a") as f:
                    f.write("NEWNYM\n")
                state["nym"] += 1
                state["circ_id"] = 5 + state["nym"]
                reply("250 OK\r\n")
                try:
                    reply("650 CIRC %d BUILT "
                          "$AA11AA11AA11AA11AA11AA11AA11AA11AA11AA11"
                          "~GuardOne,"
                          "$EE66EE66EE66EE66EE66EE66EE66EE66EE66EE66"
                          "~MiddleFour,"
                          "$FF77FF77FF77FF77FF77FF77FF77FF77FF77FF77"
                          "~ExitFour PURPOSE=GENERAL\r\n"
                          % state["circ_id"])
                except OSError:
                    return
            elif cmd == "SIGNAL SHUTDOWN":
                reply("250 OK\r\n250 closing connection\r\n")
                sys.exit(0)
            elif cmd == "SIGNAL TERM":
                reply("250 OK\r\n")
                sys.exit(0)
            elif cmd:
                reply("552 Unrecognized command\r\n")

    sys.exit(0)


if __name__ == "__main__":
    main()
