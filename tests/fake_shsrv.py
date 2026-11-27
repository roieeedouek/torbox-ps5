#!/usr/bin/env python3
"""A stand-in for shsrv's pkg_install, for the host tests.

    fake_shsrv.py PORT RESULT_FILE

Answers "pkg_install <url>" like shsrv (a content_id line, then the prompt), then reads the package from <url> the
way the PS5 installer does: header and tail first, then front to back in 18 MiB ranges, two at a time. Each read's
bytes are checked (byte i must be i % 251). A URL whose name contains "partial" stops after ~20 MiB (a PS5 that gives
up). RESULT_FILE gets one line per install: "<name> ok <bytes>" or "<name> bad <bytes>".
"""
import socketserver
import sys
import threading
import urllib.request

CID = "UP0000-TEST00000_00-TORBOXTESTPKG000"
RESULT = sys.argv[2]
LOCK = threading.Lock()


def size_of(url):
    req = urllib.request.Request(url, method="HEAD")
    return int(urllib.request.urlopen(req, timeout=30).headers["Content-Length"])


def get(url, a, b):
    req = urllib.request.Request(url, headers={"Range": "bytes=%d-%d" % (a, b)})
    return urllib.request.urlopen(req, timeout=120).read()


def ok(data, start):
    return all(data[i] == (start + i) % 251 for i in range(0, len(data), 4093)) and \
        all(data[i] == (start + i) % 251 for i in range(max(0, len(data) - 64), len(data)))


def install(url):
    name = url.rsplit("/", 1)[-1]
    good, total = True, 0
    try:
        size = size_of(url)
        good &= ok(get(url, 0, 65535), 0) and ok(get(url, size - 65536, size - 1), size - 65536)
        chunk = 18 << 20
        starts = list(range(65536, size, chunk))
        if "partial" in name:
            starts = starts[:1]
        sent = {}

        def worker():
            while starts:
                try:
                    a = starts.pop(0)
                except IndexError:
                    return
                sent[a] = get(url, a, min(a + chunk, size) - 1)

        ts = [threading.Thread(target=worker) for _ in range(2)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        total = sum(len(v) for v in sent.values()) + 65536
        good &= all(ok(v, a) for a, v in sent.items())
    except Exception as e:  # noqa: BLE001 - reported in the result file
        good = False
        name += " (%s)" % e
    with LOCK, open(RESULT, "a") as f:
        f.write("%s %s %d\n" % (name, "ok" if good else "bad", total))


class Shell(socketserver.StreamRequestHandler):
    def handle(self):
        self.wfile.write(b"Welcome to fake shsrv\n/$ ")
        for raw in self.rfile:
            line = raw.decode().strip()
            if line.startswith("pkg_install "):
                url = line.split(" ", 1)[1]
                self.wfile.write(("IpcFacade::appInstallByPackage pkg_info content_id = [%s] content_type = [0] "
                                  "content_platform = [0]\n/$ " % CID).encode())
                self.wfile.flush()
                threading.Thread(target=install, args=(url,), daemon=True).start()
            elif line == "exit":
                return
            else:
                self.wfile.write(b"/$ ")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    Server(("127.0.0.1", int(sys.argv[1])), Shell).serve_forever()
