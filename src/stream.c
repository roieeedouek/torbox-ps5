// Streaming to the PS5's package installer: a TorBox/Real-Debrid/AllDebrid file is offered at
// http://127.0.0.1:<port>/stream/<token>/<name> with HTTP byte ranges, and the system installer
// (sceAppInstUtilInstallByPackage with that URL) pulls the package from there while it installs, so the package is
// never stored on the console. Each request is passed on to the provider's direct link; when the link expires or
// drops part-way, it is unrestricted again and the transfer continues where it stopped, unseen by the installer.
// Direct links stay in memory only and are never logged.
#include "app.h"

#include <curl/curl.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_STREAMS 8
#define MAX_TRIES 8 // upstream attempts per installer request

typedef struct {
    char token[33];
    char kind[12]; // rd, ad, url, or TorBox's torrents/webdl/usenet (link "item:file")
    char *link;    // hoster link (rd/ad), "item:file" (TorBox) or the direct URL (url)
    char *direct; // current direct link, memory only
    char name[256];
    int64_t size, served, created_ms, last_ms;
    int requests, active;
    // Read-ahead window (see ra_thread): RA_WIN blocks from block `base` on, fetched ahead of the installer.
    struct rablock *win;
    int64_t base;
    int64_t pos[4]; // where the installer's open requests are reading, -1 = free
    int ra_running, ra_quit; // ra_quit: asked to stop (the stream is being removed)
    int stopping;            // Stop pressed in the page: answer no more requests, then remove the stream
    int64_t gone_ms;         // when the installer last closed a connection in the middle of a transfer
    pthread_cond_t cv;
    // Install through the helper (helper/installer.c), when the stream was made for one.
    // Speeds over the last ~20 s: the installer pulls in bursts (~900 MB at loopback speed, then ~5 s unpacking),
    // so shorter windows swing between ~15 and ~190 MB/s. What it pulled, and what the read-ahead fetched.
#define SP_RING 21
    double speed, net_speed;
    int64_t ra_fetched, ahead; // bytes fetched by the read-ahead so far / ready ahead of the installer
    int64_t sp_ms[SP_RING], sp_served[SP_RING], sp_fetched[SP_RING]; // 1 s samples (ring), sp_n of them
    int sp_n;
    int64_t saved_served, saved_ms; // `served` survives restarts (streams.tsv); when it was last saved
    int end_seen;                   // the installer read the file's end late in the install
    int restored;                   // restored mid-install: the installer's next big read shows where it is
    int ins; // 0 none, 1 starting, 2 installing (the PS5 pulls the package), 3 done, 4 failed
    char ins_status[24], ins_err[200], content_id[48];
    int64_t ins_done, ins_total, ins_started_ms, ins_finished_ms, ins_report_ms;
} stream_t;

static const char *ins_names[] = {"", "starting", "installing", "installed", "failed", "stopped"};

static pthread_once_t g_monitor_once = PTHREAD_ONCE_INIT;
static void start_monitor(void); // finishes installs (defined with the install code below)
static void clear_slot_locked(stream_t *s);

static stream_t g_streams[MAX_STREAMS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void save_streams_locked(void) {
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s/streams.tsv", plat_app_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &g_streams[i];
        if (s->token[0]) // hoster links only, like queue.tsv: never the direct link
            fprintf(f, "%s\t%s\t%lld\t%lld\t%s\t%s\t%d\t%s\t%lld\n", s->token, s->kind, (long long)s->size,
                    (long long)s->last_ms, s->name, s->link ? s->link : "", s->ins,
                    s->content_id[0] ? s->content_id : "-", (long long)s->served);
    }
    fclose(f);
    rename(tmp, path);
}

// Streams survive a server restart: an install in progress keeps its URL, and the direct link is fetched again on
// the next request.
void stream_init(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/streams.tsv", plat_app_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = malloc(16384);
    int n = 0;
    pthread_mutex_lock(&g_lock);
    while (line && n < MAX_STREAMS && fgets(line, 16384, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *field[9];
        int k = 0;
        for (char *p = line, *save; k < 9 && (field[k] = strtok_r(k ? NULL : p, "\t", &save)); k++) {}
        if (k < 6 || strlen(field[0]) != 32) continue;
        stream_t *s = &g_streams[n++];
        memset(s, 0, sizeof *s);
        pthread_cond_init(&s->cv, NULL);
        for (int i = 0; i < 4; i++) s->pos[i] = -1;
        snprintf(s->token, sizeof s->token, "%s", field[0]);
        snprintf(s->kind, sizeof s->kind, "%s", field[1]);
        s->size = atoll(field[2]);
        snprintf(s->name, sizeof s->name, "%s", field[4]);
        s->link = strdup(field[5]);
        s->created_ms = s->last_ms = plat_now_ms();
        s->ins = k > 6 ? atoi(field[6]) : 0;
        if (k > 7 && strcmp(field[7], "-")) snprintf(s->content_id, sizeof s->content_id, "%s", field[7]);
        if (k > 8) s->served = s->saved_served = atoll(field[8]); // progress so far (approximate after a restart)
        if (s->ins == 4 && s->size > 0 && s->served >= s->size - s->size / 50) { // judged "stalled" by 1.2.0 near
            s->ins = 3;                                                          // the end: it was delivered
            snprintf(s->ins_status, sizeof s->ins_status, "delivered");
            s->ins_err[0] = 0;
        }
        if (s->ins == 1) { // the helper never picked it up before the restart
            s->ins = 4;
            snprintf(s->ins_err, sizeof s->ins_err, "the server restarted before the install started: try again");
        }
        s->ins_report_ms = plat_now_ms(); // an install in progress keeps reporting to the new server
        s->ins_started_ms = s->created_ms; // its real start is not kept: count time from the restart
        if (s->ins == 2) s->restored = 1;
    }
    pthread_mutex_unlock(&g_lock);
    free(line);
    fclose(f);
    if (n) plat_log("streams: %d restored", n);
    int running = 0;
    for (int i = 0; i < n; i++) running |= g_streams[i].ins == 2;
    if (running) pthread_once(&g_monitor_once, start_monitor); // an install in progress keeps going
}

static int server_port(void) {
    const char *env = getenv("TORBOX_PORT");
    return env && atoi(env) > 0 ? atoi(env) : APP_PORT;
}

static void make_token(char out[33]) {
    unsigned char r[16];
    int f = open("/dev/urandom", O_RDONLY);
    ssize_t n = f >= 0 ? read(f, r, sizeof r) : -1;
    if (f >= 0) close(f);
    if (n != (ssize_t)sizeof r) {
        uint64_t x = (uint64_t)plat_now_us() ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)&r;
        for (int i = 0; i < 16; i++) {
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
            r[i] = (unsigned char)(x >> 56);
        }
    }
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", r[i]);
}

static int is_tb_kind(const char *kind) {
    return !strcmp(kind, "torrents") || !strcmp(kind, "webdl") || !strcmp(kind, "usenet");
}

