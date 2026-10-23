# A web server for the cookie test: /login sets cookies and redirects to
# /home, which shows the cookies it got; /logout deletes the session.
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_GET(self):
        if self.path.startswith('/login'):
            self.send_response(302)
            self.send_header('Set-Cookie','session=abc123; Path=/; HttpOnly')
            self.send_header('Set-Cookie','theme=dark; Max-Age=3600')
            self.send_header('Set-Cookie','gone=x; Expires=Thu, 01 Jan 1970 00:00:00 GMT')
            self.send_header('Location','/home')
            self.end_headers(); return
        if self.path.startswith('/logout'):
            self.send_response(200)
            self.send_header('Set-Cookie','session=; Max-Age=0; Path=/')
            self.send_header('Content-Type','text/html'); self.end_headers()
            self.wfile.write(b'<title>Out</title><p>bye</p>'); return
        c=self.headers.get('Cookie','none')
        self.send_response(200); self.send_header('Content-Type','text/html'); self.end_headers()
        self.wfile.write(('<title>Home</title><p>cookies: %s</p>'%c).encode())
http.server.HTTPServer(('127.0.0.1',int(sys.argv[1])),H).serve_forever()
