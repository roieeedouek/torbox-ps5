// Download queue: curl_multi transfer engine with decoupled async disk writes, byte ranges of up to 512 MiB,
// tail splitting, per-host AIMD congestion control, USB/exFAT sequential sliding-window mode, and TorBox
// CDN rotation.
//
// A job writes <folder>/<name>.part; a multi-connection job also keeps <name>.part.state (the segment table),
// so a paused, failed or interrupted job (even across restarts) resumes where it stopped. Finished: renamed
// to <name>. TorBox/Real-Debrid links are requested when the job starts (and again after an error), never stored.
#include "app.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { J_QUEUED, J_STARTING, J_RUNNING, J_DONE, J_FAILED, J_PAUSED, J_CANCELLED };
static const char *state_names[] = {"queued", "starting", "downloading", "done", "failed", "paused", "cancelled"};

#define MAX_JOBS 1000
// Max connections per file, also Auto's ceiling. On the console TorBox gave ~52 MB/s with 8 and ~100 MB/s with 16;
// the AIMD window still starts at 8 and halves on 429/5xx/timeouts.
#define MAX_CONNS 16
#define MAX_SEGS 256                     // segment table entries (grow when idle connections split tails)
#define MULTI_MIN (8ll * 1024 * 1024)    // smaller files use one connection
#define CHUNK_CAP (512ll * 1024 * 1024)  // large range ceiling cuts CDN req/min 32x while claim_seg() still tail-splits
#define SPLIT_MIN (2ll * 1024 * 1024)    // min remaining bytes in a segment before an idle slot splits it
#define USB_CHUNK (1024 * 1024)          // 1 MiB sliding-window chunk on /mnt/usb* (exFAT)
#define USB_WIN_SLOTS 8                  // at most 8 MiB of in-flight USB window buffers

typedef struct job {
    int id, state;
    volatile int stop; // 1 = pause, 2 = cancel (delete the partial file)
    char kind[12];
    long long item_id, file_id;
    char *url;    // kind "url": the link; kind "rd": the hoster link Real-Debrid unrestricts
    char *direct; // kind "rd": the last unrestricted (direct) link, kept in memory only: it expires
    int max_conns; // the server's limit on connections per file (Real-Debrid's "chunks"), 0 = none
    char folder[1024], subdir[512], name[256], path[1800];
    int64_t total, done, start_done, started_ms, finished_ms;
    double speed, disk_speed;
    size_t buffered;
    unsigned write_max_ms;
    int conns, active, waiting, paused_conns, win, rcvbuf, usb_mode;
    long last_code;
    char http_ver[8], cdn[128];
    char err[300], note[160];
} job;

static job *g_jobs[MAX_JOBS];
static int g_njobs, g_next_id = 1, g_running;
static int g_parallel = 1, g_connections = 0; // 0 = Auto (up to MAX_CONNS, adapted by AIMD)
static volatile int g_quit;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static int test_env_int(const char *name, int def) {
#ifdef __PROSPERO__
    (void)name;
    return def;
#else
    const char *v = getenv(name);
    return v && *v && atoi(v) > 0 ? atoi(v) : def;
#endif
}

// ---------- persistence: queue.tsv (unfinished jobs) and settings.txt ----------

static void tsv_field(FILE *f, const char *s, int last) {
    for (const char *p = s ? s : ""; *p; p++) fputc(*p == '\t' || *p == '\n' || *p == '\r' ? ' ' : *p, f);
    fputc(last ? '\n' : '\t', f);
}

static void save_queue_locked(void) {
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s/queue.tsv", plat_app_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < g_njobs; i++) {
        job *j = g_jobs[i];
        if (j->state == J_DONE || j->state == J_CANCELLED) continue;
        int st = j->state == J_FAILED ? J_FAILED : j->state == J_PAUSED ? J_PAUSED : J_QUEUED;
        fprintf(f, "%d\t%s\t%lld\t%lld\t", st, j->kind, j->item_id, j->file_id);
        tsv_field(f, j->folder, 0), tsv_field(f, j->subdir, 0), tsv_field(f, j->name, 0), tsv_field(f, j->url, 1);
    }
    fclose(f);
    rename(tmp, path);
}

static void save_settings_locked(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/settings.txt", plat_app_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "parallel=%d\nconnections=%d\n", g_parallel, g_connections);
    fclose(f);
}

static void load_settings(void) {
    char path[600], line[200];
    snprintf(path, sizeof path, "%s/settings.txt", plat_app_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "parallel=%d", &v) == 1 && v >= 1 && v <= 3) g_parallel = v;
        if (sscanf(line, "connections=%d", &v) == 1 && v >= 0 && v <= MAX_CONNS) g_connections = v;
    }
    fclose(f);
}

static job *new_job_locked(void) {
    if (g_njobs == MAX_JOBS) {
        int i;
        for (i = 0; i < g_njobs; i++)
            if (g_jobs[i]->state >= J_DONE && g_jobs[i]->state != J_PAUSED) break;
        if (i == g_njobs) return NULL;
        free(g_jobs[i]->url), free(g_jobs[i]->direct), free(g_jobs[i]);
        memmove(g_jobs + i, g_jobs + i + 1, (g_njobs - i - 1) * sizeof *g_jobs);
        g_njobs--;
    }
    job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->id = g_next_id++;
    j->total = -1;
    g_jobs[g_njobs++] = j;
    return j;
}

static void build_path(job *j) {
    if (j->subdir[0]) snprintf(j->path, sizeof j->path, "%s/%s/%s", j->folder, j->subdir, j->name);
    else snprintf(j->path, sizeof j->path, "%s/%s", j->folder, j->name);
}

static void state_path(const job *j, char *out, size_t len) { snprintf(out, len, "%s.part.state", j->path); }

