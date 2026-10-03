#!/usr/bin/env python3
"""A miniature content site used by the end-to-end tests.

It is deliberately small but exercises the paths that matter:

  /                     search results (paginated, 2 pages)
  /book/<id>            detail page with a chapter list
  /read/<id>/<n>        a chapter
  /redirect/<n>         an N-hop redirect chain
  /chunked              a chunked-transfer-encoded response
  /cookie/set           sets cookies; /cookie/check verifies they came back
  /slow?ms=N            a deliberately slow response (deadline tests)
  /big?kb=N             a large response (frame limit tests)
  /404                  a missing page

Run standalone:  python3 content_site.py [port]
"""

from __future__ import annotations

import http.server
import json
import socket
import sys
import threading
import time
import urllib.parse

BOOKS = [
    {"id": 1, "title": "Solo Leveling", "author": "Chugong",
     "intro": "Ten years after the portals opened, the weakest hunter gets a second chance.",
     "chapters": ["Chapter 1 - The Weakest Hunter",
                  "Chapter 2 - Double Dungeon",
                  "Chapter 3 - Awakening"]},
    {"id": 2, "title": "Omniscient Reader", "author": "Sing Shong",
     "intro": "The novel he alone had read becomes reality.",
     "chapters": ["Chapter 1 - Prologue",
                  "Chapter 2 - The First Scenario"]},
    {"id": 3, "title": "The Beginning After The End", "author": "TurtleMe",
     "intro": "A king is reborn into a world of magic.",
     "chapters": ["Chapter 1 - Rebirth"]},
]

PAGE_SIZE = 2


def search_page(query: str, page: int) -> str:
    start = (page - 1) * PAGE_SIZE
    matches = [b for b in BOOKS
               if not query or query.lower() in b["title"].lower()]
    chunk = matches[start:start + PAGE_SIZE]
    rows = "\n".join(
        f'<div class="book">'
        f'<h3 class="title"><a href="/book/{b["id"]}">{b["title"]}</a></h3>'
        f'<span class="author">{b["author"]}</span>'
        f'<img class="cover" src="/img/{b["id"]}.jpg">'
        f'</div>'
        for b in chunk
    )
    nxt = '<a class="next" href="?page=%d">next</a>' % (page + 1) if start + PAGE_SIZE < len(matches) else ""
    return (f"<!doctype html><html><body>"
            f'<div id="results">{rows}</div>{nxt}'
            f"</body></html>")


def book_page(book_id: int) -> str:
    b = next((x for x in BOOKS if x["id"] == book_id), None)
    if b is None:
        return ""
    # Chapter numbers are 1-based in the URLs, like every real reading site.
    chapters = "\n".join(
        f'<li><a href="/read/{b["id"]}/{i + 1}">{name}</a>'
        f'<span class="date">2026-01-0{i + 1}</span></li>'
        for i, name in enumerate(b["chapters"])
    )
    return (f"<!doctype html><html><body>"
            f'<h1 class="bookname">{b["title"]}</h1>'
            f'<span class="writer">{b["author"]}</span>'
            f'<img class="cover" src="/img/{b["id"]}.jpg">'
            f'<div class="intro">{b["intro"]}</div>'
            f'<ul class="chapters">{chapters}</ul>'
            f"</body></html>")


def chapter_page(book_id: int, n: int) -> str:
    b = next((x for x in BOOKS if x["id"] == book_id), None)
    if b is None or n <= 0 or n > len(b["chapters"]):
        return ""
    title = b["chapters"][n - 1]
    return (f"<!doctype html><html><body>"
            f"<h2>{title}</h2>"
            f'<div id="content">'
            f"<p>Paragraph one of {title}.</p>"
            f"<p>Paragraph two, with an &amp; entity and a &lt;tag&gt; in it.</p>"
            f"</div>"
            f'<a class="next" href="/read/{book_id}/{n + 1}">next chapter</a>'
            f"</body></html>")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # silence
        pass

    def _send(self, body: bytes, status: int = 200, ctype: str = "text/html; charset=utf-8",
              extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for name, value in (extra_headers or []):
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, obj, status: int = 200):
        self._send(json.dumps(obj).encode(), status, "application/json")

    def do_GET(self):  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        qs = urllib.parse.parse_qs(parsed.query)

        if path == "/":
            page = int(qs.get("page", ["1"])[0])
            query = qs.get("q", [""])[0]
            return self._send(search_page(query, page).encode())

        if path.startswith("/book/"):
            body = book_page(int(path.rsplit("/", 1)[1]))
            return self._send(body.encode(), 200 if body else 404)

        if path.startswith("/read/"):
            parts = path.strip("/").split("/")
            body = chapter_page(int(parts[1]), int(parts[2]))
            return self._send(body.encode(), 200 if body else 404)

        if path == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for piece in (b"hello ", b"chunked ", b"world\n"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
            return

        if path == "/cookie/set":
            return self._send(b"set", extra_headers=[
                ("Set-Cookie", "session=abc123; Path=/; HttpOnly"),
                ("Set-Cookie", "theme=dark; Path=/"),
            ])

        if path == "/cookie/check":
            got = self.headers.get("Cookie", "")
            ok = "session=abc123" in got and "theme=dark" in got
            return self._send_json({"ok": ok, "cookie": got})

        if path == "/slow":
            ms = int(qs.get("ms", ["1000"])[0])
            time.sleep(ms / 1000.0)
            return self._send_json({"slept_ms": ms})

        if path == "/big":
            kb = int(qs.get("kb", ["64"])[0])
            return self._send(b"A" * (kb * 1024), ctype="application/octet-stream")

        if path.startswith("/redirect/"):
            n = int(path.rsplit("/", 1)[1])
            target = "/redirect/%d" % (n - 1) if n > 1 else "/"
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if path == "/index.json":
            return self._send_json({
                "name": "Fixture Repository",
                "extensions": [
                    {"name": "Fixture Anime", "apk": "/dl/anime.apk", "version": "1.2.3", "lang": "en"},
                    {"name": "Fixture Manga", "apk": "/dl/manga.apk", "version": "4.5.6", "lang": "all"},
                ],
            })

        if path == "/dl/anime.apk" or path == "/dl/manga.apk":
            # A minimal well-formed zip so the detection test has real bytes.
            payload = b"PK\x03\x04" + b"\x00" * 26
            return self._send(payload, ctype="application/octet-stream")

        return self._send(b"not found", 404, "text/plain")


class Server:
    """In-process test server usable from the test suites."""

    def __init__(self, port: int = 0):
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    @property
    def base(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def __enter__(self) -> "Server":
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


if __name__ == "__main__":
    # Standalone mode prints the base URL on the first line so a test harness
    # (the Node smoke test) can start it and read the port back.
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8777
    srv = Server(port)
    print(f"{srv.base}", flush=True)
    srv.thread.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        srv.httpd.shutdown()