// kind "rd"/"ad": unrestrict the hoster link; TorBox kinds: request a link for "item:file"; "url": as it is.
static int resolve(const char *kind, const char *link, char *direct, size_t dlen, char *name, size_t nlen, char *err,
                   size_t errlen) {
    int permanent = 0, chunks = 0;
    if (is_tb_kind(kind)) {
        long long item = -1, file = -1;
        if (sscanf(link, "%lld:%lld", &item, &file) != 2) {
            snprintf(err, errlen, "bad TorBox file");
            return -1;
        }
        return tb_request_link(kind, item, file, direct, dlen, err, errlen);
    }
    if (!strcmp(kind, "rd")) return rd_unrestrict(link, direct, dlen, name, nlen, &chunks, &permanent, err, errlen);
    if (!strcmp(kind, "ad")) return ad_unlock(link, direct, dlen, name, nlen, &permanent, err, errlen);
    snprintf(direct, dlen, "%s", link);
    return 0;
}

typedef struct {
    int64_t total;
} size_probe;

static size_t probe_hdr(char *p, size_t sz, size_t n, void *ud) {
    size_probe *s = ud;
    size_t len = sz * n;
    if (len > 14 && !strncasecmp(p, "content-range:", 14)) {
        const char *slash = memchr(p, '/', len);
        if (slash) s->total = strtoll(slash + 1, NULL, 10);
    }
    return len;
}

// Stops after the first bytes: some servers (Real-Debrid's) answer "bytes=0-0" with the rest of the file.
static size_t first_bytes_only(char *p, size_t sz, size_t n, void *ud) {
    (void)p, (void)sz, (void)n, (void)ud;
    return 0;
}

// The file size from a 1-byte range request; -1 if the server does not do ranges (the installer needs them).
static int64_t remote_size(const char *url, char *err, size_t errlen) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    size_probe s = {-1};
    net_apply_dl(c, NULL); // HTTP/1.1, like the downloader's probe (HTTP/2 stalled on Real-Debrid's servers)
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probe_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &s);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, first_bytes_only);
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_WRITE_ERROR) rc = CURLE_OK; // stopped on purpose
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        snprintf(err, errlen, "%s", curl_easy_strerror(rc));
        return -1;
    }
    if (code != 206 || s.total <= 0) {
        snprintf(err, errlen, "the file server does not support byte ranges (HTTP %ld)", code);
        return -1;
    }
    return s.total;
}

int stream_create(const char *kind, const char *link, const char *name_hint, sbuf *out, char *token_out, char *err,
                  size_t errlen) {
    if (strcmp(kind, "rd") && strcmp(kind, "ad") && strcmp(kind, "url") && !is_tb_kind(kind)) {
        snprintf(err, errlen, "kind must be rd, ad, url, torrents, webdl or usenet");
        return -1;
    }
    if (!is_tb_kind(kind) && strncmp(link, "http://", 7) && strncmp(link, "https://", 8)) {
        snprintf(err, errlen, "the link must start with http:// or https://");
        return -1;
    }
    char *direct = malloc(4096), name[256] = "";
    if (!direct) return -1;
    if (resolve(kind, link, direct, 4096, name, sizeof name, err, errlen)) {
        free(direct);
        return -1;
    }
    int64_t size = remote_size(direct, err, errlen);
    if (size < 0) {
        memset(direct, 0, 4096);
        free(direct);
        return -1;
    }
    if (name_hint && name_hint[0]) snprintf(name, sizeof name, "%s", name_hint);
    if (!name[0] && !is_tb_kind(kind)) { // the last path part of the link
        const char *q = strstr(link, "://");
        const char *path = q ? strchr(q + 3, '/') : NULL;
        if (path) {
            size_t end = strcspn(path, "?#");
            const char *s = path + end;
            while (s > path && s[-1] != '/') s--;
            snprintf(name, sizeof name, "%.*s", (int)(path + end - s), s);
            url_decode(name);
        }
    }
    sanitize_name(name);
    if (!name[0]) snprintf(name, sizeof name, "package.pkg");

    char token[33];
    make_token(token);
    pthread_mutex_lock(&g_lock);
    stream_t *slot = NULL;
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &g_streams[i];
        if (!s->token[0]) {
            slot = s;
            break;
        }
        if (!s->active && !s->ra_running && s->ins != 1 && s->ins != 2 && (!slot || s->last_ms < slot->last_ms))
            slot = s; // the oldest idle one, never a running install
    }
    if (!slot) {
        pthread_mutex_unlock(&g_lock);
        memset(direct, 0, 4096);
        free(direct);
        snprintf(err, errlen, "too many streams in use");
        return -1;
    }
    free(slot->link);
    if (slot->direct) memset(slot->direct, 0, strlen(slot->direct)), free(slot->direct);
    if (slot->token[0]) pthread_cond_destroy(&slot->cv);
    memset(slot, 0, sizeof *slot);
    pthread_cond_init(&slot->cv, NULL);
    for (int i = 0; i < 4; i++) slot->pos[i] = -1;
    snprintf(slot->token, sizeof slot->token, "%s", token);
    snprintf(slot->kind, sizeof slot->kind, "%s", kind);
    snprintf(slot->name, sizeof slot->name, "%s", name);
    slot->link = strdup(link);
    slot->direct = direct;
    slot->size = size;
    slot->created_ms = slot->last_ms = plat_now_ms();
    save_streams_locked();
    pthread_mutex_unlock(&g_lock);

    if (token_out) snprintf(token_out, 33, "%s", token);
    char ename[800], ip[64];
    url_encode(name, ename, sizeof ename);
    plat_local_ip(ip, sizeof ip);
    int port = server_port();
    sb_printf(out, "{\"ok\":true,\"token\":\"%s\",\"size\":%lld,\"name\":", token, (long long)size);
    sb_json_str(out, name);
    sb_printf(out, ",\"url\":\"http://127.0.0.1:%d/stream/%s/%s\",\"lan_url\":\"http://%s:%d/stream/%s/%s\"}", port,
              token, ename, ip, port, token, ename);
    plat_log("stream %.8s: %s, %lld bytes", token, name, (long long)size);
    return 0;
}

// ---------- serving: each installer request is fetched as parallel 2 MiB parts over shared connections ----------
//
// The installer reads front to back in 18 MiB ranges, two at a time. One connection to a debrid server carries
// ~6 MB/s on the console (512 KiB receive window), so each range is split into PART_SIZE parts fetched
// PARTS_IN_FLIGHT at a time and sent back in order: two installer requests then keep ~16 connections busy. A libcurl
// share keeps connections (and TLS sessions) open across requests.

#define PART_SIZE (2ll * 1024 * 1024)
#define PARTS_IN_FLIGHT 8
#define PART_TRIES 8

static CURLSH *g_share;
static pthread_mutex_t g_share_locks[CURL_LOCK_DATA_LAST];
static pthread_once_t g_share_once = PTHREAD_ONCE_INIT;

static void share_lock(CURL *h, curl_lock_data data, curl_lock_access access, void *ud) {
    (void)h, (void)access, (void)ud;
    pthread_mutex_lock(&g_share_locks[data]);
}
static void share_unlock(CURL *h, curl_lock_data data, void *ud) {
    (void)h, (void)ud;
    pthread_mutex_unlock(&g_share_locks[data]);
}
static void share_init(void) {
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&g_share_locks[i], NULL);
    g_share = curl_share_init();
    if (!g_share) return;
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
}

