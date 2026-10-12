import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
PORT = int(sys.argv[1]) if len(sys.argv)>1 else 8898
PAGE = b'<!doctype html><html><body><img src="/img2.png"><a href="/p2">x</a></body></html>'
class H(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_GET(self):
        print("GET", self.path, "Referer:", self.headers.get("Referer"), flush=True)
        body = PAGE if self.path in ("/","/p2") else b"x"
        self.send_response(200)
        self.send_header("Content-Type","text/html" if self.path in ("/","/p2") else "image/png")
        self.send_header("Content-Length",str(len(body)))
        self.end_headers(); self.wfile.write(body)
ThreadingHTTPServer(("127.0.0.1",PORT),H).serve_forever()
