import http.server, urllib.parse, sys, time
PORT = int(sys.argv[1])
BASE = f"http://127.0.0.1:{PORT}"
FORM = """<form action="/lite/" method="post"><input class="query" type="text" size="40" name="q" value="%s" autofocus>
<input class="submit" type="submit" value="Search"><select class="submit" name="kl"><option value="" >All Regions</option><option value="us-en">US (English)</option></select>
<input type="hidden" name="df" value=""></form>"""
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *a): sys.stderr.write("REQ " + (fmt % a) + "\n"); sys.stderr.flush()
    def send(self, body, ctype="text/html; charset=utf-8"):
        b = body.encode(); self.send_response(200); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/lite/":
            self.send("<html><head><title>DuckDuckGo</title></head><body><h1>DuckDuckGo</h1>" + FORM % "" + "</body></html>")
        elif u.path == "/l/":
            t = urllib.parse.parse_qs(u.query)["uddg"][0]
            # same shape as DuckDuckGo's real /l/ interstitial
            self.send(f'<html><head><meta name="referrer" content="origin"><meta name="robots" content="noindex, nofollow"><meta http-equiv="refresh" content="0; url={t}"></head><body><script language="JavaScript">function ffredirect(){{window.location.replace(\'{t}\');}}setTimeout(\'ffredirect()\',100);</script></body></html>')
        elif u.path == "/jsonly":
            self.send(f"<html><body><script>window.location.href = '{BASE}/target.html?via=js';</script></body></html>")
        elif u.path == "/dupsel":
            self.send('<html><head><title>dupsel</title></head><body><form action="/echo" method="get"><input name="q" value="x">'
                      '<select name="k"><option value="a">A</option><option value="1">--</option><option value="2">--</option>'
                      '<option value="b" selected>B</option></select><input type="submit" value="Go"></form></body></html>')
        elif u.path == "/multipart":
            self.send('<html><head><title>mp</title></head><body><form action="/echo" method="post" enctype="multipart/form-data">'
                      '<input name="q" value="hello"><textarea name="t">l1\nl2</textarea><input type="submit" name="go" value="Send"></form></body></html>')
        elif u.path == "/latin1":
            b = '<html><head><title>latin1</title></head><body><form action="/echo"><input name="q" value="caf\xe9 \u20ac"><input type="submit"></form></body></html>'.encode("cp1252")
            self.send_response(200); self.send_header("Content-Type", "text/html; charset=windows-1252")
            self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        elif u.path == "/badaction":
            self.send('<html><head><title>badaction</title></head><body><form action="http:"><input name="q" value="x"><input type="submit" value="Bad"></form></body></html>')
        elif u.path == "/choice":
            opts = "".join(f'<option value="{i}">Option {i}</option>' for i in range(30))
            self.send(f'<html><head><title>choice</title></head><body><form action="/echo"><select name="c">{opts}</select></form><p>choice page</p></body></html>')
        elif u.path == "/slow":
            time.sleep(2.5)
            self.send('<html><head><title>slow</title></head><body><form action="/echo"><input name="z"></form><p>slow page loaded</p></body></html>')
        elif u.path == "/loop1":
            self.send('<html><head><meta http-equiv="refresh" content="0; url=/loop2"></head><body></body></html>')
        elif u.path == "/loop2":
            self.send('<html><head><meta http-equiv="refresh" content="0; url=/loop1"></head><body></body></html>')
        elif u.path == "/bigjs":
            self.send("<html><body><script>location.href='/target.html?big=" + "x" * 120000 + "';</script></body></html>")
        elif u.path == "/geo":
            self.send("<html><body><script>var geolocation = 'Paris';</script>short</body></html>")
        elif u.path == "/echo":
            self.send(f"<html><head><title>echo GET</title></head><body><h1>ECHO GET</h1><p>{u.query}</p></body></html>")
        elif u.path in ("/target.html", "/direct.html"):
            self.send(f"<html><head><title>Target {u.path} {u.query}</title></head><body><h1>Arrived at {u.path}</h1><p>query={u.query}</p></body></html>")
        else:
            self.send_response(404); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n).decode("utf-8", "replace")
        sys.stderr.write(f"POSTBODY {self.path} {self.headers.get('Content-Type')} {body!r}\n"); sys.stderr.flush()
        if self.path.startswith("/echo"):
            self.send("<html><head><title>echo POST</title></head><body><h1>ECHO POST</h1></body></html>"); return
        q = urllib.parse.parse_qs(body).get("q", [""])[0]
        enc = urllib.parse.quote(f"{BASE}/target.html?from=meta", safe="")
        enc2 = urllib.parse.quote(f"{BASE}/direct.html?from=uddg", safe="")
        self.send(f"""<html><head><title>{q} at DuckDuckGo</title></head><body>{FORM % q}
<h1><a rel="nofollow" href="//127.0.0.1:{PORT}/l/?uddg={enc}&amp;rut=abc" class='result-link'>FIRST RESULT VIA INTERSTITIAL</a></h1>
<h1><a rel="nofollow" href="https://duckduckgo.com/l/?uddg={enc2}&amp;rut=def" class='result-link'>SECOND RESULT VIA UDDG</a></h1>
<h1><a href="/jsonly">THIRD RESULT JS ONLY</a></h1></body></html>""")
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