typedef struct {
    int64_t start, end; // inclusive
    char *buf;
    size_t got;
    int state; // 0 waiting to be (re)started, 1 fetching, 2 complete
    int tries;
    int64_t retry_at_ms;
    long code;
    int bad;
    CURL *h;
} part;

static size_t part_hdr(char *p, size_t sz, size_t n, void *ud) {
    part *pt = ud;
    size_t len = sz * n;
    if (len > 5 && !strncasecmp(p, "HTTP/", 5)) {
        const char *sp = memchr(p, ' ', len);
        pt->code = sp ? strtol(sp + 1, NULL, 10) : 0;
    }
    return len;
}

static size_t part_body(char *p, size_t sz, size_t n, void *ud) {
    part *pt = ud;
    size_t len = sz * n;
    if (pt->code != 206) { // only the asked byte range may reach the installer
        pt->bad = 1;
        return 0;
    }
    size_t room = (size_t)(pt->end - pt->start + 1) - pt->got;
    size_t take = len < room ? len : room;
    memcpy(pt->buf + pt->got, p, take);
    pt->got += take;
    return take < len ? 0 : len; // more than asked: stop there
}

static int part_start(CURLM *m, part *pt, const char *direct) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", (long long)(pt->start + (int64_t)pt->got), (long long)pt->end);
    net_apply_dl(c, NULL);
    if (g_share && getenv("TORBOX_STREAM_SHARE")) curl_easy_setopt(c, CURLOPT_SHARE, g_share); // off: slowed the console down
    curl_easy_setopt(c, CURLOPT_URL, direct);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_RANGE, range);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, part_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, pt);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, part_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, pt);
    curl_easy_setopt(c, CURLOPT_PRIVATE, pt);
    pt->code = 0, pt->bad = 0;
    if (curl_multi_add_handle(m, c) != CURLM_OK) {
        curl_easy_cleanup(c);
        return -1;
    }
    pt->h = c;
    pt->state = 1;
    return 0;
}

// A fresh direct link for the stream (after a refusal): updates the stream and the caller's copy.
static void refresh_link(const char *tok, int reqno, const char *kind, const char *link, char **direct) {
    if (!strcmp(kind, "url")) return;
    char *fresh = malloc(4096), err[300] = "", name[256];
    if (!fresh) return;
    if (!resolve(kind, link, fresh, 4096, name, sizeof name, err, sizeof err)) {
        free(*direct);
        *direct = strdup(fresh);
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_STREAMS; i++)
            if (!strcmp(g_streams[i].token, tok)) {
                free(g_streams[i].direct);
                g_streams[i].direct = strdup(fresh);
            }
        pthread_mutex_unlock(&g_lock);
        plat_log("stream %.8s #%d: new direct link", tok, reqno);
    } else plat_log("stream %.8s #%d: new link failed: %s", tok, reqno, err);
    memset(fresh, 0, 4096);
    free(fresh);
}

// Sends bytes a..b of the stream to the installer. Returns the bytes sent; *why says how it ended.
static int64_t serve_range(int fd, stream_t *s, const char *tok, int reqno, const char *kind, const char *link,
                           char **direct, int64_t a, int64_t b, const char **why) {
    pthread_once(&g_share_once, share_init);
    CURLM *m = curl_multi_init();
    if (!m) {
        *why = "out of memory";
        return 0;
    }
    part ring[PARTS_IN_FLIGHT];
    memset(ring, 0, sizeof ring);
    int64_t nparts = (b - a) / PART_SIZE + 1, next_issue = 0, next_send = 0, sent = 0;
    int failed = 0, gone = 0;
    while (next_send < nparts && !failed && !gone) {
        int64_t now = plat_now_ms();
        pthread_mutex_lock(&g_lock);
        int stop = s->stopping;
        pthread_mutex_unlock(&g_lock);
        if (stop) {
            *why = "stopped";
            break;
        }
        // Queue new parts into free ring slots, and restart failed ones once their wait is over.
        while (next_issue < nparts && next_issue - next_send < PARTS_IN_FLIGHT) {
            part *pt = &ring[next_issue % PARTS_IN_FLIGHT];
            if (!pt->buf) pt->buf = malloc(PART_SIZE);
            if (!pt->buf) {
                failed = 1;
                *why = "out of memory";
                break;
            }
            pt->start = a + next_issue * PART_SIZE;
            pt->end = pt->start + PART_SIZE - 1 < b ? pt->start + PART_SIZE - 1 : b;
            pt->got = 0, pt->tries = 0, pt->state = 0, pt->retry_at_ms = 0;
            next_issue++;
        }
        for (int64_t k = next_send; k < next_issue && !failed; k++) {
            part *pt = &ring[k % PARTS_IN_FLIGHT];
            if (pt->state == 0 && now >= pt->retry_at_ms && part_start(m, pt, *direct)) {
                failed = 1;
                *why = "could not start a transfer";
            }
        }
        if (failed) break;
        int running = 0;
        curl_multi_perform(m, &running);
        CURLMsg *msg;
        int left;
        int refused = 0;
        while ((msg = curl_multi_info_read(m, &left))) {
            if (msg->msg != CURLMSG_DONE) continue;
            part *pt = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&pt);
            CURLcode rc = msg->data.result;
            curl_multi_remove_handle(m, msg->easy_handle);
            curl_easy_cleanup(msg->easy_handle);
            if (!pt) continue;
            pt->h = NULL;
            if (pt->got == (size_t)(pt->end - pt->start + 1)) {
                pt->state = 2;
                continue;
            }
            // Dropped, stalled or refused: retry the rest of this part; a refusal gets a fresh link first.
            int refusal = pt->bad || pt->code == 401 || pt->code == 403 || pt->code == 404 || pt->code == 410;
            refused |= refusal;
            if (++pt->tries > PART_TRIES) {
                failed = 1;
                *why = "the file server kept failing";
                break;
            }
            plat_log("stream %.8s #%d: part at %lld: %s (HTTP %ld), retry %d", tok, reqno,
                     (long long)(pt->start + (int64_t)pt->got),
                     rc == CURLE_OK || rc == CURLE_WRITE_ERROR ? "stopped early" : curl_easy_strerror(rc), pt->code,
                     pt->tries);
            pt->state = 0;
            pt->retry_at_ms = plat_now_ms() + (pt->tries > 1 ? 1000 : 100);
        }
        if (refused) refresh_link(tok, reqno, kind, link, direct);
        // Send finished parts to the installer, in order.
        while (next_send < next_issue && ring[next_send % PARTS_IN_FLIGHT].state == 2) {
            part *pt = &ring[next_send % PARTS_IN_FLIGHT];
            if (http_send_raw(fd, pt->buf, pt->got)) {
                gone = 1;
                *why = "installer closed the connection";
                break;
            }
            sent += (int64_t)pt->got;
            pthread_mutex_lock(&g_lock); // counted as it goes, so progress and speed move smoothly
            if (!strcmp(s->token, tok)) s->served += (int64_t)pt->got, s->last_ms = plat_now_ms();
            pthread_mutex_unlock(&g_lock);
            pt->state = 0, pt->got = 0;
            next_send++;
        }
        if (next_send < nparts && !failed && !gone) curl_multi_poll(m, NULL, 0, 100, NULL);
    }
    for (int i = 0; i < PARTS_IN_FLIGHT; i++) {
        if (ring[i].h) {
            curl_multi_remove_handle(m, ring[i].h);
            curl_easy_cleanup(ring[i].h);
        }
        free(ring[i].buf);
    }
    curl_multi_cleanup(m);
    return sent;
}

