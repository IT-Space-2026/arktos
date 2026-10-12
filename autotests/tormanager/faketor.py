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

import os
import socket
import sys
import threading

SOCKS_REP = {
    "refuse": b"\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00",
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
}
GEOIP = {"5.9.80.11": "de", "185.220.101.4": "nl",
         "171.25.193.9": "is", "203.0.113.7": "us"}


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

    state = {"progress": 10, "owner": False}
    no_geoip = os.environ.get("FAKETOR_NOGEOIP") == "1"

    def socks_worker():
        while True:
            try:
                conn, _ = socks.accept()
            except OSError:
                return
            try:
                conn.settimeout(10)
                if conn.recv(64)[:1] != b"\x05":
                    conn.close()
                    continue
                conn.sendall(b"\x05\x00")  # no-auth accepted
                req = conn.recv(300)
                if req[:3] == b"\x05\x01\x00":
                    conn.sendall(SOCKS_REP["refuse"])
                conn.close()
            except OSError:
                pass

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
                reply("250+circuit-status=\r\n")
                reply("3 BUILT "
                      "$AA11AA11AA11AA11AA11AA11AA11AA11AA11AA11"
                      "~GuardOne,"
                      "$BB22BB22BB22BB22BB22BB22BB22BB22BB22BB22"
                      "=MiddleTwo,"
                      "$CC33CC33CC33CC33CC33CC33CC33CC33CC33CC33"
                      "~ExitThree "
                      "BUILD_FLAGS=NEED_CAPACITY PURPOSE=GENERAL "
                      "TIME_CREATED=2026-10-09T00:00:00.000000\r\n")
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
                reply("8 SUCCEEDED 3 127.0.0.1:80\r\n")
                reply("9 SUCCEEDED 3 127.0.0.1:443\r\n")
                reply("10 NEW 5 127.0.0.1:80\r\n")
                reply(".\r\n250 OK\r\n")
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
