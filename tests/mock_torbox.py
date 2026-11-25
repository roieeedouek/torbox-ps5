#!/usr/bin/env python3
"""Mock TorBox API plus a file server with HTTP Range support, for testing the downloader on the PC.

    mock_torbox.py [PORT]

API (same shapes as https://api.torbox.app/v1/api): user/me, {torrents,webdl,usenet}/mylist, .../requestdl.
The only accepted API key is "test-key-0000".
Files: /files/<size>[?norange=1][&drop=<bytes>][&rate=<bytes/s>] serve <size> bytes whose byte i is i % 251.
  norange: ignore Range headers;  drop: close each connection after that many bytes (tests resume);
  rate: throttle each connection;  maxconn: answer 429 beyond that many parallel connections.
"""
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

KEY = "test-key-0000"
PATTERN = bytes(i % 251 for i in range(251 * 4096))
MIB = 1024 * 1024
ACTIVE = {}
ACTIVE_LOCK = threading.Lock()


def content(start, length):
    out = bytearray()
    pos = start
    while length > 0:
        off = pos % 251
        take = min(length, len(PATTERN) - off)
        out += PATTERN[off:off + take]
        pos += take
        length -= take
    return bytes(out)


def file_entry(fid, name, size):
    return {"id": fid, "md5": None, "hash": "h%d" % fid, "name": name, "size": size, "zipped": False,
            "s3_path": "x/" + name, "infected": False, "mimetype": "application/octet-stream",
            "short_name": name.split("/")[-1], "absolute_path": "/x/" + name, "opensubtitles_hash": None}


ITEMS = {
    "torrents": [
        {"id": 101, "hash": "abc", "name": "Big Buck Bunny (test)", "size": 60 * MIB + 3 * MIB, "active": False,
         "created_at": "2026-11-20T10:00:00Z", "download_state": "completed", "progress": 1, "download_speed": 0,
         "download_present": True, "download_finished": True, "cached": True,
         "files": [file_entry(0, "Big Buck Bunny (test)/movie.mkv", 60 * MIB),
                   file_entry(1, "Big Buck Bunny (test)/extras/poster.jpg", 3 * MIB)]},
        {"id": 102, "hash": "def", "name": "Still downloading", "size": 10 * MIB, "active": True,
         "created_at": "2026-11-21T10:00:00Z", "download_state": "downloading", "progress": 0.42,
         "download_present": False, "download_finished": False, "files": []},
    ],
    "webdl": [
        {"id": 201, "name": "flaky-server.bin", "size": 40 * MIB, "created_at": "2026-11-22T10:00:00Z",
         "download_state": "completed", "progress": 1, "download_present": True, "download_finished": True,
         "files": [file_entry(5, "flaky-server.bin", 40 * MIB)]},
    ],
    "usenet": [],
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if "-v" in sys.argv:
            super().log_message(fmt, *args)

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def fail(self, code, error, detail):
        self.send_json(code, {"success": False, "error": error, "detail": detail, "data": None})

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path.startswith("/files/"):
            return self.serve_file(u.path, q)
        m = re.match(r"^/v1/api/(.*)$", u.path)
        if not m:
            return self.fail(404, "ENDPOINT_NOT_FOUND", "Endpoint not found.")
        ep = m.group(1)
        auth = self.headers.get("Authorization", "")
        token = q.get("token") or (auth[7:] if auth.startswith("Bearer ") else "")
        if not token:
            return self.fail(401, "NO_AUTH", "Please provide a valid API token.")
        if token != KEY:
            return self.fail(401, "BAD_TOKEN", "Your token is invalid or has expired.")
        if ep == "user/me":
            return self.send_json(200, {"success": True, "error": None, "detail": "ok", "data": {
                "id": 1, "email": "tester@example.com", "plan": 2, "premium_expires_at": "2027-01-01T00:00:00Z"}})
        m = re.match(r"^(torrents|webdl|usenet)/(mylist|requestdl)$", ep)
        if not m:
            return self.fail(404, "ENDPOINT_NOT_FOUND", "Endpoint not found.")
        kind, op = m.groups()
        if op == "mylist":
            data = ITEMS[kind]
            if not data:
                return self.send_json(200, {"success": True, "error": None, "detail": "none", "data": []})
            return self.send_json(200, {"success": True, "error": None, "detail": "ok", "data": data})
        idname = {"torrents": "torrent_id", "webdl": "web_id", "usenet": "usenet_id"}[kind]
        item = next((i for i in ITEMS[kind] if str(i["id"]) == q.get(idname)), None)
        if not item:
            return self.fail(404, "DOWNLOAD_NOT_FOUND", "Download not found.")
        host = "http://%s" % self.headers.get("Host")
        if q.get("zip_link") == "true":
            size = item["size"]
        else:
            f = next((f for f in item["files"] if str(f["id"]) == q.get("file_id")), None)
            if not f:
                return self.fail(404, "FILE_NOT_FOUND", "File not found.")
            size = f["size"]
        extra = "&drop=%d" % (7 * MIB) if kind == "webdl" else ""
        return self.send_json(200, {"success": True, "error": None, "detail": "ok",
                                    "data": "%s/files/%d?x=1%s" % (host, size, extra)})

    def serve_file(self, path, q):
        limit = int(q.get("maxconn", 0))
        with ACTIVE_LOCK:
            busy = limit and ACTIVE.get(path, 0) >= limit
            if not busy:
                ACTIVE[path] = ACTIVE.get(path, 0) + 1
        if busy:
            return self.fail(429, "TOO_MANY", "too many connections")
        try:
            self.serve_file_body(path, q)
        finally:
            with ACTIVE_LOCK:
                ACTIVE[path] -= 1

    def serve_file_body(self, path, q):
        size = int(path.split("/")[2])
        start, end, code = 0, size - 1, 200
        rng = self.headers.get("Range")
        if rng and not q.get("norange"):
            m = re.match(r"bytes=(\d*)-(\d*)$", rng)
            if m:
                start = int(m.group(1) or 0)
                end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                code = 206
        length = end - start + 1
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        if not q.get("norange"):
            self.send_header("Accept-Ranges", "bytes")
        if code == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        drop = int(q.get("drop", 0))
        rate = int(q.get("rate", 0))
        sent, pos = 0, start
        t0 = time.time()
        try:
            while sent < length:
                n = min(256 * 1024, length - sent)
                if drop and sent + n > drop:
                    n = drop - sent
                    if n > 0:
                        self.wfile.write(content(pos, n))
                    self.close_connection = True
                    return
                self.wfile.write(content(pos, n))
                sent += n
                pos += n
                if rate:
                    ahead = sent / rate - (time.time() - t0)
                    if ahead > 0:
                        time.sleep(ahead)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    port = int(next((a for a in sys.argv[1:] if a.isdigit()), "8765"))
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print("mock TorBox on http://127.0.0.1:%d/v1/api" % port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
