# TorBox for PS5

Browse your [TorBox](https://torbox.app) account and download its files **onto a jailbroken PS5**, from a web page
you open on your phone or PC: `http://<console-ip>:8080`.

- Log in with your TorBox API key (checked against `user/me`, then stored on the console).
- Torrents, web downloads and usenet lists with names, sizes, status and the files inside each item.
- Download one file, all files of an item (folder structure kept), or the item as one ZIP, to a folder you choose:
  type it or pick it with the folder browser (`/data/...` and USB drives `/mnt/usbN`, `/mnt/extN`).
- Progress bar, bytes, speed, time left, connections in use. Pause, resume, cancel, retry, a queue.
- Several HTTP Range connections per file (1 to 16, default 8). When a connection finishes its part it splits the
  largest remaining part, so all connections stay busy until the end.
- Partial files resume: after a pause, an error, a network drop, a server restart or a console reboot, the download
  continues from the `.part` file and its segment table (`.part.state`); nothing finished is fetched again.
- A direct link box (any `http(s)://` URL) for non-TorBox files.

## How it runs (and why)

There are two pieces:

| Piece | What it is | Why |
|---|---|---|
| `torbox-ps5.elf` | The web server and downloader, a payload for the ELF loader (elfldr, port 9021). | Runs in the background, independent of any app: downloads continue while you play. |
| `PPSA97630` title | A home-screen tile, "TorBox Downloader". Opening it starts the server if it isn't running (sends `torbox-ps5.elf` to elfldr on `127.0.0.1:9021`), opens the PS5's web browser on `http://127.0.0.1:8080` (the same page, on the TV, controlled with the controller), and closes itself. | One tap to start and use, like your other homebrew. |

The server is deliberately **not** inside the title. A PS5 game or app is closed when another game starts and
stops in rest mode. A server living in the title would cut downloads off as soon as you started a game. The
payload runs on its own, so you can start a download and play something else.

The tile needs the ELF loader listening on port 9021, which is how this console's jailbreak loads payloads.
Without it, the tile tells you so, and you can send `torbox-ps5.elf` with any payload sender instead.

The server stops when you press **Stop server** in the web page, or when the console reboots or enters rest
mode. Rest mode also suspends payloads. Unfinished downloads are kept in the queue and continue the next time
the server starts.

## Install

You need FTP access to the console (e.g. zftpd on port 2120 or 2121) and ShadowMountPlus to register titles.

1. Copy the `PPSA97630` folder (from the release zip, or `build/PPSA97630` after building) to
   **`/data/homebrew/PPSA97630/`**. With curl:
   ```sh
   H=ftp://10.0.0.20:2120 D=build/PPSA97630
   for f in eboot.bin torbox-ps5.elf sce_sys/param.json sce_sys/icon0.png sce_module/libc.prx; do
       curl --disable-epsv --ftp-create-dirs -T "$D/$f" "$H/data/homebrew/PPSA97630/$f"
   done
   curl --disable-epsv -Q "SITE CHMOD 755 /data/homebrew/PPSA97630/eboot.bin" \
        -Q "SITE CHMOD 755 /data/homebrew/PPSA97630/sce_module/libc.prx" "$H/data/homebrew/PPSA97630/"
   ```
   It must end up as:
   ```
   /data/homebrew/PPSA97630/eboot.bin            (chmod 755)
   /data/homebrew/PPSA97630/torbox-ps5.elf
   /data/homebrew/PPSA97630/sce_sys/param.json
   /data/homebrew/PPSA97630/sce_sys/icon0.png
   /data/homebrew/PPSA97630/sce_module/libc.prx  (chmod 755)
   ```
   The last command sets the two modes (or use your FTP client's chmod, or the shell server's `chmod 755 <file>`).
2. Register the title with ShadowMountPlus. "TorBox Downloader" appears on the home screen.
3. Open the tile. The server starts (a notification shows `TorBox: open http://<console-ip>:8080 in a browser`)
   and the PS5's browser opens on the page. You can also use the page from a phone or PC at that address.

**Without the title:** send the payload yourself: `cat torbox-ps5.elf > /dev/tcp/<console-ip>/9021` (Linux/WSL),
or with any payload sender to port 9021.

**Update:** press *Stop server* on the page, replace the files, start again. Downloads resume.

## Use

1. On a phone or PC on the same network open `http://<console-ip>:8080`.
2. Paste your API key (torbox.app, Settings, API key) and press *Log in*. The key is checked with TorBox first.
   It is stored **unencrypted** on the console in `/data/torbox-ps5/torbox-key.txt` and never sent back to the
   browser. *Log out* deletes it.
3. Pick *Torrents*, *Web* or *Usenet*. *Show files* lists an item's files. *Refresh* reloads from TorBox
   (bypassing TorBox's cache).
4. Set *Save to* (default `/data/torbox-downloads`) or press *Browse* to pick or create a folder. The folder is
   remembered by your browser.
5. *Download* (one file, saved directly in the folder), *Download all files* (into `<folder>/<item name>/...`), or
   *As one ZIP*. The *Downloads* tab shows the queue. Items still downloading on TorBox's side can't be
   downloaded yet.
6. In *Downloads*: *Connections per file* and *Files at a time* apply to downloads that start (or resume) from
   then on. Pause, then Resume, to change the connections of a running download.

### Speed

Measured on this console (fw 13.60) on the owner's network:

- The console gives sockets a 64 KiB TCP receive buffer, which held one connection at about 1.2 MB/s
  (window / round-trip time). The app asks for 512 KiB, the maximum the console grants: about 7.9 MB/s on one
  connection to a public mirror.
- TorBox downloads reached 16 to 26 MB/s total with 4 to 16 connections. 32 connections made TorBox's CDN stall
  most of them, so the setting stops at 16. The best value depends on the CDN server you get (each start or
  resume asks TorBox for a fresh link). 4 to 8 is a good start.
- The console's internal storage took 130 to 180 MB/s of writes, so the disk is not the limit.
- If you expect more (TorBox itself can do 80 to 90 MB/s), check the console's own link first. Open
  `http://<console-ip>:8080/api/speedtest?mb=200` on a PC on the same network: it streams 200 MB from the
  console. A Wi-Fi console showed about 23 MB/s there. A wired connection is the biggest win.

## Files on the console

```
/data/torbox-ps5/torbox-key.txt   API key (unencrypted, mode 600)
/data/torbox-ps5/queue.tsv        unfinished downloads (ids and folders; never links or the key)
/data/torbox-ps5/settings.txt     connections, files at a time
/data/torbox-ps5/log.txt          event log (never contains the key or download links)
<folder>/<name>.part(.state)      a download in progress and its segment table
```

The server also logs to klog (port 3232).

## Security notes

- The page has no password: anyone on your network can use it while the server runs (but cannot read the key).
  Stop the server when you don't need it, and don't expose port 8080 to the internet.
- Changes need an `X-TorBox-PS5` request header, so a web page on another site cannot drive the server from your
  browser.
- Folders are limited to `/data/...`, `/mnt/usbN/...` and `/mnt/extN/...`.

## Build

Needs this checkout next to the PS5 toolchains in `../deps` (used in place, not copied):

- `deps/ps5-sdk-full`: payload SDK with libcurl 8 + OpenSSL 3 (for the server payload),
- `deps/ps5-vulkan` (native tool, app_crt, link recipe, libc.prx) and
  `deps/ps5-vulkan-template/.deps/native/ps5-payload-sdk` (for the title),
- LLD 22 through `LLVM_CONFIG` (defaults to `../source-recomp/build/ps5-toolchain/llvm-config` when present),
- python3 with numpy (icon), clang/gcc, and for the host build libcurl (`HOST_CURL`, default linuxbrew's).

```sh
./build.sh host      # Linux build for testing            -> build/host/torbox-ps5
./build.sh payload   # PS5 server payload                 -> build/torbox-ps5.elf
./build.sh title     # PS5 title, includes the payload    -> build/PPSA97630/
./build.sh package   # zip with title, payload and README -> build/torbox-ps5-<version>.zip
./build.sh all
tests/test_host.sh   # end-to-end test of the host build against tests/mock_torbox.py
```

The web UI (`web/`) and the CA certificates (`/etc/ssl/certs/ca-certificates.crt`, or `CA_BUNDLE`) are compiled
into the binary by `tools/embed.py`.

`tests/mock_torbox.py` imitates the TorBox API (`user/me`, `*/mylist`, `*/requestdl`) and serves test files with
Range support, broken connections, throttling and connection limits. `tests/test_host.sh` covers login and logout,
lists, the folder browser, single and multi-connection downloads, resume after pause and after a restart,
connection splitting, HTTP 429 limits, cancel, and checksums.

### Code map

```
src/main.c       start-up
src/httpd.c      small HTTP/1.1 server (thread per connection; poll() before every socket wait)
src/api.c        JSON API: status, login, logout, list, dirs, mkdir, download, jobs/*, settings, sha256, speedtest, quit
src/torbox.c     TorBox client (libcurl), key storage, socket tuning
src/dl.c         queue, multi-connection downloads, resume, persistence
src/platform.c   PS5 payload / host differences (paths, notifications, klog)
web/             the page (plain HTML/CSS/JS, no external resources)
title/           launcher title: launcher.c, build_title.sh, param.json, icon
```

API used: `GET /v1/api/user/me`, `GET /v1/api/{torrents,webdl,usenet}/mylist`,
`GET /v1/api/{torrents,webdl,usenet}/requestdl?token=...&{torrent_id|web_id|usenet_id}=...&file_id=...|zip_link=true`.
