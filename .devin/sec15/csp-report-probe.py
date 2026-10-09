#!/usr/bin/env python3
# CSP report-uri probe for SEC16B: does a CSP violation report reach the
# server under the app's PING01 CspReport block?  Serves a violating page
# and logs every request.  Usage: python3 csp_probe.py <port>
import sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
MODE = sys.argv[2] if len(sys.argv) > 2 else "report-uri"

PAGE = b"""<!doctype html><html><body>
<img src="/img.png">
<script src="/evil.js"></script>
</body></html>"""

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def _log(self, tag):
        print(f"GOT {tag} {self.path} ct={self.headers.get('Content-Type')}",
              flush=True)
    def do_GET(self):
        self._log("GET")
        if self.path == "/":
            body = PAGE
            self.send_response(200)
            if MODE == "report-uri":
                self.send_header("Content-Security-Policy",
                                 "default-src 'none'; report-uri /r")
            else:
                self.send_header(
                    "Content-Security-Policy",
                    "default-src 'none'; report-to rep")
                self.send_header(
                    "Report-To",
                    '{"group":"rep","max_age":60,'
                    '"endpoints":[{"url":"http://127.0.0.1:%d/r"}]}' % PORT)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(200)
            self.send_header("Content-Length", "3")
            self.end_headers()
            self.wfile.write(b"x")
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        self._log("POST")
        print("  body:", body[:200], flush=True)
        self.send_response(204)
        self.end_headers()

ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
