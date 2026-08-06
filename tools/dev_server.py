"""
dev_server.py
Servidor de arquivos estáticos para iteração local em preprocessor.html/posprocessor.html, com
cache desabilitado (Cache-Control: no-store) em toda resposta -- python -m http.server sozinho
não manda esse header, e o navegador cacheia agressivamente os módulos ES (js/*.js importados via
<script type="module">), fazendo edições nesses arquivos parecerem "não aplicadas" mesmo depois de
um refresh normal (só um hard refresh forçava a revalidação).

Uso: python3 dev_server.py [porta] [diretório]
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
