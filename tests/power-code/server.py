# tests/power-code/server.py PORT LOGFILE WEATHER_JSON (run-events.sh starts it)
# A local stand-in for Mastodon and Open-Meteo: logs each request
import http.server, sys, time, json
LOG = open(sys.argv[2], 'a', buffering=1)
WEATHER = open(sys.argv[3], 'rb').read()
ETAG = 'W/"pro1x-test-1"'
POSTS = b'[]'
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        inm = self.headers.get('If-None-Match')
        if self.path.startswith('/api/v1/timelines/home'):
            if inm == ETAG:
                LOG.write('%.1f feed 304\n' % time.time())
                self.send_response(304); self.send_header('ETag', ETAG); self.end_headers(); return
            LOG.write('%.1f feed 200\n' % time.time())
            body = POSTS; ctype = 'application/json'
            self.send_response(200); self.send_header('ETag', ETAG)
        elif self.path.startswith('/v1/forecast'):
            LOG.write('%.1f weather 200\n' % time.time())
            body = WEATHER; ctype = 'application/json'
            self.send_response(200)
        else:
            LOG.write('%.1f other 404 %s\n' % (time.time(), self.path))
            self.send_response(404); self.end_headers(); return
        self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