static void peek_state(job *j) {
    char part[1900], sp[1900];
    snprintf(part, sizeof part, "%s.part", j->path);
    struct stat st;
    if (stat(part, &st) != 0 || st.st_size <= 0) return;
    state_path(j, sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return;
    long long t = -1;
    int n = 0;
    int ok = fscanf(f, "torbox-ps5 1 %lld %d\n", &t, &n) == 2 && t > 0 && n >= 1 && n <= MAX_SEGS;
    int64_t left = 0;
    for (int i = 0; ok && i < n; i++) {
        long long s, e, p;
        ok = fscanf(f, "%lld %lld %lld\n", &s, &e, &p) == 3 && s >= 0 && s <= e + 1 && p >= s && p <= e + 1;
        if (ok && p <= e) left += (int64_t)(e + 1 - p);
    }
    fclose(f);
    if (!ok) return;
    j->total = t;
    j->done = t > left ? t - left : 0;
}

static void load_queue(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/queue.tsv", plat_app_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *line = malloc(16384);
    while (line && fgets(line, 16384, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fields[8] = {0};
        int n = 0;
        for (char *p = line; n < 8; n++) {
            fields[n] = p;
            char *t = strchr(p, '\t');
            if (!t) {
                n++;
                break;
            }
            *t = 0, p = t + 1;
        }
        if (n < 7) continue;
        job *j = new_job_locked();
        if (!j) break;
        j->state = atoi(fields[0]);
        if (j->state != J_FAILED && j->state != J_PAUSED) j->state = J_QUEUED;
        snprintf(j->kind, sizeof j->kind, "%s", fields[1]);
        j->item_id = atoll(fields[2]), j->file_id = atoll(fields[3]);
        snprintf(j->folder, sizeof j->folder, "%s", fields[4]);
        snprintf(j->subdir, sizeof j->subdir, "%s", fields[5]);
        snprintf(j->name, sizeof j->name, "%s", fields[6]);
        if (n > 7 && fields[7][0]) j->url = strdup(fields[7]);
        build_path(j);
        peek_state(j);
        snprintf(j->note, sizeof j->note, "restored after restart");
    }
    free(line);
    fclose(f);
}

// ---------- transfers (curl_multi + async writer) ----------

typedef struct seg {
    int64_t start, end;    // inclusive range
    volatile int64_t pos;  // next byte to commit on disk (written by writer thread)
    int64_t seen;          // received from network (network thread only)
    int64_t retry_at_ms;   // earliest time this segment can be retried
    int64_t last_data_ms;
    int attempts;          // transport error attempts for this segment
    int busy_retries;      // 429/503 retries for this segment
    int owned;
    int waiting;           // currently waiting out a 429/503/retry backoff
} seg;

typedef struct xfer xfer;

typedef struct slot {
    xfer *x;
    CURL *h;
    int seg_idx;
    int active;
    int paused;    // paused via CURL_WRITEFUNC_PAUSE waiting for a free writer block
    int cut_done;  // tail-stolen past new end: handle ready to be removed from multi
    int checked;   // response headers verified on first body callback

    int64_t req_start, req_end, cur_off;

    // Standard mode: 128 KiB writer block staged before handing to writer_enqueue()
    wblock *block;
    int64_t block_off;
    size_t block_used;

    // USB sequential mode: 1 MiB chunk buffer drained into writer in ascending offset order
    char *win_buf;
    int64_t chunk_start, chunk_end;
    size_t win_used, win_drained;
    int usb_assigned, usb_ready;
    int usb_attempts, usb_busy_retries;
    int64_t usb_retry_at_ms;

    struct curl_slist *hdrs;

    // Response header state
    int sent_if_range;
    long resp_code;
    int resp_range_ok, resp_encoding_bad, resp_html;
    int bad_resp, validator_mismatch;
    int64_t resp_start, resp_end, resp_total, resp_clen;
    int resp_retry_after;
    char resp_etag[256], resp_lastmod[128];
    char err[256];
} slot;

struct xfer {
    job *j;
    const char *url;
    int fd;
    int single, usb_mode, is_debrid;
    int64_t total;
    int nseg;
    seg segs[MAX_SEGS];

    CURLM *multi;
    writer *w;
    pthread_mutex_t state_lock; // protects segs[0..nseg] reads during writer's 5s checkpoint
    slot slots[MAX_CONNS];
    int nslots;

    // AIMD congestion window
    int max_conns, win_allowed, win_congested;
    int64_t win_grown_ms, win_shrunk_ms, win_paused_until_ms;

    // USB sliding window cursors
    int64_t usb_cursor, usb_next_issue;

    // Diagnostics & stall watchdog
    net_sock_info sock_info;
    int rcvbuf;
    char etag[256], last_modified[128], http_ver[8], cdn[128];
    long http_error, last_code;
    int relink_needed, no_if_range, failed;
    char err[256];
    int64_t last_progress_ms, stall_ms, low_speed_time;
    int64_t grace_until_ms;
};

// Returns 0 on success, -1 missing/unreadable, -2 total mismatch, -3 corrupt format.
static int load_state(xfer *x, int64_t total, int64_t *state_total) {
    if (state_total) *state_total = -1;
    char path[1900];
    state_path(x->j, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long long t = -1;
    int n = 0;
    int hdr_ok = fscanf(f, "torbox-ps5 1 %lld %d\n", &t, &n) == 2;
    if (hdr_ok && state_total) *state_total = t;
    if (!hdr_ok) {
        fclose(f);
        return -3;
    }
    if (t != total) {
        fclose(f);
        return -2;
    }
    int ok = n >= 1 && n <= MAX_SEGS;
    for (int i = 0; ok && i < n; i++) {
        long long s, e, p;
        ok = fscanf(f, "%lld %lld %lld\n", &s, &e, &p) == 3 && s >= 0 && s <= e + 1 && p >= s && p <= e + 1;
        x->segs[i].start = s, x->segs[i].end = e, x->segs[i].pos = p;
    }
    fclose(f);
    if (!ok) return -3;
    int k = 0;
    for (int i = 0; i < n; i++)
        if (x->segs[i].pos <= x->segs[i].end) x->segs[k++] = x->segs[i];
    for (int i = 0; i < k; i++) x->segs[i].seen = x->segs[i].pos;
    if (k == 0) {
        x->segs[0].start = x->segs[0].pos = x->segs[0].seen = total, x->segs[0].end = total - 1;
        k = 1;
    }
    x->nseg = k;
    return 0;
}

static int save_state(xfer *x, int64_t total) {
    if (total <= 0) return -1;
    char path[1900], tmp[1920];
    state_path(x->j, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        plat_log("job %d: save_state open failed: %s", x->j->id, strerror(errno));
        return -1;
    }
    pthread_mutex_lock(&x->state_lock);
    int n = x->nseg;
    int valid = n >= 1 && n <= MAX_SEGS;
    if (valid) fprintf(f, "torbox-ps5 1 %lld %d\n", (long long)total, n);
    for (int i = 0; valid && i < n; i++) {
        int64_t s = x->segs[i].start;
        int64_t end = __atomic_load_n(&x->segs[i].end, __ATOMIC_ACQUIRE);
        int64_t pos = __atomic_load_n(&x->segs[i].pos, __ATOMIC_RELAXED);
        if (s < 0 || s > end + 1) {
            valid = 0;
            break;
        }
        if (pos < s) pos = s;
        if (pos > end + 1) pos = end + 1;
        fprintf(f, "%lld %lld %lld\n", (long long)s, (long long)end, (long long)pos);
    }
    pthread_mutex_unlock(&x->state_lock);
    int err = !valid || ferror(f) || fflush(f) != 0 || fsync(fileno(f)) != 0;
    if (fclose(f) != 0) err = 1;
    if (err || rename(tmp, path) != 0) {
        plat_log("job %d: save_state failed (valid=%d): %s", x->j->id, valid, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void on_writer_commit(void *ctx, int seg_idx, int64_t new_pos) {
    xfer *x = ctx;
    if (seg_idx < 0 || seg_idx >= MAX_SEGS) return;
    int64_t cur = __atomic_load_n(&x->segs[seg_idx].pos, __ATOMIC_RELAXED);
    if (new_pos > cur) __atomic_store_n(&x->segs[seg_idx].pos, new_pos, __ATOMIC_RELEASE);
}

static void on_writer_state(void *ctx) {
    xfer *x = ctx;
    if (!x->single) (void)save_state(x, x->total);
}

static int64_t xfer_done(xfer *x, int seen) {
    if (x->single) {
        int64_t p = __atomic_load_n(&x->segs[0].pos, __ATOMIC_RELAXED);
        int64_t r = x->segs[0].seen;
        return seen && r > p ? r : p;
    }
    if (x->usb_mode) {
        if (!seen) return __atomic_load_n(&x->segs[0].pos, __ATOMIC_RELAXED);
        int64_t total_seen = x->usb_cursor;
        for (int i = 0; i < x->nslots; i++) {
            slot *sl = &x->slots[i];
            if (sl->usb_assigned && sl->win_used > sl->win_drained)
                total_seen += (int64_t)(sl->win_used - sl->win_drained);
        }
        return total_seen;
    }
    int64_t left = 0;
    for (int i = 0; i < x->nseg; i++) {
        int64_t end = x->segs[i].end;
        int64_t p = __atomic_load_n(&x->segs[i].pos, __ATOMIC_RELAXED);
        int64_t r = x->segs[i].seen;
        int64_t at = seen && r > p ? r : p;
        if (at <= end) left += end + 1 - at;
    }
    return x->total - left;
}

static void record_http_version(xfer *x, CURL *h) {
    if (x->http_ver[0]) return;
    long v = 0;
    if (curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &v) != CURLE_OK) return;
    if (v == CURL_HTTP_VERSION_1_0) snprintf(x->http_ver, sizeof x->http_ver, "1.0");
    else if (v == CURL_HTTP_VERSION_1_1) snprintf(x->http_ver, sizeof x->http_ver, "1.1");
    else if (v == CURL_HTTP_VERSION_2_0 || v == CURL_HTTP_VERSION_2TLS || v == CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE)
        snprintf(x->http_ver, sizeof x->http_ver, "2.0");
    else if (v == CURL_HTTP_VERSION_3 || v == CURL_HTTP_VERSION_3ONLY)
        snprintf(x->http_ver, sizeof x->http_ver, "3");
}

static void trim_hdr(const char *s, size_t n, char *out, size_t cap) {
    while (n && (*s == ' ' || *s == '\t')) s++, n--;
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static int parse_retry_after(const char *val) {
    char *end = NULL;
    long sec = strtol(val, &end, 10);
    if (end && end != val && (*end == 0 || *end == ' ')) {
        if (sec < 0) return 0;
        return sec > 3600 ? 3600 : (int)sec;
    }
    time_t target = curl_getdate(val, NULL);
    if (target != (time_t)-1) {
        time_t now = time(NULL);
        if (target > now) {
            long diff = (long)(target - now);
            return diff > 3600 ? 3600 : (int)diff;
        }
    }
    return 0;
}

static size_t slot_header(char *p, size_t sz, size_t n, void *ud) {
    slot *sl = ud;
    size_t len = sz * n;
    char line[512];
    size_t l = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, p, l);
    line[l] = 0;

    if (!strncasecmp(line, "HTTP/", 5)) {
        sl->resp_code = 0;
        sl->resp_range_ok = sl->resp_encoding_bad = sl->resp_html = 0;
        sl->resp_start = sl->resp_end = sl->resp_total = sl->resp_clen = -1;
        sl->resp_retry_after = 0;
        sl->resp_etag[0] = sl->resp_lastmod[0] = 0;
        char *sp = strchr(line, ' ');
        if (sp) sl->resp_code = strtol(sp + 1, NULL, 10);
    } else if (!strncasecmp(line, "content-range:", 14)) {
        long long a = 0, b = 0, tot = -1;
        if (sscanf(line + 14, " bytes %lld-%lld/%lld", &a, &b, &tot) == 3) {
            sl->resp_start = a, sl->resp_end = b, sl->resp_total = tot;
            sl->resp_range_ok = 1;
        }
    } else if (!strncasecmp(line, "content-length:", 15)) {
        sl->resp_clen = atoll(line + 15);
    } else if (!strncasecmp(line, "content-encoding:", 17)) {
        char enc[64];
        trim_hdr(line + 17, l - 17, enc, sizeof enc);
        if (enc[0] && strcasecmp(enc, "identity")) sl->resp_encoding_bad = 1;
    } else if (!strncasecmp(line, "content-type:", 13)) {
        char ctype[128];
        trim_hdr(line + 13, l - 13, ctype, sizeof ctype);
        if (!strncasecmp(ctype, "text/html", 9) || !strncasecmp(ctype, "application/xhtml+xml", 21))
            sl->resp_html = 1;
    } else if (!strncasecmp(line, "etag:", 5)) {
        trim_hdr(line + 5, l - 5, sl->resp_etag, sizeof sl->resp_etag);
    } else if (!strncasecmp(line, "last-modified:", 14)) {
        trim_hdr(line + 14, l - 14, sl->resp_lastmod, sizeof sl->resp_lastmod);
    } else if (!strncasecmp(line, "retry-after:", 12)) {
        char ra[128];
        trim_hdr(line + 12, l - 12, ra, sizeof ra);
        sl->resp_retry_after = parse_retry_after(ra);
    }
    return len;
}

static void flush_slot_block(xfer *x, slot *sl) {
    if (!sl->block) return;
    if (sl->block_used == 0) {
        writer_release(x->w, sl->block);
    } else {
        writer_enqueue(x->w, sl->block, x->single || x->usb_mode ? 0 : sl->seg_idx, sl->block_off, sl->block_used);
    }
    sl->block = NULL;
    sl->block_used = 0;
}

static size_t slot_on_data(char *p, size_t sz, size_t n, void *ud) {
    slot *sl = ud;
    xfer *x = sl->x;
    size_t len = sz * n;
    if (x->j->stop || g_quit || x->failed || writer_disk_error(x->w)) return 0;

    if (!sl->checked) {
        sl->checked = 1;
        long code = sl->resp_code;
        if (!code) curl_easy_getinfo(sl->h, CURLINFO_RESPONSE_CODE, &code);
        sl->resp_code = code;
        record_http_version(x, sl->h);
        if (x->sock_info.after > x->rcvbuf) x->rcvbuf = x->sock_info.after;

        if (sl->resp_encoding_bad) {
            sl->bad_resp = 1;
            snprintf(sl->err, sizeof sl->err, "unexpected Content-Encoding from server");
            return 0;
        }
        if (x->is_debrid && sl->resp_html) {
            sl->bad_resp = 1;
            snprintf(sl->err, sizeof sl->err, "provider returned an HTML page instead of the file");
            return 0;
        }
        if (!x->single) {
            if (code != 206 || !sl->resp_range_ok || sl->resp_start != sl->req_start ||
                sl->resp_end != sl->req_end || (x->total > 0 && sl->resp_total != x->total)) {
                sl->bad_resp = 1;
                if (code == 200) snprintf(sl->err, sizeof sl->err, "server ignored the byte range (HTTP 200)");
                else snprintf(sl->err, sizeof sl->err, "unexpected range response (HTTP %ld)", code);
                return 0;
            }
            if ((x->etag[0] && sl->resp_etag[0] && strcmp(sl->resp_etag, x->etag)) ||
                (!x->etag[0] && x->last_modified[0] && sl->resp_lastmod[0] &&
                 strcmp(sl->resp_lastmod, x->last_modified))) {
                sl->bad_resp = 1;
                sl->validator_mismatch = 1;
                snprintf(sl->err, sizeof sl->err, "server file validator changed during download");
                return 0;
            }
        } else {
            if (sl->req_start > 0 && code == 200) {
                sl->bad_resp = 1;
                snprintf(sl->err, sizeof sl->err, "server ignored resume offset (HTTP 200); partial file kept");
                return 0;
            }
            if (code != 200 && code != 206) {
                sl->bad_resp = 1;
                snprintf(sl->err, sizeof sl->err, "HTTP %ld", code);
                return 0;
            }
        }
    }

    if (sl->cut_done) return len; // accept and discard trailing bytes after a segment cut

    int64_t cap_end = sl->req_end;
    if (!x->single && !x->usb_mode) {
        int64_t seg_end = __atomic_load_n(&x->segs[sl->seg_idx].end, __ATOMIC_ACQUIRE);
        if (seg_end < cap_end) cap_end = seg_end;
    }

    size_t keep = len;
    if (cap_end != INT64_MAX) {
        int64_t room = cap_end + 1 - sl->cur_off;
        if (room <= 0) {
            sl->cut_done = 1;
            if (!x->usb_mode) flush_slot_block(x, sl);
            return len;
        }
        if ((int64_t)keep > room) keep = (size_t)room;
    }

    if (x->usb_mode) {
        if (sl->win_used + keep > USB_CHUNK) keep = USB_CHUNK - sl->win_used;
        if (keep > 0) {
            memcpy(sl->win_buf + sl->win_used, p, keep);
            sl->win_used += keep;
            sl->cur_off += (int64_t)keep;
        }
        if (sl->cur_off > cap_end && cap_end != INT64_MAX) sl->cut_done = 1;
        x->segs[0].last_data_ms = plat_now_ms();
        return len;
    }

    // Standard mode: buffer into 128 KiB block; pause before copying if pool is exhausted so pause is lossless.
    if (keep > 0) {
        if (sl->block && WBLOCK_SIZE - sl->block_used < keep) flush_slot_block(x, sl);
        if (!sl->block) {
            sl->block = writer_take(x->w);
            if (!sl->block) {
                sl->paused = 1;
                return CURL_WRITEFUNC_PAUSE;
            }
            sl->block_off = sl->cur_off;
            sl->block_used = 0;
        }
        memcpy(sl->block->data + sl->block_used, p, keep);
        sl->block_used += keep;
        sl->cur_off += (int64_t)keep;
        if (sl->block_used == WBLOCK_SIZE) flush_slot_block(x, sl);
    }

    seg *sg = &x->segs[x->single ? 0 : sl->seg_idx];
    if (sl->cur_off > sg->seen) sg->seen = sl->cur_off;
    sg->last_data_ms = plat_now_ms();
    if (!x->single && sl->cur_off > cap_end && cap_end != INT64_MAX) {
        if (cap_end < sl->req_end) sl->cut_done = 1;
        flush_slot_block(x, sl);
    }
    return len;
}

static int slot_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt, (void)dn, (void)ut, (void)un;
    slot *sl = ud;
    xfer *x = sl->x;
    return x->j->stop || g_quit || x->failed || writer_disk_error(x->w) != 0;
}

static int is_valid_strong_etag(const char *s) {
    if (!s || s[0] != '"') return 0;
    size_t n = strlen(s);
    if (n < 2 || n >= 250 || s[n - 1] != '"') return 0;
    for (size_t i = 1; i + 1 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f || c == '"') return 0;
    }
    return 1;
}

static int is_valid_http_date(const char *s) {
    if (!s || !s[0]) return 0;
    size_t n = strlen(s);
    if (n >= 120) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return curl_getdate(s, NULL) != (time_t)-1;
}

// Probe on the transfer's multi handle (keeps the TCP+TLS connection warm for the first chunk).
typedef struct {
    long code;
    int64_t range_total, length;
    int bad_enc, is_html, range_limit;
    char etag[256], last_modified[128];
    char err_body[160];
    size_t err_body_len;
} probe_info;

static size_t probe_header(char *p, size_t sz, size_t n, void *ud) {
    probe_info *pi = ud;
    size_t len = sz * n;
    char line[512];
    size_t l = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, p, l);
    line[l] = 0;
    if (!strncasecmp(line, "HTTP/", 5)) {
        pi->range_total = -1, pi->length = -1;
        pi->bad_enc = pi->is_html = 0;
        pi->etag[0] = pi->last_modified[0] = 0;
        pi->err_body[0] = 0;
        pi->err_body_len = 0;
        char *sp = strchr(line, ' ');
        if (sp) pi->code = strtol(sp + 1, NULL, 10);
    } else if (!strncasecmp(line, "content-range:", 14)) {
        char *slash = strchr(line, '/');
        if (slash && slash[1] != '*') pi->range_total = atoll(slash + 1);
    } else if (!strncasecmp(line, "content-length:", 15)) {
        pi->length = atoll(line + 15);
    } else if (!strncasecmp(line, "content-encoding:", 17)) {
        char enc[64];
        trim_hdr(line + 17, l - 17, enc, sizeof enc);
        if (enc[0] && strcasecmp(enc, "identity")) pi->bad_enc = 1;
    } else if (!strncasecmp(line, "content-type:", 13)) {
        char ct[128];
        trim_hdr(line + 13, l - 13, ct, sizeof ct);
        if (!strncasecmp(ct, "text/html", 9) || !strncasecmp(ct, "application/xhtml+xml", 21)) pi->is_html = 1;
    } else if (!strncasecmp(line, "etag:", 5)) {
        trim_hdr(line + 5, l - 5, pi->etag, sizeof pi->etag);
    } else if (!strncasecmp(line, "last-modified:", 14)) {
        trim_hdr(line + 14, l - 14, pi->last_modified, sizeof pi->last_modified);
    }
    return len;
}

static size_t probe_body(char *p, size_t sz, size_t n, void *ud) {
    probe_info *pi = ud;
    size_t len = sz * n;
    if (pi->code >= 400) {
        for (size_t i = 0; i < len && pi->err_body_len + 1 < sizeof pi->err_body; i++) {
            unsigned char c = (unsigned char)p[i];
            char ch = (c >= 0x20 && c <= 0x7e) ? (char)c : ' ';
            if (ch == ' ' && (pi->err_body_len == 0 || pi->err_body[pi->err_body_len - 1] == ' ')) continue;
            pi->err_body[pi->err_body_len++] = ch;
            pi->err_body[pi->err_body_len] = 0;
            if (pi->err_body_len >= 7 && !strncmp(pi->err_body + pi->err_body_len - 7, "http://", 7)) {
                pi->err_body_len -= 7;
                pi->err_body[pi->err_body_len] = 0;
                return 0;
            }
            if (pi->err_body_len >= 8 && !strncmp(pi->err_body + pi->err_body_len - 8, "https://", 8)) {
                pi->err_body_len -= 8;
                pi->err_body[pi->err_body_len] = 0;
                return 0;
            }
        }
        return pi->err_body_len + 1 >= sizeof pi->err_body ? 0 : len;
    }
    if (pi->code != 206) return 0; // abort 200 / other responses immediately
    return len > (size_t)pi->range_limit ? 0 : len;
}

static int probe_once(xfer *x, const char *url, const char *range_str, int range_limit, probe_info *pi,
                      char *final_url, size_t final_len, CURLcode *out_rc) {
    CURL *h = x->slots[0].h;
    memset(pi, 0, sizeof *pi);
    pi->range_total = pi->length = -1;
    pi->range_limit = range_limit;
    curl_easy_reset(h);
    net_apply_dl(h, &x->sock_info);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_RANGE, range_str);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, probe_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, pi);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, probe_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, pi);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, slot_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &x->slots[0]);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);

    if (curl_multi_add_handle(x->multi, h) != CURLM_OK) {
        *out_rc = CURLE_FAILED_INIT;
        return -1;
    }
    CURLcode rc = CURLE_OK;
    int done = 0;
    while (!done && !x->j->stop && !g_quit) {
        int running = 0;
        CURLMcode mrc = curl_multi_perform(x->multi, &running);
        if (mrc != CURLM_OK) {
            rc = CURLE_FAILED_INIT;
            break;
        }
        int left = 0;
        CURLMsg *msg;
        while ((msg = curl_multi_info_read(x->multi, &left))) {
            if (msg->msg == CURLMSG_DONE && msg->easy_handle == h) {
                rc = msg->data.result;
                done = 1;
            }
        }
        if (!done) curl_multi_poll(x->multi, NULL, 0, 50, NULL);
    }
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &pi->code);
    char *eff = NULL;
    curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
    snprintf(final_url, final_len, "%s", eff ? eff : url);
    record_http_version(x, h);
    if (x->sock_info.after > x->rcvbuf) x->rcvbuf = x->sock_info.after;
    curl_multi_remove_handle(x->multi, h);
    while (pi->err_body_len > 0 && pi->err_body[pi->err_body_len - 1] == ' ')
        pi->err_body[--pi->err_body_len] = 0;
    *out_rc = rc;
    return 0;
}

