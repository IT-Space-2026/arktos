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