// ---------- read-ahead: the installer reads front to back, so blocks ahead of it are fetched in advance ----------
//
// One thread per stream keeps up to RA_WIN connections busy, each fetching the next RA_BLOCK block after the
// lowest position an installer request is reading (`base`). Requests are answered from those blocks; parts outside
// the window (the installer's first reads near the end of the file) and small reads go through serve_range.

// Real-Debrid's servers answer every request with "Connection: close", so each block costs a new TLS connection;
// at 8 MiB blocks (~10 new connections a second at 80 MB/s) they started refusing handshakes. 64 MiB blocks make
// that ~1.5 a second; connection errors halve the number of parallel connections for a while (added back one at a
// time). A block is served while it fills (`avail`), so the installer never waits for a whole block.
#define RA_BLOCK (64ll * 1024 * 1024)
#define RA_WIN 16 // up to 1 GiB of read-ahead
#define RA_MIN_CONNS 2
#define RA_TRIES 30
#define RA_IDLE_MS 120000

struct rablock {
    int64_t idx; // block number, -1 = unused
    char *buf;
    size_t len, got;
    size_t avail; // bytes the installer may read: `got` as of the last loop (published under g_lock)
    int state;    // 0 to fetch, 1 fetching, 2 ready, 3 failed
    int tries;
    int64_t retry_at_ms;
    size_t got_at_start; // bytes it had when the current attempt started
    long code;
    int bad;
    CURL *h;
};

static size_t ra_hdr(char *p, size_t sz, size_t n, void *ud) {
    struct rablock *bl = ud;
    size_t len = sz * n;
    if (len > 5 && !strncasecmp(p, "HTTP/", 5)) {
        const char *sp = memchr(p, ' ', len);
        bl->code = sp ? strtol(sp + 1, NULL, 10) : 0;
    }
    return len;
}

static size_t ra_body(char *p, size_t sz, size_t n, void *ud) {
    struct rablock *bl = ud;
    size_t len = sz * n;
    if (bl->code != 206) {
        bl->bad = 1;
        return 0;
    }
    size_t take = len < bl->len - bl->got ? len : bl->len - bl->got;
    memcpy(bl->buf + bl->got, p, take);
    bl->got += take;
    return take < len ? 0 : len;
}

static void ra_drop(CURLM *m, struct rablock *bl) {
    if (!bl->h) return;
    curl_multi_remove_handle(m, bl->h);
    curl_easy_cleanup(bl->h);
    bl->h = NULL;
}