static int probe(xfer *x, const char *url, int64_t *total, int *ranges, char *final_url, size_t final_len, char *err,
                 size_t errlen) {
    if (!x->slots[0].h) return -1;
    probe_info pi;
    CURLcode rc = CURLE_OK;
    if (probe_once(x, url, "0-0", 1, &pi, final_url, final_len, &rc) != 0) {
        snprintf(err, errlen, "cannot schedule probe");
        return -1;
    }
    if (!x->j->stop && !g_quit && pi.code == 400) {
        char hst[128];
        net_host_of(final_url, hst, sizeof hst);
        plat_log("job %d: probe Range 0-0 got HTTP 400 on host=%s%s%s; retrying with Range 0-1023", x->j->id,
                 hst[0] ? hst : "-", pi.err_body[0] ? " body=" : "", pi.err_body);
        if (probe_once(x, url, "0-1023", 1024, &pi, final_url, final_len, &rc) != 0) {
            snprintf(err, errlen, "cannot schedule probe");
            return -1;
        }
    }

    if (x->j->stop || g_quit) return -3;
    if (pi.code == 206 || pi.code == 200) {
        if (pi.bad_enc) {
            snprintf(err, errlen, "server sent unsupported Content-Encoding");
            return -2;
        }
        if (x->is_debrid && pi.is_html) {
            snprintf(err, errlen, "provider returned an HTML page instead of a file");
            return -2;
        }
    }
    if (pi.code == 206) {
        *ranges = 1;
        *total = pi.range_total;
        if (is_valid_strong_etag(pi.etag)) snprintf(x->etag, sizeof x->etag, "%s", pi.etag);
        else if (is_valid_http_date(pi.last_modified)) snprintf(x->last_modified, sizeof x->last_modified, "%s", pi.last_modified);
        return 0;
    }
    if (pi.code == 200) {
        *ranges = 0;
        *total = pi.length;
        return 0;
    }
    if (pi.code >= 400) snprintf(err, errlen, "HTTP %ld", pi.code);
    else snprintf(err, errlen, "%s", curl_easy_strerror(rc));
    char hst[128];
    net_host_of(final_url, hst, sizeof hst);
    plat_log("job %d: probe failed http=%ld host=%s (%s)%s%s", x->j->id, pi.code, hst[0] ? hst : "-", err,
             pi.err_body[0] ? " body=" : "", pi.err_body);
    // On TorBox/RD, unreachable/4xx/5xx CDN probes trigger a fresh requestdl link.
    if (pi.code >= 400 && pi.code != 429 && pi.code != 503) return -2;
    if (x->is_debrid && pi.code == 0) return -2;
    return -1;
}

