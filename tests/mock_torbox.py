#!/usr/bin/env python3
"""Mock TorBox and Real-Debrid APIs plus a file server with HTTP Range support, for testing on the PC.

    mock_torbox.py [PORT]

API (same shapes as https://api.torbox.app/v1/api): user/me, {torrents,webdl,usenet}/mylist, .../requestdl.
The only accepted API key is "test-key-0000".
Real-Debrid (same shapes as https://api.real-debrid.com/rest/1.0) under /rest/1.0: user, torrents (paged, X-Total-Count),
torrents/info/<id>, downloads, unrestrict/link. Token "rd-test-token". Direct links /rdfile/<issued>/<size>/<name>
expire after RD["ttl"] seconds (403). /control?rd_ttl=N&rd_429=N sets the link lifetime and makes the next N API calls
answer 429; /control/stats gives counts of unrestrict calls per link.
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
TB_REQUESTDL = {}
TOKEN_BYTES = {}
ONCE_200 = set()


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
        {"id": 103, "hash": "ghi", "name": "Stalled CDN rotate", "size": 20 * MIB, "active": False,
         "created_at": "2026-11-22T10:00:00Z", "download_state": "completed", "progress": 1, "download_speed": 0,
         "download_present": True, "download_finished": True, "cached": True,
         "files": [file_entry(0, "stalled-cdn.bin", 20 * MIB)]},
        {"id": 104, "hash": "jkl", "name": "HTTP 400 rotate", "size": 24 * MIB, "active": False,
         "created_at": "2026-11-23T10:00:00Z", "download_state": "completed", "progress": 1, "download_speed": 0,
         "download_present": True, "download_finished": True, "cached": True,
         "files": [file_entry(0, "http400-rotate.bin", 24 * MIB)]},
    ],
    "webdl": [
        {"id": 201, "name": "flaky-server.bin", "size": 40 * MIB, "created_at": "2026-11-22T10:00:00Z",
         "download_state": "completed", "progress": 1, "download_present": True, "download_finished": True,
         "files": [file_entry(5, "flaky-server.bin", 40 * MIB)]},
    ],
    "usenet": [],
}


RD_TOKEN = "rd-test-token"
RD = {"ttl": 600, "429": 0, "unrestrict": {}}
RDL = "https://real-debrid.com/d/"
# hoster link -> (file name, size, chunks, extra query for the direct link)
RD_LINKS = {
    RDL + "EPISODE1": ("e01.mkv", 20 * MIB, 4, ""),
    RDL + "SUBS1": ("e01.srt", 3 * MIB, 0, ""),
    RDL + "PACKED": ("Packed.rar", 5 * MIB, 0, ""),
    RDL + "SLOW": ("slow-rd.bin", 40 * MIB, 0, "?rate=1000000"),
    RDL + "HIST": ("history.iso", 9 * MIB, 0, ""),
}


def rd_torrent(i, name, status, files, links, progress=100):
    sel = sum(f["bytes"] for f in files if f["selected"])
    return {"id": "RDT%d" % i, "filename": name, "original_filename": name, "hash": "h%d" % i, "bytes": sel,
            "original_bytes": sum(f["bytes"] for f in files), "host": "real-debrid.com", "split": 2000,
            "progress": progress, "status": status, "added": "2026-11-2%dT10:00:00.000Z" % i, "files": files,
            "links": links, "ended": "2026-11-2%dT11:00:00.000Z" % i}


RD_TORRENTS = [
    rd_torrent(1, "Show S01", "downloaded", [
        {"id": 1, "path": "/Show S01/e01.mkv", "bytes": 20 * MIB, "selected": 1},
        {"id": 2, "path": "/Show S01/sample.txt", "bytes": 1000, "selected": 0},
        {"id": 3, "path": "/Show S01/Subs/e01.srt", "bytes": 3 * MIB, "selected": 1}],
        [RDL + "EPISODE1", RDL + "SUBS1"]),
    rd_torrent(2, "Packed", "downloaded", [
        {"id": 1, "path": "/a.bin", "bytes": MIB, "selected": 1},
        {"id": 2, "path": "/b.bin", "bytes": MIB, "selected": 1},
        {"id": 3, "path": "/c.bin", "bytes": MIB, "selected": 1}], [RDL + "PACKED"]),
    rd_torrent(3, "Still downloading", "downloading", [
        {"id": 1, "path": "/x.mkv", "bytes": 5 * MIB, "selected": 1}], [], 40),
    rd_torrent(4, "Slow one", "downloaded", [{"id": 1, "path": "/slow-rd.bin", "bytes": 40 * MIB, "selected": 1}],
               [RDL + "SLOW"]),
    rd_torrent(5, "Fifth", "downloaded", [{"id": 1, "path": "/five.bin", "bytes": MIB, "selected": 1}], [RDL + "FIVE"]),
]
RD_HISTORY = [{"id": "D1", "filename": "history.iso", "mimeType": "application/octet-stream", "filesize": 9 * MIB,
               "link": RDL + "HIST", "host": "real-debrid.com", "chunks": 16, "download": "https://expired.example/x",
               "generated": "2026-11-20T10:00:00.000Z"}]


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

    def rd_fail(self, code, error, error_code):
        self.send_json(code, {"error": error, "error_code": error_code})

    def rd_api(self, method, path, q, form):
        if RD["429"] > 0:
            RD["429"] -= 1
            return self.rd_fail(429, "too_many_requests", 34)
        auth = self.headers.get("Authorization", "")
        if auth != "Bearer " + RD_TOKEN:
            return self.rd_fail(401, "bad_token", 8)
        if method == "GET" and path == "user":
            return self.send_json(200, {"id": 7, "username": "rdtester", "email": "rd@example.com", "points": 10,
                                        "locale": "en", "avatar": "", "type": "premium", "premium": 86400 * 30,
                                        "expiration": "2027-02-01T00:00:00.000Z"})
        if method == "GET" and path in ("torrents", "downloads"):
            items = RD_TORRENTS if path == "torrents" else RD_HISTORY
            limit = int(q.get("limit", 100))
            page = int(q.get("page", 1))
            chunk = items[(page - 1) * limit: page * limit]
            if path == "torrents":
                chunk = [{k: v for k, v in t.items() if k not in ("files", "original_filename", "original_bytes")}
                         for t in chunk]
            body = json.dumps(chunk).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("X-Total-Count", str(len(items)))
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        m = re.match(r"^torrents/info/(\w+)$", path)
        if method == "GET" and m:
            t = next((t for t in RD_TORRENTS if t["id"] == m.group(1)), None)
            return self.send_json(200, t) if t else self.rd_fail(404, "unknown_ressource", 7)
        if method == "POST" and path == "unrestrict/link":
            link = form.get("link", "")
            RD["unrestrict"][link] = RD["unrestrict"].get(link, 0) + 1
            if link.startswith("https://unsupported.example/"):
                return self.rd_fail(503, "hoster_unsupported", 16)
            if link not in RD_LINKS:
                return self.rd_fail(503, "unavailable_file", 24)
            name, size, chunks, extra = RD_LINKS[link]
            host = "http://%s" % self.headers.get("Host")
            return self.send_json(200, {"id": "U1", "filename": name, "mimeType": "application/octet-stream",
                                        "filesize": size, "link": link, "host": "example.com", "chunks": chunks,
                                        "crc": 1, "download": "%s/rdfile/%d/%d/%s%s" % (host, int(time.time()), size,
                                                                                          name, extra),
                                        "streamable": 0})
        return self.rd_fail(404, "unknown_method", 3)

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length", 0))
        form = {k: v[0] for k, v in parse_qs(self.rfile.read(n).decode()).items()}
        if u.path.startswith("/rest/1.0/"):
            return self.rd_api("POST", u.path[len("/rest/1.0/"):], {}, form)
        self.fail(404, "ENDPOINT_NOT_FOUND", "Endpoint not found.")

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path.startswith("/files/"):
            return self.serve_file(u.path, q)
        if u.path.startswith("/rest/1.0/"):
            return self.rd_api("GET", u.path[len("/rest/1.0/"):], q, {})
        if u.path == "/control":
            if "rd_ttl" in q:
                RD["ttl"] = int(q["rd_ttl"])
            if "rd_429" in q:
                RD["429"] = int(q["rd_429"])
            if "once_200" in q:
                with ACTIVE_LOCK:
                    ONCE_200.add(q["once_200"])
            return self.send_json(200, {"ok": True})
        if u.path == "/control/stats":
            return self.send_json(200, {"unrestrict": RD["unrestrict"], "tb_requestdl": TB_REQUESTDL})
        m = re.match(r"^/rdfile/(\d+)/(\d+)/", u.path)
        if m:
            if time.time() - int(m.group(1)) > RD["ttl"]:
                return self.send_json(403, {"error": "link expired"})
            return self.serve_file("/files/" + m.group(2), q)
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
        if kind == "torrents":
            sid = str(item["id"])
            with ACTIVE_LOCK:
                n = TB_REQUESTDL.get(sid, 0) + 1
                TB_REQUESTDL[sid] = n
            if item["id"] == 103 and n == 1:
                extra += "&stall=1"
            if item["id"] == 104:
                extra += "&tok=t104_%d" % n
                if n == 1:
                    extra += "&drop=%d&http400_after=%d" % (2 * MIB, 4 * MIB)
        return self.send_json(200, {"success": True, "error": None, "detail": "ok",
                                    "data": "%s/files/%d?x=1%s" % (host, size, extra)})

    def serve_file(self, path, q):
        limit = int(q.get("maxconn", 0))
        with ACTIVE_LOCK:
            busy = limit and ACTIVE.get(path, 0) >= limit
            if not busy:
                ACTIVE[path] = ACTIVE.get(path, 0) + 1
        if busy:
            ra = q.get("retry_after")
            body = json.dumps({"success": False, "error": "TOO_MANY", "detail": "too many connections", "data": None}).encode()
            self.send_response(429)
            self.send_header("Content-Type", "application/json")
            if ra:
                self.send_header("Retry-After", ra)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        try:
            self.serve_file_body(path, q)
        finally:
            with ACTIVE_LOCK:
                ACTIVE[path] -= 1

    def serve_file_body(self, path, q):
        if q.get("bad_if_range") and self.headers.get("If-Range"):
            self.send_response(400)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        tok = q.get("tok", "")
        cap400 = int(q.get("http400_after", 0))
        if tok and cap400:
            with ACTIVE_LOCK:
                if TOKEN_BYTES.get(tok, 0) >= cap400:
                    self.send_response(400)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
        if q.get("bad_probe_zero") and self.headers.get("Range") == "bytes=0-0":
            body = b"zero-byte range rejected"
            self.send_response(400)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        force_norange = bool(q.get("norange"))
        if q.get("tag"):
            with ACTIVE_LOCK:
                if q["tag"] in ONCE_200:
                    ONCE_200.discard(q["tag"])
                    force_norange = True
        size = int(path.split("/")[2])
        start, end, code = 0, size - 1, 200
        rng = self.headers.get("Range")
        if rng and not force_norange:
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
        if q.get("etag"):
            tag = "flipped" if q.get("etag_flip") and start > 0 else q["etag"]
            self.send_header("ETag", '"%s"' % tag)
        if not force_norange:
            self.send_header("Accept-Ranges", "bytes")
        if code == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        if q.get("stall") and length > 1:
            try:
                self.wfile.write(content(start, 1))
                self.wfile.flush()
                time.sleep(30)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            return
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
                if tok:
                    with ACTIVE_LOCK:
                        TOKEN_BYTES[tok] = TOKEN_BYTES.get(tok, 0) + n
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
