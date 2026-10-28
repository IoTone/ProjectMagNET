#!/usr/bin/env python3
"""A fake "click OK" captive portal, for testing the Sentinel's auto-accept.

Imitates a typical hotel portal: the check page redirects to a welcome page
that sets a session cookie and shows two forms (a language picker and the
real "I agree" form). The accept only counts when it carries the cookie, the
hidden token and the ticked terms box; after that the check page answers 204.

    python3 tools/fake_portal.py                # listens on :8099
    # on the Stick's console:
    net-check-url           then  http://<this-mac-ip>:8099/generate_204
    net-check               then  net-stat   ->  open — portal accepted automatically
    net-check-url           then  -     (back to the real check page)

--mode email makes the portal ask for an email address, which the Sentinel
must refuse to fill in (it should report "login needed" instead).
"""

import argparse
import http.server
import urllib.parse

state = {"accepted": False, "mode": "click"}

WELCOME = """<!doctype html><html><head><title>Hotel WiFi</title></head><body>
<h1>Welcome to Hotel WiFi</h1>
<form action="/lang"><select name="l"><option>en</option></select><input type=submit value=Go></form>
<form method="POST" action="accept">
  <input type="hidden" name="token" value="t0k&amp;3n">
  {extra}
  <label><input type="checkbox" name="tos" value="yes"> I agree to the terms of use</label>
  <button type="submit" name="action" value="connect">Connect</button>
</form></body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"{self.address_string()}  {fmt % args}", flush=True)

    def send(self, code, body=b"", headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/generate_204":
            if state["accepted"]:
                return self.send(204)
            return self.send(302, headers=[("Location", "/portal/welcome?ref=check")])
        if path == "/portal/welcome":
            extra = '<input type="email" name="email">' if state["mode"] == "email" else ""
            body = WELCOME.format(extra=extra).encode()
            return self.send(200, body, [("Content-Type", "text/html"),
                                         ("Set-Cookie", "sid=abc123; Path=/; HttpOnly")])
        if path == "/portal/done":
            return self.send(200, b"<p>You're online.</p>", [("Content-Type", "text/html")])
        if path == "/reset":
            state["accepted"] = False
            return self.send(200, b"reset\n")
        self.send(404)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        n = int(self.headers.get("Content-Length", 0))
        form = urllib.parse.parse_qs(self.rfile.read(n).decode())
        cookie_ok = "sid=abc123" in (self.headers.get("Cookie") or "")
        print(f"    form={form} cookie_ok={cookie_ok}", flush=True)
        if (path == "/portal/accept" and cookie_ok and form.get("tos") == ["yes"]
                and form.get("token") == ["t0k&3n"] and form.get("action") == ["connect"]):
            state["accepted"] = True
            return self.send(302, headers=[("Location", "/portal/done")])
        self.send(403, b"<p>Please accept the terms.</p>", [("Content-Type", "text/html")])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--mode", choices=["click", "email"], default="click")
    a = ap.parse_args()
    state["mode"] = a.mode
    print(f"fake portal on :{a.port} ({a.mode})", flush=True)
    http.server.ThreadingHTTPServer(("", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
