from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Event

BODY = b'PK\x03\x04' + bytes(range(256)) * 64


class DownloadHandler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', f'http://127.0.0.1:{self.server.server_port}/ok')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if self.path == '/error':
            status, body, size = 404, b'Not found', 9
        elif self.path == '/empty':
            status, body, size = 200, b'', 0
        elif self.path == '/oversized':
            status, body, size = 200, BODY, 8 * 1024 * 1024 * 1024 + 1
        elif self.path == '/truncated':
            status, body, size = 200, BODY, len(BODY) * 2
        elif self.path == '/hold':
            status, body, size = 200, BODY, len(BODY) * 1024
        else:
            status, body, size = 200, BODY, len(BODY)
        self.send_response(status)
        if self.path != '/unknown-size':
            self.send_header('Content-Length', str(size))
        self.end_headers()
        try:
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return
        if self.path in ('/hold', '/oversized'):
            Event().wait()


if __name__ == '__main__':
    server = ThreadingHTTPServer(('127.0.0.1', 0), DownloadHandler)
    print(f'http://127.0.0.1:{server.server_port}/', flush=True)
    while True:
        server.handle_request()