static void *ra_thread(void *arg) {
    stream_t *s = arg;
    char tok[33], kind[12];
    pthread_mutex_lock(&g_lock);
    snprintf(tok, sizeof tok, "%s", s->token);
    snprintf(kind, sizeof kind, "%s", s->kind);
    char *link = strdup(s->link);
    if (!s->win) {
        s->win = calloc(RA_WIN, sizeof *s->win);
        for (int i = 0; s->win && i < RA_WIN; i++) s->win[i].idx = -1;
    }
    struct rablock *win = s->win;
    int64_t nblocks = (s->size + RA_BLOCK - 1) / RA_BLOCK, size = s->size;
    pthread_mutex_unlock(&g_lock);
    CURLM *m = curl_multi_init();
    int64_t idle_since = plat_now_ms(), fetched = 0, calm_since = plat_now_ms();
    int limit = RA_WIN, in_flight = 0; // parallel connections allowed / running
    int64_t report_ms = plat_now_ms(), fetched_at_report = 0;
    plat_log("stream %.8s: read-ahead started", tok);
    while (win && m && link) {
        int64_t now = plat_now_ms();
        pthread_mutex_lock(&g_lock);
        if (strcmp(s->token, tok) || s->ra_quit) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        if (s->active) idle_since = now;
        else if (now - idle_since > RA_IDLE_MS) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        // Slide the window to [base, base + RA_WIN): reuse slots of blocks the installer has passed.
        int64_t base = s->base, end = base + RA_WIN < nblocks ? base + RA_WIN : nblocks;
        for (int64_t k = base; k < end; k++) {
            struct rablock *bl = &win[k % RA_WIN];
            if (bl->idx == k) continue;
            if (bl->h) in_flight--;
            ra_drop(m, bl);
            bl->idx = k, bl->state = 0, bl->got = 0, bl->avail = 0, bl->tries = 0, bl->retry_at_ms = 0;
            bl->len = (size_t)(k * RA_BLOCK + RA_BLOCK <= size ? RA_BLOCK : size - k * RA_BLOCK);
        }
        int moved = 0;
        for (int i = 0; i < RA_WIN; i++)
            if (win[i].avail != win[i].got) s->ra_fetched += (int64_t)(win[i].got - win[i].avail), win[i].avail = win[i].got, moved = 1;
        if (moved) pthread_cond_broadcast(&s->cv);
        {
            int64_t ready = 0, lo = -1;
            for (int i = 0; i < RA_WIN; i++)
                if (win[i].idx >= base) ready += (int64_t)win[i].avail;
            for (int i = 0; i < 4; i++)
                if (s->pos[i] >= 0 && (lo < 0 || s->pos[i] < lo)) lo = s->pos[i];
            if (lo >= base * RA_BLOCK) ready -= lo - base * RA_BLOCK;
            s->ahead = ready > 0 ? ready : 0;
        }
        if (limit < RA_WIN && now - calm_since >= 5000) limit++, calm_since = now; // no connection errors lately
        if (now - report_ms >= 10000) { // what limits the speed: few MB ahead = the network, many = the installer
            int64_t ahead = 0, done_now = fetched;
            for (int i = 0; i < RA_WIN; i++) {
                if (win[i].idx < base) continue;
                ahead += (int64_t)win[i].avail;
                if (win[i].state == 1) done_now += (int64_t)win[i].got;
            }
            int64_t lo = -1;
            for (int i = 0; i < 4; i++)
                if (s->pos[i] >= 0 && (lo < 0 || s->pos[i] < lo)) lo = s->pos[i];
            if (lo >= base * RA_BLOCK) ahead -= lo - base * RA_BLOCK;
            plat_log("stream %.8s: read-ahead %d/%d connections, %lld MB ready ahead, fetching %.1f MB/s", tok,
                     in_flight, limit, (long long)(ahead / 1000000),
                     (done_now - fetched_at_report) / 1e3 / (double)(now - report_ms));
            report_ms = now, fetched_at_report = done_now;
        }
        char *direct = s->direct ? strdup(s->direct) : NULL;
        for (int64_t k = base; k < end && direct && in_flight < limit; k++) {
            struct rablock *bl = &win[k % RA_WIN];
            if (bl->state != 0 || now < bl->retry_at_ms) continue;
            if (!bl->buf) bl->buf = malloc(RA_BLOCK);
            CURL *c = bl->buf ? curl_easy_init() : NULL;
            if (!c) break;
            char range[64];
            snprintf(range, sizeof range, "%lld-%lld", (long long)(k * RA_BLOCK + (int64_t)bl->got),
                     (long long)(k * RA_BLOCK + (int64_t)bl->len - 1));
            net_apply_dl(c, NULL);
            curl_easy_setopt(c, CURLOPT_URL, direct);
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_RANGE, range);
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
            curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, ra_hdr);
            curl_easy_setopt(c, CURLOPT_HEADERDATA, bl);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, ra_body);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, bl);
            curl_easy_setopt(c, CURLOPT_PRIVATE, bl);
            bl->code = 0, bl->bad = 0;
            if (curl_multi_add_handle(m, c) != CURLM_OK) {
                curl_easy_cleanup(c);
                break;
            }
            bl->h = c, bl->state = 1, bl->got_at_start = bl->got;
            in_flight++;
        }
        free(direct);
        pthread_mutex_unlock(&g_lock);

        int running = 0, refused = 0;
        curl_multi_perform(m, &running);
        CURLMsg *msg;
        int left;
        while ((msg = curl_multi_info_read(m, &left))) {
            if (msg->msg != CURLMSG_DONE) continue;
            struct rablock *bl = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char **)&bl);
            CURLcode rc = msg->data.result;
            pthread_mutex_lock(&g_lock);
            in_flight--;
            if (bl) ra_drop(m, bl);
            else curl_multi_remove_handle(m, msg->easy_handle), curl_easy_cleanup(msg->easy_handle);
            if (bl && bl->got == bl->len) {
                s->ra_fetched += (int64_t)(bl->got - bl->avail);
                bl->state = 2, bl->avail = bl->got;
                fetched += (int64_t)bl->len;
            } else if (bl) {
                int refusal = bl->bad || bl->code == 401 || bl->code == 403 || bl->code == 404 || bl->code == 410;
                int conn_err = rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_COULDNT_CONNECT || rc == CURLE_GOT_NOTHING ||
                               rc == CURLE_COULDNT_RESOLVE_HOST || (rc == CURLE_RECV_ERROR && !bl->got);
                refused |= refusal;
                if (conn_err || bl->code == 429 || bl->code == 503) { // the server pushes back: fewer connections
                    int was = limit;
                    limit = limit / 2 > RA_MIN_CONNS ? limit / 2 : RA_MIN_CONNS;
                    calm_since = plat_now_ms();
                    if (limit < was) plat_log("stream %.8s: read-ahead down to %d connections", tok, limit);
                }
                if (bl->got > bl->got_at_start && !conn_err) bl->tries = 0; // it moved: a dropped connection
                bl->state = ++bl->tries > RA_TRIES ? 3 : 0;
                int64_t wait = bl->tries == 1 ? 100 : 500ll << (bl->tries < 6 ? bl->tries : 5);
                bl->retry_at_ms = plat_now_ms() + (wait < 15000 ? wait : 15000);
                if (bl->tries <= 3 || bl->state == 3)
                    plat_log("stream %.8s: read-ahead block %lld: %s (HTTP %ld), %s", tok, (long long)bl->idx,
                             rc == CURLE_OK || rc == CURLE_WRITE_ERROR ? "stopped early" : curl_easy_strerror(rc),
                             bl->code, bl->state == 3 ? "given up" : "retrying");
            }
            pthread_cond_broadcast(&s->cv);
            pthread_mutex_unlock(&g_lock);
        }
        if (refused) {
            char *fresh = NULL; // refresh_link also stores the new link in the stream
            refresh_link(tok, 0, kind, link, &fresh);
            free(fresh);
        }
        curl_multi_poll(m, NULL, 0, 50, NULL);
    }
    pthread_mutex_lock(&g_lock);
    for (int i = 0; win && i < RA_WIN; i++) {
        if (m) ra_drop(m, &win[i]);
        free(win[i].buf);
        win[i].buf = NULL, win[i].idx = -1, win[i].state = 0;
    }
    if (!strcmp(s->token, tok)) {
        free(s->win);
        s->win = NULL;
        s->ra_running = 0;
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&g_lock);
    if (m) curl_multi_cleanup(m);
    free(link);
    plat_log("stream %.8s: read-ahead stopped (%lld MB fetched)", tok, (long long)(fetched / 1000000));
    return NULL;
}

// The window follows the lowest position any open installer request is reading (g_lock held).
static void ra_update_base(stream_t *s) {
    int64_t lo = -1;
    for (int i = 0; i < 4; i++)
        if (s->pos[i] >= 0 && (lo < 0 || s->pos[i] < lo)) lo = s->pos[i];
    if (lo >= 0) s->base = lo / RA_BLOCK;
}

// Sends a..b from the read-ahead window. Returns bytes sent; *next is where it stopped (b + 1 when all sent; less
// when the rest lies outside the window or a block failed, for serve_range to finish); *gone if the installer left.
static int64_t serve_window(int fd, stream_t *s, const char *tok, int64_t a, int64_t b, int64_t *next, int *gone) {
    *next = a, *gone = 0;
    char *tmp = malloc(8 * 1024 * 1024);
    if (!tmp) return 0;
    pthread_mutex_lock(&g_lock);
    int me = -1;
    for (int i = 0; i < 4 && me < 0; i++)
        if (s->pos[i] < 0) me = i;
    if (me < 0 || strcmp(s->token, tok)) {
        pthread_mutex_unlock(&g_lock);
        free(tmp);
        return 0;
    }
    s->pos[me] = a;
    ra_update_base(s);
    if (!s->ra_running) {
        s->ra_running = 1;
        if (plat_start_thread(ra_thread, s)) s->ra_running = 0;
    }
    int64_t off = a, sent = 0;
    while (off <= b && s->ra_running && !s->stopping) {
        int64_t k = off / RA_BLOCK;
        s->pos[me] = off;
        ra_update_base(s);
        if (k >= s->base + RA_WIN) break; // outside the window: another request holds it further back
        struct rablock *bl = s->win ? &s->win[k % RA_WIN] : NULL;
        int64_t waited = plat_now_ms();
        int64_t need = off - k * RA_BLOCK; // offset inside the block
        while (s->ra_running && s->win &&
               !(bl->idx == k && (bl->state == 2 || bl->state == 3 || (int64_t)bl->avail > need)) &&
               plat_now_ms() - waited < 300000) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct timespec ts = {tv.tv_sec, (tv.tv_usec + 200000L) * 1000L};
            if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&s->cv, &g_lock, &ts);
            bl = s->win ? &s->win[k % RA_WIN] : NULL;
            if (!bl) break;
        }
        if (!bl || bl->idx != k || (int64_t)bl->avail <= need) break; // failed or timed out: serve_range takes over
        int64_t from = need, to = b - k * RA_BLOCK < (int64_t)bl->avail - 1 ? b - k * RA_BLOCK : (int64_t)bl->avail - 1;
        if (to - from + 1 > 8 * 1024 * 1024) to = from + 8 * 1024 * 1024 - 1; // tmp holds 8 MiB
        size_t n = (size_t)(to - from + 1);
        memcpy(tmp, bl->buf + from, n);
        pthread_mutex_unlock(&g_lock);
        int fail = http_send_raw(fd, tmp, n);
        pthread_mutex_lock(&g_lock);
        if (fail) {
            *gone = 1;
            break;
        }
        off += (int64_t)n, sent += (int64_t)n;
        if (!strcmp(s->token, tok)) s->served += (int64_t)n, s->last_ms = plat_now_ms();
    }
    s->pos[me] = -1; // the window stays where it is for the installer's next request
    pthread_mutex_unlock(&g_lock);
    free(tmp);
    *next = off;
    return sent;
}

