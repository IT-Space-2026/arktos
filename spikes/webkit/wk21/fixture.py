#!/usr/bin/env python3
# WK21 audit fixture — minimal SOCKS5 server + marker HTTP server.
#
#   python3 fixture.py <workdir>
#
# <workdir>/ports.env   (written) SOCKS_PORT=..  HTTP_PORT=..
# <workdir>/socks.log   CONNECT lines: "CONNECT atyp=3 target=host port=80"
#                       atyp 3 = client sent a DOMAIN name (proxy resolves
#                       = remote DNS); atyp 1/4 = client sent a resolved IP
#                       (it looked the name up locally first).
# <workdir>/http.log    "GET /path Host=hdr" per hit — proves which bytes
#                       were fetched through the proxy tunnel.
#
# Every DOMAIN CONNECT is tunnelled to the local HTTP fixture regardless
# of the name — so http://<anything>.test/ loads iff it rides the SOCKS
# proxy with the hostname intact. IP CONNECTs get a real outbound dial
# (short timeout) — a proxied IP request is not itself a leak, but the
# line tells us who resolved it. UDP ASSOCIATE is logged and refused.
#
# No dependencies beyond the stdlib; loopback only.

import os
import socket
import struct
import sys
import threading

WORKDIR = sys.argv[1]
SOCKS_LOG = os.path.join(WORKDIR, "socks.log")
HTTP_LOG = os.path.join(WORKDIR, "http.log")
PORTS_ENV = os.path.join(WORKDIR, "ports.env")

HTTP_PORT = 0  # filled in after bind; SOCKS thread reads it


def log(path, line):
    with open(path, "a") as f:
        f.write(line + "\n")


PAGE = (
    "<!doctype html><title>WK21-FIXTURE-OK</title>"
    # DNS-prefetch of a deliberately unresolvable name — a local
    # getaddrinfo for it is only explainable as browser prefetch.
    '<link rel="dns-prefetch" href="//{prefetch}">'
    # A same-proxy subresource on a DIFFERENT fake host.
    '<img src="http://{img}/px.png">'
    "<body>wk21 fixture</body>"
).encode()


def http_conn(c, addr):
    try:
        c.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 16384:
            chunk = c.recv(4096)
            if not chunk:
                break
            data += chunk
        if not data:
            return
        reqline = data.split(b"\r\n", 1)[0].decode("latin1", "replace")
        host = ""
        for line in data.split(b"\r\n")[1:]:
            if line.lower().startswith(b"host:"):
                host = line.split(b":", 1)[1].strip().decode("latin1", "replace")
        log(HTTP_LOG, "%s Host=%s from=%s:%d"
            % (reqline, host, addr[0], addr[1]))
        path = reqline.split(" ")[1] if " " in reqline else "/"
        if path.endswith(".png"):
            # 1x1 transparent png
            body = bytes.fromhex(
                "89504e470d0a1a0a0000000d4948445200000001000000010806000"
                "01f15c4890000000d49444154789c626001000000ffff0300000600"
                "0557bfabd40000000049454e44ae426082")
            ctype = "image/png"
        elif path.endswith(".json"):
            body = b'{"ok":true}'
            ctype = "application/json"
        else:
            body = PAGE.replace(b"{prefetch}",
                                os.environb.get(b"WK21_PF", b"pf.invalid"))
            body = body.replace(b"{img}",
                                os.environb.get(b"WK21_IMG", b"img.invalid"))
            ctype = "text/html"
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: " + ctype.encode()
                  + b"\r\nContent-Length: " + str(len(body)).encode()
                  + b"\r\nConnection: close\r\n\r\n" + body)
    except Exception as e:
        log(HTTP_LOG, "ERR %s" % e)
    finally:
        try:
            c.close()
        except Exception:
            pass


def http_server(port_holder):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(16)
    port_holder.append(s.getsockname()[1])
    while True:
        try:
            c, a = s.accept()
        except Exception:
            return
        threading.Thread(target=http_conn, args=(c, a), daemon=True).start()