// Claims an unowned segment, or tail-splits the largest active segment with >= SPLIT_MIN left.
static int claim_seg(xfer *x, int64_t now) {
    int best = -1;
    int64_t best_left = 0;
    for (int i = 0; i < x->nseg; i++) {
        seg *s = &x->segs[i];
        int64_t end = s->end;
        int64_t pos = __atomic_load_n(&s->pos, __ATOMIC_RELAXED);
        int64_t at = s->seen > pos ? s->seen : pos;
        if (!s->owned && pos <= end) {
            if (now >= s->retry_at_ms) {
                s->owned = 1;
                s->waiting = 0;
                return i;
            }
            continue;
        }
        if (s->owned && end - at > best_left) best = i, best_left = end - at;
    }
    if (best >= 0 && best_left >= SPLIT_MIN && x->nseg < MAX_SEGS) {
        pthread_mutex_lock(&x->state_lock);
        seg *s = &x->segs[best];
        int64_t at = s->seen;
        int64_t pos = __atomic_load_n(&s->pos, __ATOMIC_RELAXED);
        if (at < pos) at = pos;
        int64_t left = s->end - at;
        if (left >= SPLIT_MIN) {
            int64_t mid = at + left / 2;
            int got = x->nseg;
            seg *n = &x->segs[got];
            memset(n, 0, sizeof *n);
            n->start = n->pos = n->seen = mid + 1;
            n->end = s->end;
            n->owned = 1;
            n->last_data_ms = now;
            __atomic_store_n(&s->end, mid, __ATOMIC_RELEASE);
            x->nseg = got + 1;
            pthread_mutex_unlock(&x->state_lock);
            return got;
        }
        pthread_mutex_unlock(&x->state_lock);
    }
    return -1;
}

static int start_slot_request(xfer *x, slot *sl, int seg_idx, int64_t start, int64_t end) {
    CURL *h = sl->h;
    curl_easy_reset(h);
    if (sl->hdrs) {
        curl_slist_free_all(sl->hdrs);
        sl->hdrs = NULL;
    }
    net_apply_dl(h, &x->sock_info);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 64L * 1024); // strictly <= WBLOCK_SIZE so pause is lossless
    curl_easy_setopt(h, CURLOPT_URL, x->url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 16384L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, (long)x->low_speed_time);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, slot_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, sl);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, slot_on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, sl);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, slot_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, sl);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);

    sl->sent_if_range = 0;
    if (!x->single) {
        if (start < 0 || start > end || (x->total > 0 && end >= x->total)) {
            snprintf(x->err, sizeof x->err, "invalid byte range %lld-%lld", (long long)start, (long long)end);
            return -1;
        }
        char range[64];
        snprintf(range, sizeof range, "%lld-%lld", (long long)start, (long long)end);
        curl_easy_setopt(h, CURLOPT_RANGE, range);
        const char *val = x->etag[0] ? x->etag : x->last_modified[0] ? x->last_modified : NULL;
        if (val && !x->no_if_range) {
            char ifr[300];
            snprintf(ifr, sizeof ifr, "If-Range: %s", val);
            sl->hdrs = curl_slist_append(sl->hdrs, ifr);
            if (sl->hdrs) {
                curl_easy_setopt(h, CURLOPT_HTTPHEADER, sl->hdrs);
                sl->sent_if_range = 1;
            }
        }
    } else if (start > 0) {
        curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)start);
    }

    sl->seg_idx = seg_idx;
    sl->req_start = sl->cur_off = start;
    sl->req_end = end;
    sl->checked = sl->paused = sl->cut_done = 0;
    sl->bad_resp = sl->validator_mismatch = 0;
    sl->resp_code = 0;
    sl->resp_range_ok = sl->resp_encoding_bad = sl->resp_html = 0;
    sl->resp_retry_after = 0;
    sl->err[0] = 0;
    sl->block_used = 0;

    if (curl_multi_add_handle(x->multi, h) != CURLM_OK) {
        snprintf(x->err, sizeof x->err, "curl_multi_add_handle failed");
        return -1;
    }
    sl->active = 1;
    return 0;
}

static void aimd_grow(xfer *x, int in_flight, int64_t now) {
    if (x->single || x->win_allowed >= x->max_conns) return;
    if (now < x->win_paused_until_ms) return;
    if (in_flight + x->win_allowed / 10 < x->win_allowed) return;
    if (now - x->win_grown_ms < 1000 || now - x->win_shrunk_ms < 3000) return;
    int step = x->win_congested ? 1 : (x->win_allowed / 2 > 2 ? x->win_allowed / 2 : 2);
    x->win_allowed += step;
    if (x->win_allowed > x->max_conns) x->win_allowed = x->max_conns;
    x->win_grown_ms = now;
}

static void aimd_shrink(xfer *x, int in_flight, int retry_after, int64_t now) {
    if (retry_after > 0) {
        int cap = x->is_debrid ? 300 : 60;
        if (retry_after > cap) retry_after = cap;
        int64_t until = now + (int64_t)retry_after * 1000;
        if (until > x->win_paused_until_ms) x->win_paused_until_ms = until;
    }
    if (x->win_congested && now - x->win_shrunk_ms < 2000) return;
    int cur = x->win_allowed < in_flight + 1 ? x->win_allowed : in_flight + 1;
    x->win_allowed = cur / 2 > 1 ? cur / 2 : 1;
    x->win_congested = 1;
    x->win_shrunk_ms = x->win_grown_ms = now;
}

static int congested_error(long code, CURLcode rc) {
    if (code == 408 || code == 429 || (code >= 500 && code <= 599)) return 1;
    switch (rc) {
    case CURLE_RECV_ERROR: case CURLE_SEND_ERROR: case CURLE_OPERATION_TIMEDOUT:
    case CURLE_PARTIAL_FILE: case CURLE_COULDNT_CONNECT: case CURLE_GOT_NOTHING:
    case CURLE_SSL_CONNECT_ERROR: case CURLE_COULDNT_RESOLVE_HOST:
        return 1;
    default: return 0;
    }
}

enum { T_OK, T_STOPPED, T_RETRY, T_FATAL, T_RELINK };