void stream_handle(http_req *r) {
    const char *p = r->path + 8;
    if (strcspn(p, "/") != 32) {
        http_send(r->fd, 404, "text/plain", "not found", 9, NULL);
        return;
    }
    char tok[33], kind[12];
    memcpy(tok, p, 32);
    tok[32] = 0;
    pthread_mutex_lock(&g_lock);
    stream_t *s = NULL;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!strcmp(g_streams[i].token, tok)) s = &g_streams[i];
    if (!s || s->stopping) {
        pthread_mutex_unlock(&g_lock);
        http_send(r->fd, 404, "text/plain", "not found", 9, NULL);
        return;
    }
    if (s->ins == 5) { // the PS5 pulls again after a pause we took for a cancel
        s->ins = 2;
        s->ins_err[0] = 0;
        s->ins_finished_ms = 0;
        plat_log("install %.8s: the PS5 resumed", tok);
    }
    s->active++;
    int reqno = ++s->requests;
    int64_t size = s->size;
    if (s->restored && r->range[0]) { // after a restart: the installer's first big read is where it really is
        long long ra = -1, rb = -1;
        if (sscanf(r->range, "bytes=%lld-%lld", &ra, &rb) == 2 && rb - ra + 1 >= (1 << 20)) {
            if (ra > s->served && ra < size - 256ll * 1024 * 1024) {
                plat_log("install %.8s: progress picked up at %lld (was %lld)", tok, ra, (long long)s->served);
                s->served = s->saved_served = ra;
                s->sp_n = 0; // that jump is not speed: measure afresh
            }
            s->restored = 0;
        }
    }
    snprintf(kind, sizeof kind, "%s", s->kind);
    char *link = strdup(s->link), *direct = s->direct ? strdup(s->direct) : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!direct) { // restored after a restart: get a direct link first
        refresh_link(tok, reqno, kind, link, &direct);
        if (!direct && !strcmp(kind, "url")) direct = strdup(link);
        if (direct && !strcmp(kind, "url")) {
            pthread_mutex_lock(&g_lock);
            if (!s->direct) s->direct = strdup(direct);
            pthread_mutex_unlock(&g_lock);
        }
        if (!direct) {
            http_send(r->fd, 502, "text/plain", "no link", 7, NULL);
            pthread_mutex_lock(&g_lock);
            s->active--;
            pthread_mutex_unlock(&g_lock);
            free(link);
            return;
        }
    }

    int64_t a = 0, b = size - 1;
    int partial = 0;
    long long x = -1, y = -1;
    if (r->range[0]) {
        if (sscanf(r->range, "bytes=%lld-%lld", &x, &y) >= 1 && x >= 0) {
            a = x;
            if (y >= 0) b = y;
            partial = 1;
        } else if (sscanf(r->range, "bytes=-%lld", &y) == 1 && y > 0) {
            a = size > y ? size - y : 0;
            partial = 1;
        }
    }
    if (b >= size) b = size - 1;
    int verbose = reqno <= 40 || reqno % 100 == 0; // the installer sends hundreds of requests
    if (verbose) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        char ip[64] = "?";
        if (!getpeername(r->fd, (struct sockaddr *)&peer, &pl)) inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        plat_log("stream %.8s #%d: %s %s from %s", tok, reqno, r->method, r->range[0] ? r->range : "(whole file)", ip);
    }

    int64_t sent = 0, t0 = plat_now_ms();
    const char *why = "done";
    char head[512];
    if (a > b || a >= size) {
        snprintf(head, sizeof head, "Content-Range: bytes */%lld\r\n", (long long)size);
        http_send(r->fd, 416, "text/plain", "", 0, head);
        why = "range not satisfiable";
        goto out;
    }
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
                     "Accept-Ranges: bytes\r\nConnection: close\r\n",
                     partial ? 206 : 200, partial ? "Partial Content" : "OK", (long long)(b - a + 1));
    if (partial) n += snprintf(head + n, sizeof head - n, "Content-Range: bytes %lld-%lld/%lld\r\n", (long long)a,
                               (long long)b, (long long)size);
    n += snprintf(head + n, sizeof head - n, "\r\n");
    if (http_send_raw(r->fd, head, (size_t)n)) {
        why = "installer gone";
        goto out;
    }
    if (strcmp(r->method, "HEAD")) {
        int64_t next = a;
        int gone = 0;
        if (b - a + 1 >= (1 << 20)) sent = serve_window(r->fd, s, tok, a, b, &next, &gone); // large reads
        pthread_mutex_lock(&g_lock);
        int stopping = s->stopping;
        pthread_mutex_unlock(&g_lock);
        if (gone) why = "installer closed the connection";
        else if (stopping) why = "stopped";
        else if (next <= b) sent += serve_range(r->fd, s, tok, reqno, kind, link, &direct, next, b, &why);
    }