def pipe(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except Exception:
        pass
    try:
        b.shutdown(socket.SHUT_WR)
    except Exception:
        pass


def recv_exact(c, n):
    d = b""
    while len(d) < n:
        chunk = c.recv(n - len(d))
        if not chunk:
            raise EOFError
        d += chunk
    return d


def socks_conn(c, addr):
    upstream = None
    try:
        c.settimeout(15)
        ver = recv_exact(c, 1)[0]
        nmethods = recv_exact(c, 1)[0]
        recv_exact(c, nmethods)
        if ver != 5:
            log(SOCKS_LOG, "BADVER %d" % ver)
            c.close()
            return
        c.sendall(b"\x05\x00")  # no-auth
        ver, cmd, _rsv, atyp = struct.unpack("!BBBB", recv_exact(c, 4))
        if atyp == 1:
            target = socket.inet_ntoa(recv_exact(c, 4))
        elif atyp == 3:
            ln = recv_exact(c, 1)[0]
            target = recv_exact(c, ln).decode("latin1", "replace")
        elif atyp == 4:
            target = socket.inet_ntop(socket.AF_INET6, recv_exact(c, 16))
        else:
            log(SOCKS_LOG, "BADATYP %d" % atyp)
            c.close()
            return
        port = struct.unpack("!H", recv_exact(c, 2))[0]
        cmdname = {1: "CONNECT", 2: "BIND", 3: "UDP-ASSOCIATE"}.get(
            cmd, "CMD%d" % cmd)
        log(SOCKS_LOG, "%s atyp=%d target=%s port=%d from=%s:%d"
            % (cmdname, atyp, target, port, addr[0], addr[1]))
        if cmd != 1:
            # BIND/UDP-ASSOCIATE: refuse (0x07 = command not supported)
            c.sendall(b"\x05\x07\x00\x01" + b"\x00" * 6)
            return
        # Route: domain names all map to the local HTTP fixture; IP
        # literals get a real dial (loopback or outward, 4s timeout).
        if atyp == 3:
            upstream = socket.create_connection(
                ("127.0.0.1", HTTP_PORT), timeout=4)
        else:
            upstream = socket.create_connection((target, port), timeout=4)
        # success reply: 05 00 00 01 <bnd addr> <bnd port>
        c.sendall(b"\x05\x00\x00\x01" + socket.inet_aton("127.0.0.1")
                  + struct.pack("!H", port))
        c.settimeout(None)
        upstream.settimeout(None)
        threading.Thread(target=pipe, args=(upstream, c),
                         daemon=True).start()
        pipe(c, upstream)
    except Exception as e:
        log(SOCKS_LOG, "ERR %s" % e)
        try:
            # general failure reply so the client isn't left hanging
            c.sendall(b"\x05\x05\x00\x01" + b"\x00" * 6)
        except Exception:
            pass
    finally:
        try:
            c.close()
        except Exception:
            pass
        if upstream:
            try:
                upstream.close()
            except Exception:
                pass


def main():
    global HTTP_PORT
    holder = []
    threading.Thread(target=http_server, args=(holder,),
                     daemon=True).start()
    while not holder:
        import time
        time.sleep(0.02)
    HTTP_PORT = holder[0]

    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(16)
    socks_port = s.getsockname()[1]

    with open(PORTS_ENV, "w") as f:
        f.write("SOCKS_PORT=%d\nHTTP_PORT=%d\n" % (socks_port, HTTP_PORT))
    sys.stderr.write("fixture up: socks=%d http=%d\n"
                     % (socks_port, HTTP_PORT))
    sys.stderr.flush()

    while True:
        try:
            c, a = s.accept()
        except Exception:
            return
        threading.Thread(target=socks_conn, args=(c, a),
                         daemon=True).start()


if __name__ == "__main__":
    main()
