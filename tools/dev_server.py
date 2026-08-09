"""
dev_server.py
Static file server for local iteration on preprocessor.html/posprocessor.html, with caching
disabled (Cache-Control: no-store) on every response -- python -m http.server alone doesn't send
this header, and the browser aggressively caches the ES modules (js/*.js imported via
<script type="module">), making edits to those files look "not applied" even after a normal
refresh (only a hard refresh forced revalidation).

Usage: python3 dev_server.py [port] [directory]
"""
import http.server
import socketserver
import sys


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store, no-cache, must-revalidate')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    directory = sys.argv[2] if len(sys.argv) > 2 else '.'

    def handler(*args, **kwargs):
        NoCacheHandler(*args, directory=directory, **kwargs)

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(('0.0.0.0', port), handler) as httpd:
        print(f"Servindo {directory} em http://0.0.0.0:{port} (cache desabilitado)")
        httpd.serve_forever()