static int transfer(job *j, const char *url, char *err, size_t errlen) {
    char final_url[4096], part[1900];
    xfer *x = calloc(1, sizeof *x);
    if (!x) return T_FATAL;
    x->j = j;
    pthread_mutex_init(&x->state_lock, NULL);
    x->is_debrid = net_is_debrid_cdn(url) || !strcmp(j->kind, "torrents") || !strcmp(j->kind, "webdl") ||
                   !strcmp(j->kind, "usenet") || !strcmp(j->kind, "rd");
    x->stall_ms = test_env_int("TORBOX_STALL_MS", 30000);
    x->low_speed_time = test_env_int("TORBOX_LOW_SPEED_TIME", 20);

    x->multi = curl_multi_init();
    if (!x->multi) {
        pthread_mutex_destroy(&x->state_lock);
        free(x);
        return T_FATAL;
    }
    curl_multi_setopt(x->multi, CURLMOPT_PIPELINING, 0L);

    x->slots[0].x = x;
    x->slots[0].h = curl_easy_init();
    if (!x->slots[0].h) {
        curl_multi_cleanup(x->multi);
        pthread_mutex_destroy(&x->state_lock);
        free(x);
        return T_FATAL;
    }

    int64_t total = -1;
    int ranges = 0;
    int pr = probe(x, url, &total, &ranges, final_url, sizeof final_url, err, errlen);
    if (pr != 0) {
        curl_easy_cleanup(x->slots[0].h);
        curl_multi_cleanup(x->multi);
        pthread_mutex_destroy(&x->state_lock);
        free(x);
        return pr == -3 ? T_STOPPED : pr == -2 ? T_RELINK : T_RETRY;
    }

    x->url = final_url;
    x->total = total;
    net_host_of(final_url, x->cdn, sizeof x->cdn);
    if (net_is_debrid_cdn(final_url)) x->is_debrid = 1;

    int conns = g_connections == 0 ? MAX_CONNS : g_connections;
    if (j->max_conns > 0 && conns > j->max_conns) conns = j->max_conns;
    if (conns < 1) conns = 1;
    if (conns > MAX_CONNS) conns = MAX_CONNS;

    snprintf(part, sizeof part, "%s.part", j->path);
    struct stat st, sp_st;
    int has_part = stat(part, &st) == 0;
    int64_t have = has_part ? st.st_size : 0;
    char sp[1900];
    state_path(j, sp, sizeof sp);
    int has_state = stat(sp, &sp_st) == 0;
    int resumed = 0;

    if (has_state) {
        if (have == 0) {
            unlink(sp);
            has_state = 0;
        } else if (!ranges) {
            plat_log("job %d: keeping partial (%lld bytes); server returned no Range support", j->id, (long long)have);
            snprintf(err, errlen, "server did not support byte ranges; partial file kept");
            curl_easy_cleanup(x->slots[0].h);
            curl_multi_cleanup(x->multi);
            pthread_mutex_destroy(&x->state_lock);
            free(x);
            return T_RELINK;
        } else if (total <= 0) {
            plat_log("job %d: keeping partial (%lld bytes); server did not report file size", j->id, (long long)have);
            snprintf(err, errlen, "server did not report file size; partial file kept");
            curl_easy_cleanup(x->slots[0].h);
            curl_multi_cleanup(x->multi);
            pthread_mutex_destroy(&x->state_lock);
            free(x);
            return T_RELINK;
        } else {
            int64_t state_total = -1;
            int ls = load_state(x, total, &state_total);
            if (ls == 0) {
                resumed = 1;
                plat_log("job %d: resumed %lld/%lld bytes (%d segs)", j->id, (long long)xfer_done(x, 0),
                         (long long)total, x->nseg);
            } else if (ls == -2) {
                plat_log("job %d: file size changed (state=%lld server=%lld); partial file kept", j->id,
                         (long long)state_total, (long long)total);
                snprintf(err, errlen, "file size changed on server (%lld vs %lld); partial file kept",
                         (long long)state_total, (long long)total);
                curl_easy_cleanup(x->slots[0].h);
                curl_multi_cleanup(x->multi);
                pthread_mutex_destroy(&x->state_lock);
                free(x);
                return T_RELINK;
            } else {
                plat_log("job %d: resume state unreadable (err=%d); partial file kept (%lld bytes)", j->id, ls,
                         (long long)have);
                snprintf(err, errlen, "resume state unreadable; partial file kept (Cancel to restart)");
                curl_easy_cleanup(x->slots[0].h);
                curl_multi_cleanup(x->multi);
                pthread_mutex_destroy(&x->state_lock);
                free(x);
                return T_FATAL;
            }
        }
    }

    int single = !resumed && !(ranges && total >= MULTI_MIN && conns > 1);
    if (!has_state && have > 0) {
        if (single) {
            if (!ranges) {
                plat_log("job %d: server does not support resume; partial file (%lld bytes) kept", j->id,
                         (long long)have);
                snprintf(err, errlen, "server does not support resume; partial file kept (Cancel to restart)");
                curl_easy_cleanup(x->slots[0].h);
                curl_multi_cleanup(x->multi);
                pthread_mutex_destroy(&x->state_lock);
                free(x);
                return T_FATAL;
            }
            if (total > 0 && have > total) {
                plat_log("job %d: partial file (%lld bytes) larger than server file (%lld); kept", j->id,
                         (long long)have, (long long)total);
                snprintf(err, errlen, "partial file larger than server file; partial file kept (Cancel to restart)");
                curl_easy_cleanup(x->slots[0].h);
                curl_multi_cleanup(x->multi);
                pthread_mutex_destroy(&x->state_lock);
                free(x);
                return T_FATAL;
            }
        } else {
            plat_log("job %d: partial file (%lld bytes) exists without resume state; kept", j->id, (long long)have);
            snprintf(err, errlen, "partial file exists without resume state; kept on disk (Cancel to restart)");
            curl_easy_cleanup(x->slots[0].h);
            curl_multi_cleanup(x->multi);
            pthread_mutex_destroy(&x->state_lock);
            free(x);
            return T_FATAL;
        }
    }

    x->single = single;
    x->usb_mode = !single && plat_is_usb_path(j->path);

    if (single) {
        x->nseg = 1;
        x->segs[0].start = 0;
        x->segs[0].end = total > 0 ? total - 1 : INT64_MAX;
        x->segs[0].pos = x->segs[0].seen = have;
        if (total > 0 && have == total) {
            curl_easy_cleanup(x->slots[0].h);
            curl_multi_cleanup(x->multi);
            pthread_mutex_destroy(&x->state_lock);
            free(x);
            return T_OK;
        }
    } else if (x->usb_mode) {
        // If a prior non-USB partial exists with multiple segments, keep standard mode to fill its holes safely.
        if (resumed && x->nseg > 1) {
            x->usb_mode = 0;
        } else {
            if (!resumed) {
                x->nseg = 1;
                x->segs[0].start = 0;
                x->segs[0].end = total - 1;
                x->segs[0].pos = x->segs[0].seen = 0;
            }
            x->usb_cursor = x->usb_next_issue = x->segs[0].pos;
            if (conns > USB_WIN_SLOTS) conns = USB_WIN_SLOTS;
        }
    }
    if (!single && !x->usb_mode && !resumed) {
        int n = conns;
        x->nseg = n;
        for (int i = 0; i < n; i++) {
            x->segs[i].start = total * i / n;
            x->segs[i].end = total * (i + 1) / n - 1;
            x->segs[i].pos = x->segs[i].seen = x->segs[i].start;
        }
    }

    x->fd = open(part, O_RDWR | O_CREAT, 0666);
    if (x->fd < 0) {
        snprintf(err, errlen, "cannot create %s: %s", part, strerror(errno));
        curl_easy_cleanup(x->slots[0].h);
        curl_multi_cleanup(x->multi);
        pthread_mutex_destroy(&x->state_lock);
        free(x);
        return T_FATAL;
    }
    if (!single) (void)save_state(x, total);

    x->max_conns = single ? 1 : conns;
    x->nslots = x->max_conns;
    x->win_allowed = single ? 1 : (x->max_conns < 8 ? x->max_conns : 8);
    curl_multi_setopt(x->multi, CURLMOPT_MAXCONNECTS, (long)(x->max_conns + 2));
    curl_multi_setopt(x->multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)x->max_conns);
    curl_multi_setopt(x->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)x->max_conns);

    for (int i = 0; i < x->nslots; i++) {
        x->slots[i].x = x;
        if (i > 0) x->slots[i].h = curl_easy_init();
        if (!x->slots[i].h) {
            x->nslots = i;
            break;
        }
        if (x->usb_mode) {
            x->slots[i].win_buf = malloc(USB_CHUNK);
            if (!x->slots[i].win_buf) {
                curl_easy_cleanup(x->slots[i].h);
                x->nslots = i;
                break;
            }
        }
    }
    if (x->nslots < 1) {
        close(x->fd);
        curl_multi_cleanup(x->multi);
        pthread_mutex_destroy(&x->state_lock);
        free(x);
        return T_FATAL;
    }
    if (x->max_conns > x->nslots) x->max_conns = x->nslots;
    if (x->win_allowed > x->max_conns) x->win_allowed = x->max_conns;

    int pool_blocks = x->usb_mode ? WPOOL_COMPACT : WPOOL_MAX;
    x->w = writer_start(x->fd, pool_blocks, x->multi, on_writer_commit, on_writer_state, x);
    if (!x->w) {
        for (int i = 0; i < x->nslots; i++) curl_easy_cleanup(x->slots[i].h), free(x->slots[i].win_buf);
        close(x->fd);
        curl_multi_cleanup(x->multi);
        pthread_mutex_destroy(&x->state_lock);
        free(x);
        return T_FATAL;
    }

    int64_t now0 = plat_now_ms();
    x->win_grown_ms = x->last_progress_ms = now0;
    int64_t last_loop_ms = now0, last_ui_ms = now0, last_log_ms = now0;
    int64_t last_recv_done = xfer_done(x, 0), log_recv = last_recv_done, log_disk = last_recv_done;
    int64_t ui_recv = last_recv_done;
    double recv_speed = 0, disk_speed = 0;

    pthread_mutex_lock(&g_lock);
    j->total = total;
    j->done = last_recv_done;
    j->start_done = j->done;
    j->state = J_RUNNING;
    j->note[0] = 0;
    j->usb_mode = x->usb_mode;
    j->rcvbuf = x->rcvbuf;
    snprintf(j->http_ver, sizeof j->http_ver, "%s", x->http_ver);
    snprintf(j->cdn, sizeof j->cdn, "%s", x->cdn);
    pthread_mutex_unlock(&g_lock);

    plat_log("job %d: transfer %lld bytes conns=%d rcvbuf=%dKiB http=%s host=%s%s", j->id, (long long)total,
             x->max_conns, x->rcvbuf / 1024, x->http_ver[0] ? x->http_ver : "?",
             x->cdn[0] ? x->cdn : "-", x->usb_mode ? " usb=seq" : "");

    int64_t multi_busy_us = 0;

    for (;;) {
        int64_t now = plat_now_ms();
        if (now - last_loop_ms > 10000) {
            x->grace_until_ms = now + 60000;
            x->last_progress_ms = now;
            plat_log("job %d: resumed after suspension, waiving transient drops for 60s", j->id);
        }
        last_loop_ms = now;

        if (j->stop || g_quit || x->failed || writer_disk_error(x->w)) break;

        // 1. Drain completed USB chunks strictly in ascending offset order
        if (x->usb_mode) {
            int progressed = 1;
            while (progressed) {
                progressed = 0;
                for (int i = 0; i < x->nslots; i++) {
                    slot *sl = &x->slots[i];
                    if (!sl->usb_assigned || !sl->usb_ready || sl->chunk_start != x->usb_cursor) continue;
                    while (sl->win_drained < sl->win_used) {
                        wblock *b = writer_take(x->w);
                        if (!b) break;
                        size_t take = sl->win_used - sl->win_drained;
                        if (take > WBLOCK_SIZE) take = WBLOCK_SIZE;
                        memcpy(b->data, sl->win_buf + sl->win_drained, take);
                        writer_enqueue(x->w, b, 0, x->usb_cursor, take);
                        sl->win_drained += take;
                        x->usb_cursor += (int64_t)take;
                        x->segs[0].seen = x->usb_cursor;
                    }
                    if (sl->win_drained == sl->win_used) {
                        sl->usb_ready = 0;
                        sl->usb_assigned = 0;
                        progressed = 1;
                    }
                    break;
                }
            }
        }

        // 2. Cleanly detach cut slots & unpause slots whose writer pool freed blocks
        int has_free_block = x->usb_mode ? 1 : writer_has_free(x->w);
        for (int i = 0; i < x->nslots; i++) {
            slot *sl = &x->slots[i];
            if (sl->active && sl->cut_done) {
                curl_multi_remove_handle(x->multi, sl->h);
                flush_slot_block(x, sl);
                sl->active = sl->cut_done = 0;
                if (!x->single && !x->usb_mode) x->segs[sl->seg_idx].owned = 0;
            } else if (sl->active && sl->paused && has_free_block) {
                sl->paused = 0;
                CURLcode prc = curl_easy_pause(sl->h, CURLPAUSE_CONT);
                if (prc != CURLE_OK) {
                    x->failed = 1;
                    snprintf(x->err, sizeof x->err, "%s", curl_easy_strerror(prc));
                    break;
                }
            }
        }
        if (x->failed) break;

        // 3. Check completion & count in-flight
        int in_flight = 0;
        for (int i = 0; i < x->nslots; i++) {
            if (x->slots[i].active) in_flight++;
        }

        int all_done = 0;
        if (x->single) {
            all_done = in_flight == 0 && (x->total >= 0 ? x->segs[0].seen >= x->total : x->segs[0].owned == 2);
        } else if (x->usb_mode) {
            int usb_busy = 0;
            for (int i = 0; i < x->nslots; i++)
                if (x->slots[i].usb_assigned || x->slots[i].active) usb_busy++;
            all_done = usb_busy == 0 && x->usb_cursor >= total && x->usb_next_issue >= total;
        } else {
            all_done = 1;
            for (int i = 0; i < x->nseg; i++) {
                int64_t pos = __atomic_load_n(&x->segs[i].pos, __ATOMIC_RELAXED);
                int64_t at = x->segs[i].seen > pos ? x->segs[i].seen : pos;
                if (at <= x->segs[i].end) {
                    all_done = 0;
                    break;
                }
            }
            if (all_done && in_flight > 0) all_done = 0;
        }
        if (all_done) break;

        // 4. Grow AIMD window & schedule free slots
        aimd_grow(x, in_flight, now);

        if (now >= x->win_paused_until_ms) {
            for (int i = 0; i < x->nslots && in_flight < x->win_allowed; i++) {
                slot *sl = &x->slots[i];
                if (sl->active) continue;

                if (x->single) {
                    if (x->segs[0].owned == 2) continue;
                    if (x->segs[0].owned == 0 && now >= x->segs[0].retry_at_ms) {
                        x->segs[0].owned = 1;
                        x->segs[0].waiting = 0;
                        int64_t s0 = x->segs[0].seen;
                        if (start_slot_request(x, sl, 0, s0, total > 0 ? total - 1 : INT64_MAX) == 0) in_flight++;
                        else { x->failed = 1; break; }
                    }
                    continue;
                }

                if (x->usb_mode) {
                    if (sl->usb_ready) continue;
                    if (sl->usb_assigned) {
                        if (now < sl->usb_retry_at_ms) continue;
                        if (sl->chunk_start + (int64_t)sl->win_used > sl->chunk_end) {
                            sl->usb_ready = 1;
                            continue;
                        }
                        if (start_slot_request(x, sl, 0, sl->chunk_start + (int64_t)sl->win_used, sl->chunk_end) == 0)
                            in_flight++;
                        else { x->failed = 1; break; }
                        continue;
                    }
                    if (x->usb_next_issue < total && x->usb_next_issue - x->usb_cursor < (int64_t)x->nslots * USB_CHUNK) {
                        int64_t start = x->usb_next_issue;
                        int64_t end = start + USB_CHUNK - 1;
                        if (end >= total) end = total - 1;
                        x->usb_next_issue = end + 1;
                        sl->usb_assigned = 1;
                        sl->usb_ready = 0;
                        sl->chunk_start = start;
                        sl->chunk_end = end;
                        sl->win_used = sl->win_drained = 0;
                        sl->usb_attempts = sl->usb_busy_retries = 0;
                        sl->usb_retry_at_ms = 0;
                        if (start_slot_request(x, sl, 0, start, end) == 0) in_flight++;
                        else { x->failed = 1; break; }
                    }
                    continue;
                }

                // Standard segmented mode
                int seg_idx = claim_seg(x, now);
                if (seg_idx < 0) break;
                seg *sg = &x->segs[seg_idx];
                int64_t start = sg->seen;
                int64_t pos = __atomic_load_n(&sg->pos, __ATOMIC_RELAXED);
                if (pos > start) start = pos;
                int64_t end = sg->end;
                if (end - start + 1 > CHUNK_CAP) end = start + CHUNK_CAP - 1;
                if (start > sg->end) {
                    sg->owned = 0;
                    continue;
                }
                if (start_slot_request(x, sl, seg_idx, start, end) == 0) in_flight++;
                else {
                    sg->owned = 0;
                    x->failed = 1;
                    break;
                }
            }
        }
        if (x->failed) break;

        // 5. Perform curl_multi I/O
        int running = 0;
        int64_t t0 = plat_now_us();
        CURLMcode mrc = curl_multi_perform(x->multi, &running);
        multi_busy_us += plat_now_us() - t0;
        if (mrc != CURLM_OK) {
            x->failed = 1;
            snprintf(x->err, sizeof x->err, "%s", curl_multi_strerror(mrc));
            break;
        }

        // 6. Process finished easy handles
        int left = 0;
        CURLMsg *msg;
        while ((msg = curl_multi_info_read(x->multi, &left))) {
            if (msg->msg != CURLMSG_DONE) continue;
            CURL *eh = msg->easy_handle;
            slot *sl = NULL;
            for (int i = 0; i < x->nslots; i++)
                if (x->slots[i].h == eh) { sl = &x->slots[i]; break; }
            curl_multi_remove_handle(x->multi, eh);
            if (!sl) continue;

            sl->active = 0;
            CURLcode rc = msg->data.result;
            long code = sl->resp_code;
            if (!code) curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &code);
            if (code) x->last_code = code;

            if (!x->usb_mode) flush_slot_block(x, sl);

            if (j->stop || g_quit || writer_disk_error(x->w)) break;

            int64_t got = sl->cur_off - sl->req_start;
            int chunk_ok = (rc == CURLE_OK && !sl->bad_resp &&
                            (x->single ? (x->total < 0 || sl->cur_off >= x->total) : sl->cur_off > sl->req_end));
            if (sl->cut_done && !sl->bad_resp) chunk_ok = 1;

            if (chunk_ok) {
                if (x->single) {
                    x->segs[0].owned = 2; // single stream complete
                } else if (x->usb_mode) {
                    sl->usb_ready = 1;
                } else {
                    seg *sg = &x->segs[sl->seg_idx];
                    sg->owned = 0;
                    sg->attempts = sg->busy_retries = 0;
                }
                continue;
            }

            // Failure or partial response handling
            if (code == 400 && sl->sent_if_range) {
                if (!x->no_if_range) {
                    x->no_if_range = 1;
                    plat_log("job %d: HTTP 400 with If-Range on host=%s range=%lld-%lld; disabling If-Range and retrying",
                             j->id, x->cdn[0] ? x->cdn : "-", (long long)sl->req_start, (long long)sl->req_end);
                }
                if (!x->usb_mode) x->segs[x->single ? 0 : sl->seg_idx].owned = 0;
                continue;
            }
            if (sl->validator_mismatch) {
                x->failed = 1;
                x->relink_needed = 1;
                snprintf(x->err, sizeof x->err, "%s", sl->err);
                break;
            }
            if (sl->bad_resp && code != 200) { // HTML/encoding error
                x->failed = 1;
                x->relink_needed = 1;
                snprintf(x->err, sizeof x->err, "%s", sl->err);
                break;
            }
            if (code == 401 || code == 403 || code == 404 || code == 410 ||
                (rc == CURLE_HTTP_RETURNED_ERROR && code >= 400 && code != 408 && code != 429 && code != 503 &&
                 code < 500)) {
                plat_log("job %d: chunk HTTP %ld on host=%s range=%lld-%lld/%lld if_range=%d got=%lld rc=%d", j->id,
                         code, x->cdn[0] ? x->cdn : "-", (long long)sl->req_start, (long long)sl->req_end,
                         (long long)x->total, sl->sent_if_range, (long long)got, (int)rc);
                x->failed = 1;
                x->http_error = code;
                x->relink_needed = 1;
                snprintf(x->err, sizeof x->err, "HTTP %ld", code);
                break;
            }
            if (sl->bad_resp && code == 200) { // server ignored byte range on a ranged segment
                x->failed = 1;
                snprintf(x->err, sizeof x->err, "%s", sl->err);
                break;
            }

            // Transient / congestion / partial read
            int in_grace = now < x->grace_until_ms && code < 400;
            if (!in_grace && congested_error(code, rc)) aimd_shrink(x, in_flight, sl->resp_retry_after, now);

            if (x->usb_mode) {
                if (code == 429 || code == 503) {
                    if (++sl->usb_busy_retries > 30) {
                        x->failed = 1;
                        x->http_error = code;
                        snprintf(x->err, sizeof x->err, "HTTP %ld", code);
                        break;
                    }
                    int wait_s = sl->resp_retry_after > 0 ? sl->resp_retry_after
                                                          : (sl->usb_busy_retries * 2 < 15 ? sl->usb_busy_retries * 2 : 15);
                    sl->usb_retry_at_ms = now + (int64_t)wait_s * 1000;
                } else {
                    if (got >= 65536 || in_grace) sl->usb_attempts = 0;
                    else if (++sl->usb_attempts >= 5) {
                        x->failed = 1;
                        x->relink_needed = x->is_debrid;
                        snprintf(x->err, sizeof x->err, "%s", sl->err[0] ? sl->err : curl_easy_strerror(rc));
                        break;
                    }
                    sl->usb_retry_at_ms = now + (got >= 65536 ? 200 : 2000);
                }
                continue;
            }

            seg *sg = &x->segs[x->single ? 0 : sl->seg_idx];
            if (!x->single) sg->owned = 0;
            else x->segs[0].owned = 0;

            if (code == 429 || code == 503) {
                if (++sg->busy_retries > 30) {
                    x->failed = 1;
                    x->http_error = code;
                    snprintf(x->err, sizeof x->err, "HTTP %ld", code);
                    break;
                }
                int wait_s = sl->resp_retry_after > 0 ? sl->resp_retry_after
                                                      : (sg->busy_retries * 2 < 15 ? sg->busy_retries * 2 : 15);
                sg->retry_at_ms = now + (int64_t)wait_s * 1000;
                sg->waiting = 1;
            } else {
                if (got >= 65536 || in_grace) sg->attempts = 0;
                else if (++sg->attempts >= 5) {
                    x->failed = 1;
                    x->relink_needed = x->is_debrid;
                    snprintf(x->err, sizeof x->err, "%s", sl->err[0] ? sl->err : curl_easy_strerror(rc));
                    break;
                }
                sg->retry_at_ms = now + (got >= 65536 ? 200 : 2000);
                sg->waiting = 1;
            }
        }
        if (x->failed || j->stop || g_quit || writer_disk_error(x->w)) break;

        // 7. Aggregate stall watchdog + periodic UI & log telemetry
        now = plat_now_ms();
        int64_t d_seen = xfer_done(x, 1);
        if (d_seen - last_recv_done >= 65536) {
            last_recv_done = d_seen;
            x->last_progress_ms = now;
        }
        int paused_cnt = 0;
        for (int i = 0; i < x->nslots; i++)
            if (x->slots[i].active && x->slots[i].paused) paused_cnt++;

        if (now < x->win_paused_until_ms || paused_cnt > 0) x->last_progress_ms = now;
        if (now - x->last_progress_ms >= x->stall_ms) {
            x->failed = 1;
            x->relink_needed = 1;
            snprintf(x->err, sizeof x->err, "stalled (no progress for %llds)", (long long)(x->stall_ms / 1000));
            break;
        }

        if (now - last_ui_ms >= 500) {
            int64_t dt_ms = now - last_ui_ms;
            double inst = (double)(d_seen - ui_recv) * 1000.0 / (double)dt_ms;
            recv_speed = recv_speed == 0 ? inst : recv_speed * 0.6 + inst * 0.4;
            ui_recv = d_seen;
            last_ui_ms = now;

            int64_t w_written = 0;
            size_t w_buf = 0;
            unsigned w_max = 0;
            writer_stats(x->w, &w_written, &w_buf, &w_max, NULL);

            int active_cnt = 0, waiting_cnt = 0;
            for (int i = 0; i < x->nslots; i++) {
                if (x->slots[i].active && !x->slots[i].paused) {
                    int64_t lms = x->usb_mode || x->single ? x->segs[0].last_data_ms
                                                           : x->segs[x->slots[i].seg_idx].last_data_ms;
                    if (now - lms < 3000) active_cnt++;
                }
            }
            if (!x->usb_mode) {
                for (int i = 0; i < x->nseg; i++) waiting_cnt += x->segs[i].waiting;
            }
            if (now < x->win_paused_until_ms) waiting_cnt += x->win_allowed - in_flight;

            int64_t disk_dt = now - last_log_ms;
            if (disk_dt >= 5000) {
                int64_t d_disk = xfer_done(x, 0);
                double r_mb = (double)(d_seen - log_recv) * 1000.0 / (double)disk_dt / 1048576.0;
                double d_mb = (double)(d_disk - log_disk) * 1000.0 / (double)disk_dt / 1048576.0;
                disk_speed = d_mb * 1048576.0;
                double busy_pct = (double)multi_busy_us / ((double)disk_dt * 10.0);
                plat_log("job %d: conns=%d/%d win=%d paused=%d recv=%.1fMB/s disk=%.1fMB/s buf=%zuKiB wmax=%ums "
                         "busy=%.0f%% rcvbuf=%dKiB http=%s host=%s%s",
                         j->id, active_cnt, in_flight, x->win_allowed, paused_cnt, r_mb, d_mb, w_buf / 1024, w_max,
                         busy_pct, x->rcvbuf / 1024, x->http_ver[0] ? x->http_ver : "?",
                         x->cdn[0] ? x->cdn : "-", x->usb_mode ? " usb=seq" : "");
                log_recv = d_seen;
                log_disk = d_disk;
                multi_busy_us = 0;
                last_log_ms = now;
            }

            pthread_mutex_lock(&g_lock);
            j->conns = in_flight;
            j->active = active_cnt;
            j->waiting = waiting_cnt;
            j->paused_conns = paused_cnt;
            j->win = x->win_allowed;
            j->done = d_seen;
            j->speed = recv_speed;
            j->disk_speed = disk_speed > 0 ? disk_speed : recv_speed;
            j->buffered = w_buf;
            if (w_max > j->write_max_ms) j->write_max_ms = w_max;
            j->rcvbuf = x->rcvbuf;
            j->last_code = x->last_code;
            snprintf(j->http_ver, sizeof j->http_ver, "%s", x->http_ver);
            pthread_mutex_unlock(&g_lock);
        }

        // 8. Wait for socket readiness, Retry-After wake, or writer wakeup
        int wait_ms = 100;
        if (in_flight == 0 && !all_done) wait_ms = 200;
        curl_multi_poll(x->multi, NULL, 0, wait_ms, NULL);
    }

    // Drain/cleanup all active easy handles and flush any partial blocks before closing writer
    for (int i = 0; i < x->nslots; i++) {
        slot *sl = &x->slots[i];
        if (sl->active) {
            curl_multi_remove_handle(x->multi, sl->h);
            sl->active = 0;
        }
        if (!x->usb_mode) flush_slot_block(x, sl);
    }
    // If USB mode had any completed leading chunks waiting to drain on normal completion, drain them now
    if (x->usb_mode && !x->failed && !j->stop && !g_quit && !writer_disk_error(x->w)) {
        int progressed = 1;
        while (progressed) {
            progressed = 0;
            for (int i = 0; i < x->nslots; i++) {
                slot *sl = &x->slots[i];
                if (!sl->usb_assigned || !sl->usb_ready || sl->chunk_start != x->usb_cursor) continue;
                while (sl->win_drained < sl->win_used) {
                    wblock *b = writer_take(x->w);
                    if (!b) {
                        plat_sleep_ms(10);
                        continue;
                    }
                    size_t take = sl->win_used - sl->win_drained;
                    if (take > WBLOCK_SIZE) take = WBLOCK_SIZE;
                    memcpy(b->data, sl->win_buf + sl->win_drained, take);
                    writer_enqueue(x->w, b, 0, x->usb_cursor, take);
                    sl->win_drained += take;
                    x->usb_cursor += (int64_t)take;
                }
                sl->usb_ready = sl->usb_assigned = 0;
                progressed = 1;
            }
        }
    }

    int w_err = writer_finish(x->w, 1);
    x->w = NULL;

    for (int i = 0; i < x->nslots; i++) {
        if (x->slots[i].hdrs) curl_slist_free_all(x->slots[i].hdrs);
        if (x->slots[i].h) curl_easy_cleanup(x->slots[i].h);
        free(x->slots[i].win_buf);
    }
    curl_multi_cleanup(x->multi);

    int64_t d = xfer_done(x, 0);
    int complete = !x->failed && !j->stop && !g_quit && !w_err && (total < 0 || d >= total);
    close(x->fd);

    pthread_mutex_lock(&g_lock);
    j->done = d;
    j->speed = j->disk_speed = 0;
    j->buffered = 0;
    j->conns = j->active = j->waiting = j->paused_conns = 0;
    if (single && total < 0) j->total = d;
    pthread_mutex_unlock(&g_lock);

    int result;
    if (complete) {
        if (!single) {
            char sp[1900];
            state_path(j, sp, sizeof sp);
            unlink(sp);
        }
        result = T_OK;
    } else {
        if (!single) (void)save_state(x, total);
        if (j->stop || g_quit) result = T_STOPPED;
        else if (w_err) {
            snprintf(err, errlen, "write failed: %s", strerror(w_err));
            result = T_FATAL;
        } else if (x->relink_needed || x->http_error) {
            snprintf(err, errlen, "%s", x->err[0] ? x->err : "transfer failed");
            result = T_RELINK;
        } else {
            snprintf(err, errlen, "%s", x->err[0] ? x->err : "transfer incomplete");
            result = T_RETRY;
        }
    }
    pthread_mutex_destroy(&x->state_lock);
    free(x);
    return result;
}