out:;
    int64_t ms = plat_now_ms() - t0;
    if (verbose || strcmp(why, "done"))
        plat_log("stream %.8s #%d: %s, %lld bytes in %.1f s (%.1f MB/s)", tok, reqno, why, (long long)sent,
                 ms / 1000.0, ms > 0 ? sent / 1e3 / ms : 0.0);
    if (direct) memset(direct, 0, strlen(direct)), free(direct);
    free(link);
    pthread_mutex_lock(&g_lock);
    if (!strcmp(s->token, tok)) {
        s->active--;
        s->last_ms = plat_now_ms();
        if (!strcmp(why, "installer closed the connection") && a + sent <= b) s->gone_ms = s->last_ms;
        if (sent > 0 && a + sent > b && b == size - 1 && s->served >= size * 9 / 10) s->end_seen = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

// ---------- installing: shsrv starts it, a monitor finishes it from the stream's progress ----------
//
// The system installer (sceAppInstUtilInstallByPackage) only works from a process spawned into a system image, as
// shsrv's `pkg_install` does; the same call from this payload is refused (PlayGo 0x80B2116F). So the server asks
// shsrv, on the console's shell port, to start the install pointed at our stream URL, and reads back the content id
// it prints. The PS5 then pulls the package from the stream; a monitor thread marks the install done when the whole
// package has been delivered, or failed if the PS5 stops pulling it.

#define SHSRV_PORT 2323

// Sends "pkg_install <url>" to shsrv and reads the content id it prints. 0 if shsrv accepted the command.
static int shsrv_start(const char *url, char *content_id, size_t cidlen, char *err, size_t errlen) {
    content_id[0] = 0;
    int port = SHSRV_PORT;
#ifndef __PROSPERO__
    const char *env = getenv("TORBOX_SHSRV_PORT"); // host tests run a fake shsrv
    if (env && atoi(env) > 0) port = atoi(env);
#endif
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sock < 0 || connect(sock, (struct sockaddr *)&a, sizeof a)) {
        if (sock >= 0) close(sock);
        snprintf(err, errlen, "the console's shell server (shsrv, port %d) is not running", port);
        return -1;
    }
    char cmd[4200];
    int n = snprintf(cmd, sizeof cmd, "pkg_install %s\nexit\n", url);
    if (http_send_raw(sock, cmd, (size_t)n)) {
        close(sock);
        snprintf(err, errlen, "could not talk to shsrv");
        return -1;
    }
    // Read until the shell closes or the content id appears. shsrv prints "... content_id = [ID] ..." then returns.
    char buf[8192];
    size_t got = 0;
    int64_t t0 = plat_now_ms();
    while (got + 1 < sizeof buf && plat_now_ms() - t0 < 30000) {
        struct pollfd pfd = {sock, POLLIN, 0};
        if (poll(&pfd, 1, 1000) <= 0) {
            if (strstr(buf, "content_id")) break;
            continue;
        }
        ssize_t r = recv(sock, buf + got, sizeof buf - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
        buf[got] = 0;
        if (strstr(buf, "content_id")) break;
    }
    close(sock);
    const char *p = strstr(buf, "content_id = [");
    if (p) {
        p += 14;
        size_t k = strcspn(p, "]");
        if (k && k < cidlen) {
            memcpy(content_id, p, k);
            content_id[k] = 0;
        }
    }
    if (strstr(buf, "command not found") || strstr(buf, "not found")) {
        snprintf(err, errlen, "this shsrv build has no pkg_install command");
        return -1;
    }
    return 0; // the command was sent; the install proceeds even if no content id was parsed
}

// Finishes installs the shell started: done once the whole package has been pulled, failed if the PS5 stops pulling.
static void *monitor_thread(void *arg) {
    (void)arg;
    int64_t quiet_done = 8000, quiet_stall = 180000; // after the last request: delivered / stopped
    int64_t quiet_cancel = 15000; // after the installer closed a connection mid-transfer: cancelled on the PS5
    int64_t save_every = 5000;    // how often progress is saved for restarts
#ifndef __PROSPERO__
    const char *env = getenv("TORBOX_INSTALL_QUIET_MS"); // host tests
    if (env && atoi(env) > 0) quiet_done = atoi(env);
    env = getenv("TORBOX_INSTALL_STALL_MS");
    if (env && atoi(env) > 0) quiet_stall = atoi(env);
    env = getenv("TORBOX_INSTALL_CANCEL_MS");
    if (env && atoi(env) > 0) quiet_cancel = atoi(env);
    env = getenv("TORBOX_INSTALL_SAVE_MS");
    if (env && atoi(env) > 0) save_every = atoi(env);
#endif
    for (;;) {
        plat_sleep_ms(quiet_done < 3000 ? 500 : 1000);
        pthread_mutex_lock(&g_lock);
        int64_t now = plat_now_ms();
        for (int i = 0; i < MAX_STREAMS; i++) {
            stream_t *s = &g_streams[i];
            int last = s->sp_n ? (s->sp_n - 1) % SP_RING : -1;
            if (s->token[0] && (last < 0 || now - s->sp_ms[last] >= 900)) { // one sample a second
                int k = s->sp_n % SP_RING, oldest = s->sp_n >= SP_RING ? (s->sp_n + 1) % SP_RING : 0;
                s->sp_ms[k] = now, s->sp_served[k] = s->served, s->sp_fetched[k] = s->ra_fetched;
                s->sp_n++;
                if (s->sp_n >= 2) {
                    double dt = (double)(now - s->sp_ms[oldest]) / 1000.0;
                    s->speed = dt > 0 ? (double)(s->served - s->sp_served[oldest]) / dt : 0;
                    s->net_speed = dt > 0 ? (double)(s->ra_fetched - s->sp_fetched[oldest]) / dt : 0;
                    if (s->speed < 0) s->speed = 0;
                    if (s->net_speed < 0) s->net_speed = 0;
                }
            }
            if (s->stopping) { // Stop pressed: remove it once nothing uses it any more
                s->ra_quit = 1;
                if (!s->active && !s->ra_running) {
                    plat_log("install %.8s: stopped and removed", s->token);
                    clear_slot_locked(s);
                    save_streams_locked();
                }
                continue;
            }
            if (s->ins != 2) continue;
            s->ins_done = s->served;
            s->ins_total = s->size;
            if (s->active) s->ins_report_ms = now; // the PS5 is pulling: that is its news
            else if (s->last_ms > s->ins_report_ms) s->ins_report_ms = s->last_ms;
            // The installer reads the package's end (its metadata) first and skips it in the main pass, so after a
            // restart (progress picked up from its read position) the count ends short of the size: 98% then quiet
            // is delivered. A cancel is told apart by the installer hanging up mid-transfer (checked before this).
            int delivered = s->size > 0 && (s->served >= s->size - s->size / 50 || s->end_seen);
            if (now - s->saved_ms >= save_every && s->served != s->saved_served) { // keep progress across restarts
                s->saved_served = s->served, s->saved_ms = now;
                save_streams_locked();
            }
            int64_t quiet = now - (s->last_ms > s->ins_started_ms ? s->last_ms : s->ins_started_ms);
            int cancelled = s->gone_ms && s->gone_ms >= s->last_ms - 2000 && s->served < s->size;
            if (delivered && !cancelled && !s->active && quiet > quiet_done) {
                s->ins = 3;
                s->ins_finished_ms = now;
                snprintf(s->ins_status, sizeof s->ins_status, "delivered");
                save_streams_locked();
                plat_log("install %.8s: delivered %s (%lld bytes)", s->token, s->name, (long long)s->served);
            } else if (!delivered && !s->active && s->gone_ms && s->gone_ms >= s->last_ms - 2000 &&
                       now - s->last_ms > quiet_cancel) {
                s->ins = 5; // the installer hung up mid-transfer and has not come back: cancelled on the PS5
                s->ins_finished_ms = now;
                snprintf(s->ins_err, sizeof s->ins_err,
                         "stopped on the PS5 at %.1f of %.1f GB (cancelled there, or the install failed)",
                         s->served / 1e9, s->size / 1e9);
                save_streams_locked();
                plat_log("install %.8s: stopped on the PS5 at %lld/%lld", s->token, (long long)s->served,
                         (long long)s->size);
            } else if (!delivered && !s->active && quiet > quiet_stall) {
                s->ins = 4;
                s->ins_finished_ms = now;
                snprintf(s->ins_err, sizeof s->ins_err,
                         "the PS5 stopped downloading the package at %lld of %lld bytes", (long long)s->served,
                         (long long)s->size);
                save_streams_locked();
                plat_log("install %.8s: stalled at %lld/%lld", s->token, (long long)s->served, (long long)s->size);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void start_monitor(void) { plat_start_thread(monitor_thread, NULL); }

int stream_install(const char *kind, const char *link, const char *name, sbuf *out, char *err, size_t errlen) {
    sbuf tmp;
    sb_init(&tmp);
    char token[33] = "";
    int rc = stream_create(kind, link, name, &tmp, token, err, errlen);
    sb_free(&tmp);
    if (rc) return -1;
    pthread_mutex_lock(&g_lock);
    stream_t *s = NULL;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (!strcmp(g_streams[i].token, token)) s = &g_streams[i];
    if (!s) {
        pthread_mutex_unlock(&g_lock);
        snprintf(err, errlen, "stream lost");
        return -1;
    }
    int64_t size = s->size;
    char ename[800], url[4200];
    url_encode(s->name, ename, sizeof ename);
    pthread_mutex_unlock(&g_lock);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/stream/%s/%s", server_port(), token, ename);

    // The game needs about its package's size, plus the reserve the internal storage keeps while writing.
    struct statvfs vs;
    if (!statvfs("/user", &vs)) {
        double free_b = (double)vs.f_bavail * vs.f_frsize, need = size * 1.2;
        if (free_b < need) {
            snprintf(err, errlen, "not enough space on the console: about %.1f GB needed, %.1f GB free", need / 1e9,
                     free_b / 1e9);
            pthread_mutex_lock(&g_lock);
            s->ins = 4;
            snprintf(s->ins_err, sizeof s->ins_err, "%s", err);
            s->ins_finished_ms = plat_now_ms();
            save_streams_locked();
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
    }
    pthread_mutex_lock(&g_lock);
    s->ins = 1;
    s->ins_started_ms = s->ins_report_ms = s->last_ms = plat_now_ms();
    save_streams_locked();
    pthread_mutex_unlock(&g_lock);
    plat_log("install %.8s: %s, %lld bytes", token, s->name, (long long)size);

    char cid[48] = "";
    if (shsrv_start(url, cid, sizeof cid, err, errlen)) {
        plat_log("install %.8s: not started: %s", token, err);
        pthread_mutex_lock(&g_lock);
        s->ins = 4;
        snprintf(s->ins_err, sizeof s->ins_err, "%s", err);
        s->ins_finished_ms = plat_now_ms();
        save_streams_locked();
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_once(&g_monitor_once, start_monitor);
    pthread_mutex_lock(&g_lock);
    s->ins = 2;
    s->ins_report_ms = s->last_ms = plat_now_ms();
    if (cid[0]) snprintf(s->content_id, sizeof s->content_id, "%s", cid);
    snprintf(s->ins_status, sizeof s->ins_status, "downloading");
    save_streams_locked();
    pthread_mutex_unlock(&g_lock);
    plat_log("install %.8s: started by shsrv%s%s", token, cid[0] ? ", content id " : "", cid);
    sb_printf(out, "{\"ok\":true,\"token\":\"%s\"}", token);
    return 0;
}

static void clear_slot_locked(stream_t *s) {
    free(s->link);
    if (s->direct) memset(s->direct, 0, strlen(s->direct)), free(s->direct);
    pthread_cond_destroy(&s->cv);
    memset(s, 0, sizeof *s);
}

// Removes a finished or failed install (or a plain stream) from the list. force: a running install is stopped
// first (no more data for the PS5, so its install stops there) and removed once nothing uses it.
int stream_remove(const char *id, int force) {
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &g_streams[i];
        if (!s->token[0] || strncmp(s->token, id, strlen(id)) || strlen(id) < 8) continue;
        if (s->active || s->ins == 1 || s->ins == 2) {
            if (!force) break;
            s->stopping = 1, s->ra_quit = 1;
            s->ins = 5;
            s->ins_finished_ms = plat_now_ms();
            snprintf(s->ins_err, sizeof s->ins_err, "stopped from this page");
            pthread_cond_broadcast(&s->cv);
            save_streams_locked();
            plat_log("install %.8s: stop requested", s->token);
            pthread_mutex_unlock(&g_lock);
            pthread_once(&g_monitor_once, start_monitor); // it removes the stream once it is idle
            return 0;
        }
        s->ra_quit = 1; // the read-ahead thread idles a while after the last request: stop it first
        for (int w = 0; s->ra_running && w < 50; w++) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct timespec ts = {tv.tv_sec, (tv.tv_usec + 100000L) * 1000L};
            if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&s->cv, &g_lock, &ts);
        }
        if (s->ra_running) {
            s->ra_quit = 0;
            break;
        }
        clear_slot_locked(s);
        save_streams_locked();
        rc = 0;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void stream_list_json(sbuf *out) {
    pthread_mutex_lock(&g_lock);
    int64_t now = plat_now_ms();
    sb_puts(out, "{\"ok\":true,\"streams\":[");
    int first = 1;
    for (int i = 0; i < MAX_STREAMS; i++) {
        stream_t *s = &g_streams[i];
        if (!s->token[0]) continue;
        sb_printf(out, "%s{\"id\":\"%.8s\",\"kind\":\"%s\",\"size\":%lld,\"served\":%lld,\"requests\":%d,\"active\":%d,"
                       "\"idle_s\":%lld,\"install\":\"%s\",\"ins_done\":%lld,\"ins_total\":%lld,"
                       "\"ins_age_s\":%lld,\"report_age_s\":%lld,\"ins_time_s\":%lld,\"speed\":%.0f,"
                       "\"net_speed\":%.0f,\"ahead\":%lld,\"speed_ok\":%s,\"name\":",
                  first ? "" : ",", s->token, s->kind, (long long)s->size, (long long)s->served, s->requests, s->active,
                  (long long)((now - s->last_ms) / 1000), ins_names[s->ins], (long long)s->ins_done,
                  (long long)s->ins_total, s->ins ? (long long)((now - s->ins_started_ms) / 1000) : -1,
                  s->ins ? (long long)((now - s->ins_report_ms) / 1000) : -1,
                  s->ins_finished_ms > s->ins_started_ms ? (long long)((s->ins_finished_ms - s->ins_started_ms) / 1000)
                                                         : -1,
                  s->speed, s->net_speed, (long long)s->ahead, s->sp_n >= 11 ? "true" : "false"); // >= 10 s measured
        sb_json_str(out, s->name);
        sb_puts(out, ",\"status\":");
        sb_json_str(out, s->ins_status);
        sb_puts(out, ",\"error\":");
        sb_json_str(out, s->ins_err);
        sb_puts(out, ",\"content_id\":");
        sb_json_str(out, s->content_id);
        sb_puts(out, "}");
        first = 0;
    }
    sb_puts(out, "]}");
    pthread_mutex_unlock(&g_lock);
}