static void set_state(job *j, int st, const char *err) {
    pthread_mutex_lock(&g_lock);
    j->state = st;
    if (err) snprintf(j->err, sizeof j->err, "%s", err);
    if (st >= J_DONE) j->finished_ms = plat_now_ms();
    save_queue_locked();
    pthread_mutex_unlock(&g_lock);
}

static int rd_resolve(job *j, char *url, size_t urllen, char *err, size_t errlen, int *permanent) {
    *permanent = 0;
    if (j->direct) {
        snprintf(url, urllen, "%s", j->direct);
        return 0;
    }
    char name[256] = "";
    int chunks = 0;
    if (rd_unrestrict(j->url ? j->url : "", url, urllen, name, sizeof name, &chunks, permanent, err, errlen))
        return -1;
    pthread_mutex_lock(&g_lock);
    free(j->direct);
    j->direct = strdup(url);
    j->max_conns = chunks > 0 ? chunks : 0;
    if (!j->name[0]) {
        sanitize_name(name);
        snprintf(j->name, sizeof j->name, "%s", name[0] ? name : "download");
        build_path(j);
        save_queue_locked();
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static void forget_direct(job *j) {
    pthread_mutex_lock(&g_lock);
    free(j->direct);
    j->direct = NULL;
    pthread_mutex_unlock(&g_lock);
}

static void sleep_job_ms(job *j, int ms) {
    for (int elapsed = 0; elapsed < ms && !j->stop && !g_quit; elapsed += 250) {
        int chunk = ms - elapsed < 250 ? ms - elapsed : 250;
        plat_sleep_ms(chunk);
    }
}

static void run_job(job *j) {
    char err[300] = "", url[4096];
    char dir[1800];
    int is_rd = !strcmp(j->kind, "rd"), permanent = 0;
    int is_tb = !strcmp(j->kind, "torrents") || !strcmp(j->kind, "webdl") || !strcmp(j->kind, "usenet");
    for (int i = 0; is_rd && !j->name[0] && i < 3; i++) {
        if (i) sleep_job_ms(j, 3000);
        if (!rd_resolve(j, url, sizeof url, err, sizeof err, &permanent)) break;
        if (permanent || i == 2 || j->stop || g_quit) {
            set_state(j, j->stop == 1 ? J_PAUSED : j->stop == 2 ? J_CANCELLED : g_quit ? J_QUEUED : J_FAILED, err);
            return;
        }
    }
    snprintf(dir, sizeof dir, "%s", j->path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (mkdir_p(dir)) {
        snprintf(err, sizeof err, "cannot create folder %s: %s", dir, strerror(errno));
        set_state(j, J_FAILED, err);
        return;
    }
    struct stat st;
    if (!stat(j->path, &st)) {
        pthread_mutex_lock(&g_lock);
        j->total = j->done = st.st_size;
        snprintf(j->note, sizeof j->note, "already there, not downloaded again");
        pthread_mutex_unlock(&g_lock);
        set_state(j, J_DONE, "");
        return;
    }
    plat_log("job %d: start %s", j->id, j->path);
    j->started_ms = plat_now_ms();
    int tb_relinks = 0;
    int total_attempts = 0;
    static const int retry_delays_s[] = {3, 10, 20, 40, 60, 60};
    for (int attempt = 0; attempt < 7 && total_attempts < 50; attempt++, total_attempts++) {
        if (attempt) {
            int idx = attempt - 1;
            int n_delays = (int)(sizeof retry_delays_s / sizeof *retry_delays_s);
            if (idx >= n_delays) idx = n_delays - 1;
            int max_ms = test_env_int("TORBOX_RETRY_MAX_MS", 60000);
            int wait_ms = retry_delays_s[idx] * 1000;
            if (wait_ms > max_ms) wait_ms = max_ms;
            pthread_mutex_lock(&g_lock);
            snprintf(j->note, sizeof j->note, "%s — retrying in %ds...", j->err[0] ? j->err : "retrying",
                     wait_ms >= 1000 ? wait_ms / 1000 : 1);
            pthread_mutex_unlock(&g_lock);
            sleep_job_ms(j, wait_ms);
            pthread_mutex_lock(&g_lock);
            j->note[0] = 0;
            pthread_mutex_unlock(&g_lock);
        }
        if (j->stop || g_quit) break;
        if (!strcmp(j->kind, "url")) snprintf(url, sizeof url, "%s", j->url ? j->url : "");
        else if (is_rd) {
            if (rd_resolve(j, url, sizeof url, err, sizeof err, &permanent)) {
                pthread_mutex_lock(&g_lock);
                snprintf(j->err, sizeof j->err, "%s", err);
                pthread_mutex_unlock(&g_lock);
                if (permanent) break;
                continue;
            }
        } else if (tb_request_link(j->kind, j->item_id, j->file_id, url, sizeof url, err, sizeof err)) {
            pthread_mutex_lock(&g_lock);
            snprintf(j->err, sizeof j->err, "link request: %s", err);
            pthread_mutex_unlock(&g_lock);
            if (strstr(err, "not logged in") || strstr(err, "BAD_TOKEN") || strstr(err, "LIMIT")) break;
            continue;
        }
        pthread_mutex_lock(&g_lock);
        j->start_done = j->done;
        pthread_mutex_unlock(&g_lock);
        int r = transfer(j, url, err, sizeof err);
        memset(url, 0, sizeof url);
        pthread_mutex_lock(&g_lock);
        int64_t new_bytes = j->done - j->start_done;
        pthread_mutex_unlock(&g_lock);
        if (r == T_OK) {
            char part[1900];
            snprintf(part, sizeof part, "%s.part", j->path);
            if (rename(part, j->path)) {
                snprintf(err, sizeof err, "rename failed: %s", strerror(errno));
                set_state(j, J_FAILED, err);
                return;
            }
            plat_log("job %d: done, %lld bytes in %.1f s", j->id, (long long)j->done,
                     (plat_now_ms() - j->started_ms) / 1000.0);
            pthread_mutex_lock(&g_lock);
            j->note[0] = 0;
            pthread_mutex_unlock(&g_lock);
            set_state(j, J_DONE, "");
            return;
        }
        int progressed = new_bytes >= (1024ll * 1024);
        if (r != T_STOPPED && r != T_FATAL && progressed && total_attempts + 1 < 50) {
            plat_log("job %d: progressed %lld MiB before %s; refreshing link and resuming", j->id,
                     (long long)(new_bytes / (1024ll * 1024)), err);
            attempt = -1;
            tb_relinks = 0;
            pthread_mutex_lock(&g_lock);
            snprintf(j->err, sizeof j->err, "%s", err);
            pthread_mutex_unlock(&g_lock);
            if (is_rd) forget_direct(j);
            continue;
        }
        if (is_tb && r == T_RELINK) {
            tb_relinks++;
            plat_log("job %d: TorBox CDN failed (%s); link rotation %d/6", j->id, err, tb_relinks);
            if (tb_relinks >= 6) {
                char msg[300];
                snprintf(msg, sizeof msg, "%s (try switching CDN in TorBox settings)", err);
                pthread_mutex_lock(&g_lock);
                snprintf(j->err, sizeof j->err, "%s", msg);
                pthread_mutex_unlock(&g_lock);
                break;
            }
        }
        pthread_mutex_lock(&g_lock);
        snprintf(j->err, sizeof j->err, "%s", err);
        pthread_mutex_unlock(&g_lock);
        if (is_rd && r != T_STOPPED) forget_direct(j);
        if (r == T_STOPPED || r == T_FATAL) break;
        plat_log("job %d: attempt %d failed: %s", j->id, attempt + 1, err);
    }
    if (j->stop == 2) {
        char p[1900];
        snprintf(p, sizeof p, "%s.part", j->path);
        unlink(p);
        state_path(j, p, sizeof p);
        unlink(p);
        set_state(j, J_CANCELLED, "");
    } else if (j->stop == 1) set_state(j, J_PAUSED, "");
    else if (g_quit) set_state(j, J_QUEUED, "");
    else {
        plat_log("job %d: failed: %s", j->id, j->err);
        set_state(j, J_FAILED, NULL);
    }
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        job *j = NULL;
        while (!g_quit) {
            if (g_running < g_parallel)
                for (int i = 0; i < g_njobs && !j; i++)
                    if (g_jobs[i]->state == J_QUEUED) j = g_jobs[i];
            if (j) break;
            pthread_cond_wait(&g_cv, &g_lock);
        }
        if (g_quit) {
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        j->state = J_STARTING;
        j->stop = 0;
        j->err[0] = 0;
        g_running++;
        pthread_mutex_unlock(&g_lock);
        run_job(j);
        pthread_mutex_lock(&g_lock);
        g_running--;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_lock);
    }
}

void dl_init(int workers) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_mutex_lock(&g_lock);
    load_settings();
    load_queue();
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < workers; i++) plat_start_thread(worker, NULL);
}

int dl_add(const char *kind, long long item_id, long long file_id, const char *url, const char *folder,
           const char *subdir, const char *name, char *err, size_t errlen) {
    int is_rd = !strcmp(kind, "rd"), is_url = !strcmp(kind, "url") || is_rd;
    if (!is_url && strcmp(kind, "torrents") && strcmp(kind, "webdl") && strcmp(kind, "usenet")) {
        snprintf(err, errlen, "unknown kind");
        return -1;
    }
    if (is_url && (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))) {
        snprintf(err, errlen, "the link must start with http:// or https://");
        return -1;
    }
    if (!plat_path_allowed(folder)) {
        snprintf(err, errlen, "folder not allowed (use /data/... or a USB drive /mnt/usbN/...)");
        return -1;
    }
    char clean_name[256], clean_sub[512];
    snprintf(clean_name, sizeof clean_name, "%s", name && *name ? name : is_rd ? "" : "download");
    if (is_url && !is_rd && (!name || !*name)) {
        const char *q = strchr(url + 8, '/');
        const char *end = q ? q + strcspn(q, "?#") : NULL;
        const char *s = end ? end : NULL;
        while (s && s > q && s[-1] != '/') s--;
        if (s && end > s) {
            size_t n = (size_t)(end - s) < sizeof clean_name - 1 ? (size_t)(end - s) : sizeof clean_name - 1;
            memcpy(clean_name, s, n);
            clean_name[n] = 0;
            url_decode(clean_name);
        }
    }
    sanitize_name(clean_name);
    if (!clean_name[0] && !is_rd) snprintf(clean_name, sizeof clean_name, "download");
    if (sanitize_relpath(subdir ? subdir : "", clean_sub, sizeof clean_sub)) {
        snprintf(err, errlen, "subfolder too long");
        return -1;
    }
    static job tmp;
    pthread_mutex_lock(&g_lock);
    snprintf(tmp.folder, sizeof tmp.folder, "%s", folder);
    size_t tl = strlen(tmp.folder);
    while (tl > 1 && tmp.folder[tl - 1] == '/') tmp.folder[--tl] = 0;
    snprintf(tmp.subdir, sizeof tmp.subdir, "%s", clean_sub);
    snprintf(tmp.name, sizeof tmp.name, "%s", clean_name);
    build_path(&tmp);
    for (int i = 0; i < g_njobs; i++) {
        job *o = g_jobs[i];
        if ((o->state == J_DONE || o->state == J_CANCELLED) || !tmp.name[0] || strcmp(o->path, tmp.path)) continue;
        pthread_mutex_unlock(&g_lock);
        snprintf(err, errlen, "already in the queue");
        return -1;
    }
    job *j = new_job_locked();
    if (!j) {
        pthread_mutex_unlock(&g_lock);
        snprintf(err, errlen, "queue full");
        return -1;
    }
    snprintf(j->kind, sizeof j->kind, "%s", kind);
    j->item_id = item_id, j->file_id = file_id;
    if (is_url) j->url = strdup(url);
    snprintf(j->folder, sizeof j->folder, "%s", tmp.folder);
    snprintf(j->subdir, sizeof j->subdir, "%s", clean_sub);
    snprintf(j->name, sizeof j->name, "%s", clean_name);
    build_path(j);
    j->state = J_QUEUED;
    int id = j->id;
    save_queue_locked();
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    plat_log("job %d: queued %s", id, j->path);
    return id;
}

static job *find_locked(int id) {
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i]->id == id) return g_jobs[i];
    return NULL;
}

static int stop_job(int id, int how) {
    pthread_mutex_lock(&g_lock);
    job *j = find_locked(id);
    int rc = -1;
    if (j) {
        rc = 0;
        if (j->state == J_QUEUED || (how == 2 && (j->state == J_FAILED || j->state == J_PAUSED))) {
            j->state = how == 2 ? J_CANCELLED : J_PAUSED;
            if (how == 2) {
                char p[1900];
                snprintf(p, sizeof p, "%s.part", j->path);
                unlink(p);
                snprintf(p, sizeof p, "%s.part.state", j->path);
                unlink(p);
            }
            save_queue_locked();
        } else if (j->state == J_STARTING || j->state == J_RUNNING) j->stop = how;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int dl_cancel(int id) { return stop_job(id, 2); }
int dl_pause(int id) { return stop_job(id, 1); }

int dl_retry(int id) {
    pthread_mutex_lock(&g_lock);
    job *j = find_locked(id);
    int rc = -1;
    if (j && (j->state == J_FAILED || j->state == J_PAUSED || j->state == J_CANCELLED)) {
        j->state = J_QUEUED;
        j->err[0] = j->note[0] = 0;
        save_queue_locked();
        pthread_cond_broadcast(&g_cv);
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static void remove_at_locked(int i) {
    free(g_jobs[i]->url), free(g_jobs[i]->direct), free(g_jobs[i]);
    memmove(g_jobs + i, g_jobs + i + 1, (g_njobs - i - 1) * sizeof *g_jobs);
    g_njobs--;
}

int dl_remove(int id) {
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    for (int i = 0; i < g_njobs; i++) {
        job *j = g_jobs[i];
        if (j->id != id) continue;
        if (j->state == J_STARTING || j->state == J_RUNNING) break;
        remove_at_locked(i);
        save_queue_locked();
        rc = 0;
        break;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void dl_clear_finished(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = g_njobs - 1; i >= 0; i--)
        if (g_jobs[i]->state == J_DONE || g_jobs[i]->state == J_CANCELLED) remove_at_locked(i);
    save_queue_locked();
    pthread_mutex_unlock(&g_lock);
}

int dl_active_count(void) {
    pthread_mutex_lock(&g_lock);
    int n = 0;
    for (int i = 0; i < g_njobs; i++) n += g_jobs[i]->state <= J_RUNNING;
    pthread_mutex_unlock(&g_lock);
    return n;
}

void dl_settings(int parallel, int connections) {
    pthread_mutex_lock(&g_lock);
    if (parallel >= 1 && parallel <= 3) g_parallel = parallel;
    if (connections >= 0 && connections <= MAX_CONNS) g_connections = connections;
    save_settings_locked();
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
}

void dl_jobs_json(sbuf *o) {
    pthread_mutex_lock(&g_lock);
    sb_printf(o, "{\"parallel\":%d,\"connections\":%d,\"jobs\":[", g_parallel, g_connections);
    for (int i = 0; i < g_njobs; i++) {
        job *j = g_jobs[i];
        if (i) sb_puts(o, ",");
        sb_printf(o, "{\"id\":%d,\"kind\":\"%s\",\"state\":\"%s\",\"total\":%lld,\"done\":%lld,\"speed\":%.0f,"
                     "\"disk_speed\":%.0f,\"buffered\":%zu,\"write_max_ms\":%u,\"paused_conns\":%d,"
                     "\"conns\":%d,\"win\":%d,\"active\":%d,\"waiting\":%d,\"rcvbuf\":%d,\"usb_mode\":%s,"
                     "\"http\":%ld,\"http_ver\":",
                  j->id, j->kind, state_names[j->state], (long long)j->total, (long long)j->done, j->speed,
                  j->disk_speed, j->buffered, j->write_max_ms, j->paused_conns, j->conns, j->win, j->active,
                  j->waiting, j->rcvbuf, j->usb_mode ? "true" : "false", j->last_code);
        sb_json_str(o, j->http_ver);
        sb_puts(o, ",\"cdn\":");
        sb_json_str(o, j->cdn);
        sb_puts(o, ",\"name\":");
        char shown[800];
        snprintf(shown, sizeof shown, "%s%s%s", j->subdir, j->subdir[0] ? "/" : "",
                 j->name[0] ? j->name : "(name comes from Real-Debrid)");
        sb_json_str(o, shown);
        sb_puts(o, ",\"path\":");
        sb_json_str(o, j->path);
        sb_puts(o, ",\"err\":");
        sb_json_str(o, j->err);
        sb_puts(o, ",\"note\":");
        sb_json_str(o, j->note);
        sb_puts(o, "}");
    }
    sb_puts(o, "]}");
    pthread_mutex_unlock(&g_lock);
}

void dl_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    g_quit = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < 100 && g_running; i++) plat_sleep_ms(100);
    pthread_mutex_lock(&g_lock);
    save_queue_locked();
    pthread_mutex_unlock(&g_lock);
}
