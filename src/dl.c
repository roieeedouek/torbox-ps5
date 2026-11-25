// Download queue: a few jobs at a time, each split over several HTTP Range connections when the server allows it.
//
// A job writes <folder>/<name>.part; a multi-connection job also keeps <name>.part.state (the segment table), so a
// paused, failed or interrupted job (even across restarts) resumes where it stopped. Finished: renamed to <name>.
// TorBox links are requested when the job starts (and again after an error), never stored.
#include "app.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { J_QUEUED, J_STARTING, J_RUNNING, J_DONE, J_FAILED, J_PAUSED, J_CANCELLED };
static const char *state_names[] = {"queued", "starting", "downloading", "done", "failed", "paused", "cancelled"};

#define MAX_JOBS 1000
#define MAX_CONNS 32                   // connections per file
#define MAX_SEGS 256                   // segment table entries (they grow when idle connections split segments)
#define MULTI_MIN (8ll * 1024 * 1024)  // smaller files use one connection
#define SPLIT_MIN (8ll * 1024 * 1024)  // only segments with at least this much left are split
#define WBUF (2 * 1024 * 1024)         // write buffer per connection (1 MiB writes measured 130 MB/s on /data, 4 MiB 179)

typedef struct job {
    int id, state;
    volatile int stop; // 1 = pause, 2 = cancel (delete the partial file)
    char kind[12];
    long long item_id, file_id;
    char *url; // kind "url" only
    char folder[1024], subdir[512], name[256], path[1800];
    int64_t total, done, start_done, started_ms, finished_ms;
    double speed;
    int conns, active, waiting; // started connections; of those, receiving now; waiting out a server limit
    long last_code;             // last HTTP status a connection got
    char err[300], note[160];
} job;

static job *g_jobs[MAX_JOBS];
static int g_njobs, g_next_id = 1, g_running;
static int g_parallel = 1, g_connections = 16;
static volatile int g_quit;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

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
        if (sscanf(line, "connections=%d", &v) == 1 && v >= 1 && v <= MAX_CONNS) g_connections = v;
    }
    fclose(f);
}

static job *new_job_locked(void) {
    if (g_njobs == MAX_JOBS) { // drop the oldest finished job
        int i;
        for (i = 0; i < g_njobs; i++)
            if (g_jobs[i]->state >= J_DONE && g_jobs[i]->state != J_PAUSED) break;
        if (i == g_njobs) return NULL;
        free(g_jobs[i]->url), free(g_jobs[i]);
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
        snprintf(j->note, sizeof j->note, "restored after restart");
    }
    free(line);
    fclose(f);
}

// ---------- transfers ----------

typedef struct seg {
    int64_t start, end; // inclusive range
    volatile int64_t pos;  // next byte to write (on disk)
    volatile int64_t seen; // received, including what still sits in the write buffer (progress display)
    volatile int64_t last_data_ms;
    volatile int waiting;  // in a 429/503 back-off
    int owned;             // a connection works on it (x->lock)
} seg;

typedef struct xfer {
    job *j;
    const char *url;
    int fd;
    volatile int nseg; // grows (under lock) when a segment is split
    seg segs[MAX_SEGS];
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int live;          // running connection threads
    int failed;        // a connection gave up
    long http_error;   // last HTTP status >= 400 seen
    volatile long last_code;
    int disk_error;    // errno of a failed write
    char err[256];
} xfer;

typedef struct conn {
    xfer *x;
    int index; // segment, or -1 for the single stream
    char *buf;
    size_t used;
    int64_t off;    // file offset of buf[0]
    int checked;    // response code verified
    CURL *h;
} conn;

static int flush_conn(conn *c) {
    size_t at = 0;
    while (at < c->used) {
        ssize_t n = pwrite(c->x->fd, c->buf + at, c->used - at, c->off + at);
        if (n < 0) {
            if (errno == EINTR) continue;
            c->x->disk_error = errno;
            return -1;
        }
        at += n;
    }
    c->off += c->used;
    if (c->index >= 0) __atomic_store_n(&c->x->segs[c->index].pos, c->off, __ATOMIC_RELAXED);
    else __atomic_store_n(&c->x->segs[0].pos, c->off, __ATOMIC_RELAXED);
    c->used = 0;
    return 0;
}

static size_t on_data(char *p, size_t sz, size_t n, void *ud) {
    conn *c = ud;
    size_t len = sz * n;
    if (!c->checked) {
        c->checked = 1;
        long code = 0;
        curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &code);
        if (c->index >= 0 && code != 206) {
            snprintf(c->x->err, sizeof c->x->err, "server ignored the byte range (HTTP %ld)", code);
            return 0;
        }
        if (c->index < 0 && c->off > 0 && code == 200) { // resume refused: start over
            if (ftruncate(c->x->fd, 0)) { /* pwrite from 0 overwrites anyway */ }
            c->off = 0;
            __atomic_store_n(&c->x->segs[0].pos, 0, __ATOMIC_RELAXED);
            c->x->segs[0].start = 0;
        }
    }
    if (c->index >= 0) { // never write past the segment end
        seg *s = &c->x->segs[c->index];
        int64_t room = __atomic_load_n(&s->end, __ATOMIC_ACQUIRE) + 1 - (c->off + (int64_t)c->used);
        if (room <= 0) return 0;
        if ((int64_t)len > room) len = (size_t)room;
    }
    size_t at = 0;
    while (at < len) {
        size_t take = WBUF - c->used < len - at ? WBUF - c->used : len - at;
        memcpy(c->buf + c->used, p + at, take);
        c->used += take, at += take;
        if (c->used == WBUF && flush_conn(c)) return 0;
    }
    seg *sg = &c->x->segs[c->index >= 0 ? c->index : 0];
    __atomic_store_n(&sg->seen, c->off + (int64_t)c->used, __ATOMIC_RELAXED);
    sg->last_data_ms = plat_now_ms();
    return sz * n;
}

static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt, (void)dn, (void)ut, (void)un;
    conn *c = ud;
    return c->x->j->stop || g_quit || c->x->failed;
}

// Fetches the connection's segment (or the whole file), retrying network errors a few times. 1 = complete.
static int fetch(conn *c) {
    xfer *x = c->x;
    CURL *h = c->h;
    int ok = 0, busy = 0;
    for (int attempt = 0; h && attempt < 4; attempt++) {
        seg *s = &x->segs[c->index >= 0 ? c->index : 0];
        c->off = __atomic_load_n(&s->pos, __ATOMIC_RELAXED);
        c->used = 0, c->checked = 0;
        if (c->index >= 0 && c->off > __atomic_load_n(&s->end, __ATOMIC_ACQUIRE)) { // segment complete
            ok = 1;
            break;
        }
        curl_easy_reset(h);
        tb_apply_tls(h);
        curl_easy_setopt(h, CURLOPT_URL, x->url);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 512L * 1024);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 90L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, c);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, c);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        char range[64];
        if (c->index >= 0) {
            snprintf(range, sizeof range, "%lld-%lld", (long long)c->off,
                     (long long)__atomic_load_n(&s->end, __ATOMIC_ACQUIRE));
            curl_easy_setopt(h, CURLOPT_RANGE, range);
        } else if (c->off > 0) curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)c->off);
        int64_t before = c->off;
        CURLcode rc = curl_easy_perform(h);
        int flushed = flush_conn(c) == 0;
        if (flushed && c->index >= 0 &&
            __atomic_load_n(&s->pos, __ATOMIC_RELAXED) > __atomic_load_n(&s->end, __ATOMIC_ACQUIRE)) {
            ok = 1;
            break;
        }
        if (rc == CURLE_OK && flushed) {
            if (c->index < 0) {
                ok = 1;
                break;
            }
            if (c->off > before) attempt = -1; // the server closed early but sent data: fetch the rest
            continue;
        }
        if (x->j->stop || g_quit || x->failed || !flushed || x->disk_error) break;
        long code = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        if (code) x->last_code = code;
        if ((code == 429 || code == 503) && busy < 60) {
            // The server limits parallel connections: wait for a free slot (others keep going), then retry.
            int wait = 2 + busy * 2 < 20 ? 2 + busy * 2 : 20;
            busy++, attempt--;
            s->waiting = 1;
            for (int t = 0; t < wait * 4 && !x->j->stop && !g_quit && !x->failed; t++) plat_sleep_ms(250);
            s->waiting = 0;
            continue;
        }
        if (rc == CURLE_HTTP_RETURNED_ERROR || code >= 400) {
            pthread_mutex_lock(&x->lock);
            x->http_error = code;
            if (!x->err[0]) snprintf(x->err, sizeof x->err, "HTTP %ld", code);
            pthread_mutex_unlock(&x->lock);
            break; // the link may have expired: the job asks for a new one
        }
        if (rc == CURLE_WRITE_ERROR && x->err[0]) break; // range ignored
        if (c->off > before) { // the connection broke after some data: go on at once, not counted
            attempt = -1;
            continue;
        }
        pthread_mutex_lock(&x->lock);
        snprintf(x->err, sizeof x->err, "%s", curl_easy_strerror(rc));
        pthread_mutex_unlock(&x->lock);
        plat_sleep_ms(2000);
    }
    return ok;
}

// Gives a connection work: an unowned unfinished segment, or else the second half of the segment with the most
// left (its owner stops at the new end). -1 = nothing worth taking. Keeps every connection busy until the end.
static int claim(xfer *x) {
    pthread_mutex_lock(&x->lock);
    int best = -1;
    int64_t best_left = 0;
    for (int i = 0; i < x->nseg; i++) {
        seg *s = &x->segs[i];
        int64_t end = __atomic_load_n(&s->end, __ATOMIC_ACQUIRE);
        int64_t at = __atomic_load_n(&s->seen, __ATOMIC_RELAXED);
        int64_t pos = __atomic_load_n(&s->pos, __ATOMIC_RELAXED);
        if (at < pos) at = pos;
        if (!s->owned && pos <= end) {
            s->owned = 1;
            pthread_mutex_unlock(&x->lock);
            return i;
        }
        if (s->owned && end - at > best_left) best = i, best_left = end - at;
    }
    int got = -1;
    if (best >= 0 && best_left >= SPLIT_MIN && x->nseg < MAX_SEGS) {
        seg *s = &x->segs[best];
        int64_t at = __atomic_load_n(&s->seen, __ATOMIC_RELAXED);
        int64_t mid = at + best_left / 2;
        got = x->nseg;
        seg *n = &x->segs[got];
        memset(n, 0, sizeof *n);
        n->start = n->pos = n->seen = mid + 1;
        n->end = s->end;
        n->owned = 1;
        __atomic_store_n(&s->end, mid, __ATOMIC_RELEASE);
        __atomic_store_n(&x->nseg, got + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&x->lock);
    return got;
}

static void *conn_thread(void *arg) {
    conn *c = arg;
    xfer *x = c->x;
    c->h = curl_easy_init();
    int ok = c->h != NULL;
    while (ok) {
        ok = fetch(c);
        if (c->index < 0) break; // single stream
        pthread_mutex_lock(&x->lock);
        x->segs[c->index].owned = 0;
        pthread_mutex_unlock(&x->lock);
        if (!ok || x->j->stop || g_quit || x->failed) break;
        c->index = claim(x);
        if (c->index < 0) break;
    }
    if (c->h) curl_easy_cleanup(c->h);
    pthread_mutex_lock(&x->lock);
    if (!ok) x->failed = 1;
    x->live--;
    pthread_cond_broadcast(&x->cv);
    pthread_mutex_unlock(&x->lock);
    free(c->buf);
    free(c);
    return NULL;
}

// Finds the size and whether byte ranges work, with a "Range: bytes=0-0" request. Also resolves redirects.
typedef struct {
    long code;
    int64_t range_total, length;
} probe_info;

static size_t probe_header(char *p, size_t sz, size_t n, void *ud) {
    probe_info *pi = ud;
    size_t len = sz * n;
    char line[512];
    size_t l = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, p, l);
    line[l] = 0;
    if (!strncasecmp(line, "HTTP/", 5)) pi->range_total = -1, pi->length = -1;
    else if (!strncasecmp(line, "content-range:", 14)) {
        char *slash = strchr(line, '/');
        if (slash && slash[1] != '*') pi->range_total = atoll(slash + 1);
    } else if (!strncasecmp(line, "content-length:", 15)) pi->length = atoll(line + 15);
    return len;
}

static size_t probe_body(char *p, size_t sz, size_t n, void *ud) {
    (void)p, (void)ud;
    return sz * n > 1 ? 0 : sz * n; // a 200 answer would be the whole file: stop at once
}

static int probe(const char *url, int64_t *total, int *ranges, char *final_url, size_t final_len, char *err,
                 size_t errlen) {
    CURL *h = curl_easy_init();
    if (!h) return -1;
    probe_info pi = {0, -1, -1};
    tb_apply_tls(h);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, probe_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &pi);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, probe_body);
    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &pi.code);
    char *eff = NULL;
    curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
    snprintf(final_url, final_len, "%s", eff ? eff : url);
    curl_easy_cleanup(h);
    if (pi.code == 206) {
        *ranges = 1;
        *total = pi.range_total;
    } else if (pi.code == 200) {
        *ranges = 0;
        *total = pi.length;
    } else {
        if (pi.code >= 400) snprintf(err, errlen, "HTTP %ld", pi.code);
        else snprintf(err, errlen, "%s", curl_easy_strerror(rc));
        return pi.code >= 400 && pi.code != 429 && pi.code != 503 ? -2 : -1;
    }
    return 0;
}

static void state_path(const job *j, char *out, size_t len) { snprintf(out, len, "%s.part.state", j->path); }

static int load_state(xfer *x, int64_t total) {
    char path[1900];
    state_path(x->j, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long long t;
    int n;
    int ok = fscanf(f, "torbox-ps5 1 %lld %d\n", &t, &n) == 2 && t == total && n >= 1 && n <= MAX_SEGS;
    for (int i = 0; ok && i < n; i++) {
        long long s, e, p;
        ok = fscanf(f, "%lld %lld %lld\n", &s, &e, &p) == 3 && s <= e + 1 && p >= s && p <= e + 1;
        x->segs[i].start = s, x->segs[i].end = e, x->segs[i].pos = p;
    }
    fclose(f);
    if (!ok) return -1;
    int k = 0; // drop finished segments: the table only needs what is left
    for (int i = 0; i < n; i++)
        if (x->segs[i].pos <= x->segs[i].end) x->segs[k++] = x->segs[i];
    for (int i = 0; i < k; i++) x->segs[i].seen = x->segs[i].pos;
    if (k == 0) { // all done: keep one empty segment so the table stays valid
        x->segs[0].start = x->segs[0].pos = x->segs[0].seen = total, x->segs[0].end = total - 1;
        k = 1;
    }
    x->nseg = k;
    return 0;
}

static void save_state(xfer *x, int64_t total) {
    char path[1900], tmp[1920];
    state_path(x->j, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    pthread_mutex_lock(&x->lock);
    fprintf(f, "torbox-ps5 1 %lld %d\n", (long long)total, x->nseg);
    for (int i = 0; i < x->nseg; i++) {
        int64_t end = __atomic_load_n(&x->segs[i].end, __ATOMIC_ACQUIRE);
        int64_t pos = __atomic_load_n(&x->segs[i].pos, __ATOMIC_RELAXED);
        fprintf(f, "%lld %lld %lld\n", (long long)x->segs[i].start, (long long)end,
                (long long)(pos > end + 1 ? end + 1 : pos));
    }
    pthread_mutex_unlock(&x->lock);
    fclose(f);
    rename(tmp, path);
}

// Bytes done: on disk (seen = 0) or received (seen = 1, for the progress display). The segment table only
// covers what was left when the job started, so a segmented job counts total minus what is still missing.
static int64_t xfer_done(xfer *x, int single, int seen, int64_t total) {
    if (single) {
        int64_t p = __atomic_load_n(&x->segs[0].pos, __ATOMIC_RELAXED);
        int64_t r = __atomic_load_n(&x->segs[0].seen, __ATOMIC_RELAXED);
        return seen && r > p ? r : p;
    }
    int64_t left = 0;
    int n = __atomic_load_n(&x->nseg, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        int64_t end = __atomic_load_n(&x->segs[i].end, __ATOMIC_ACQUIRE);
        int64_t p = __atomic_load_n(&x->segs[i].pos, __ATOMIC_RELAXED);
        int64_t r = __atomic_load_n(&x->segs[i].seen, __ATOMIC_RELAXED);
        int64_t at = seen && r > p ? r : p;
        if (at <= end) left += end + 1 - at;
    }
    return total - left;
}

enum { T_OK, T_STOPPED, T_RETRY, T_FATAL, T_RELINK };

// Downloads url into j->path.part. Returns T_*.
static int transfer(job *j, const char *url, char *err, size_t errlen) {
    char final_url[4096], part[1900];
    int64_t total = -1;
    int ranges = 0;
    int pr = probe(url, &total, &ranges, final_url, sizeof final_url, err, errlen);
    if (pr == -2) return T_RELINK;
    if (pr) return T_RETRY;
    snprintf(part, sizeof part, "%s.part", j->path);
    xfer *x = calloc(1, sizeof *x);
    if (!x) return T_FATAL;
    x->j = j, x->url = final_url;
    pthread_mutex_init(&x->lock, NULL);
    pthread_cond_init(&x->cv, NULL);
    int conns = g_connections;
    struct stat st;
    int64_t have = stat(part, &st) ? 0 : st.st_size;
    // A segment table from an earlier run is always continued (whatever the connection count is now): a single
    // stream cannot fill its holes.
    int resumed = ranges && total > 0 && load_state(x, total) == 0 && have > 0;
    int single = !resumed && !(ranges && total >= MULTI_MIN && conns > 1);
    if (single) {
        x->nseg = 1;
        x->segs[0].start = 0;
        x->segs[0].end = total > 0 ? total - 1 : INT64_MAX;
        // A state file means the part has holes; a single stream cannot resume those.
        char sp[1900];
        state_path(j, sp, sizeof sp);
        if (!stat(sp, &st) && unlink(sp) == 0) have = 0, unlink(part);
        if (!ranges || (total > 0 && have > total)) have = 0, unlink(part);
        x->segs[0].pos = x->segs[0].seen = have;
        if (total > 0 && have == total) { // already complete
            free(x);
            return T_OK;
        }
    } else if (!resumed) {
        int n = conns < MAX_CONNS ? conns : MAX_CONNS;
        if (n < 1) n = 1;
        x->nseg = n;
        unlink(part); // any old part without a matching table cannot be trusted
        for (int i = 0; i < n; i++) {
            x->segs[i].start = total * i / n;
            x->segs[i].end = total * (i + 1) / n - 1;
            x->segs[i].pos = x->segs[i].seen = x->segs[i].start;
        }
    }
    x->fd = open(part, O_RDWR | O_CREAT, 0666);
    if (x->fd < 0) {
        snprintf(err, errlen, "cannot create %s: %s", part, strerror(errno));
        free(x);
        return T_FATAL;
    }
    if (!single) save_state(x, total);
    pthread_mutex_lock(&g_lock);
    j->total = total;
    j->done = xfer_done(x, single, 0, total);
    j->start_done = j->done;
    j->state = J_RUNNING;
    pthread_mutex_unlock(&g_lock);

    int started = 0;
    for (int i = 0; i < (single ? 1 : conns); i++) {
        int index = single ? -1 : claim(x);
        if (!single && index < 0) break;
        conn *c = calloc(1, sizeof *c);
        char *buf = c ? malloc(WBUF) : NULL;
        if (!buf) {
            free(c);
            if (!started) x->failed = 1;
            if (index >= 0) x->segs[index].owned = 0;
            break;
        }
        c->x = x, c->index = index, c->buf = buf;
        pthread_mutex_lock(&x->lock);
        x->live++;
        pthread_mutex_unlock(&x->lock);
        if (plat_start_thread(conn_thread, c)) {
            pthread_mutex_lock(&x->lock);
            x->live--;
            if (!started) x->failed = 1;
            if (index >= 0) x->segs[index].owned = 0;
            pthread_mutex_unlock(&x->lock);
            free(buf), free(c);
            break;
        }
        started++;
    }
    pthread_mutex_lock(&g_lock);
    j->conns = started;
    pthread_mutex_unlock(&g_lock);

    int64_t last_ms = plat_now_ms(), last_done = j->done, last_save = last_ms;
    double speed = 0;
    pthread_mutex_lock(&x->lock);
    while (x->live > 0) {
        pthread_mutex_unlock(&x->lock);
        plat_sleep_ms(500);
        int64_t now = plat_now_ms(), d = xfer_done(x, single, 1, total);
        if (now - last_ms >= 1000) {
            double inst = (double)(d - last_done) * 1000.0 / (double)(now - last_ms);
            speed = speed == 0 ? inst : speed * 0.6 + inst * 0.4;
            last_ms = now, last_done = d;
        }
        if (!single && now - last_save >= 5000) save_state(x, total), last_save = now;
        int active = 0, waiting = 0, n = __atomic_load_n(&x->nseg, __ATOMIC_ACQUIRE);
        for (int i = 0; i < n; i++) {
            active += now - x->segs[i].last_data_ms < 3000;
            waiting += x->segs[i].waiting;
        }
        pthread_mutex_lock(&x->lock);
        int live = x->live;
        pthread_mutex_unlock(&x->lock);
        pthread_mutex_lock(&g_lock);
        j->conns = live;
        j->done = d, j->speed = speed, j->active = active, j->waiting = waiting, j->last_code = x->last_code;
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_lock(&x->lock);
    }
    pthread_mutex_unlock(&x->lock);

    int64_t d = xfer_done(x, single, 0, total);
    int complete = !x->failed && !j->stop && !g_quit && !x->disk_error && (total < 0 || d >= total);
    if (fsync(x->fd)) { /* best effort */ }
    close(x->fd);
    pthread_mutex_lock(&g_lock);
    j->done = d, j->speed = 0, j->conns = j->active = j->waiting = 0;
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
        if (!single) save_state(x, total);
        if (j->stop || g_quit) result = T_STOPPED;
        else if (x->disk_error) {
            snprintf(err, errlen, "write failed: %s", strerror(x->disk_error));
            result = T_FATAL;
        } else if (x->http_error) {
            snprintf(err, errlen, "%s", x->err);
            result = T_RELINK;
        } else {
            snprintf(err, errlen, "%s", x->err[0] ? x->err : "transfer incomplete");
            result = T_RETRY;
        }
    }
    pthread_mutex_destroy(&x->lock);
    pthread_cond_destroy(&x->cv);
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

static void run_job(job *j) {
    char err[300] = "", url[4096];
    char dir[1800];
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
    for (int attempt = 0; attempt < 6; attempt++) {
        if (attempt) plat_sleep_ms(attempt < 3 ? 3000 : 10000);
        if (j->stop || g_quit) break;
        if (!strcmp(j->kind, "url")) snprintf(url, sizeof url, "%s", j->url ? j->url : "");
        else if (tb_request_link(j->kind, j->item_id, j->file_id, url, sizeof url, err, sizeof err)) {
            pthread_mutex_lock(&g_lock);
            snprintf(j->err, sizeof j->err, "link request: %s", err);
            pthread_mutex_unlock(&g_lock);
            if (strstr(err, "not logged in") || strstr(err, "BAD_TOKEN") || strstr(err, "LIMIT")) break;
            continue;
        }
        int r = transfer(j, url, err, sizeof err);
        memset(url, 0, sizeof url);
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
        pthread_mutex_lock(&g_lock);
        snprintf(j->err, sizeof j->err, "%s", err);
        pthread_mutex_unlock(&g_lock);
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
    else if (g_quit) set_state(j, J_QUEUED, ""); // resumes on the next start
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
    int is_url = !strcmp(kind, "url");
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
    snprintf(clean_name, sizeof clean_name, "%s", name && *name ? name : "download");
    if (is_url && (!name || !*name)) { // name from the URL path
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
    if (!clean_name[0]) snprintf(clean_name, sizeof clean_name, "download");
    if (sanitize_relpath(subdir ? subdir : "", clean_sub, sizeof clean_sub)) {
        snprintf(err, errlen, "subfolder too long");
        return -1;
    }
    static job tmp; // only used under g_lock
    pthread_mutex_lock(&g_lock);
    snprintf(tmp.folder, sizeof tmp.folder, "%s", folder);
    size_t tl = strlen(tmp.folder);
    while (tl > 1 && tmp.folder[tl - 1] == '/') tmp.folder[--tl] = 0;
    snprintf(tmp.subdir, sizeof tmp.subdir, "%s", clean_sub);
    snprintf(tmp.name, sizeof tmp.name, "%s", clean_name);
    build_path(&tmp);
    for (int i = 0; i < g_njobs; i++) { // the same target twice would fight over one .part file
        job *o = g_jobs[i];
        if ((o->state == J_DONE || o->state == J_CANCELLED) || strcmp(o->path, tmp.path)) continue;
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
    free(g_jobs[i]->url), free(g_jobs[i]);
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
        if (j->state == J_FAILED || j->state == J_PAUSED || j->state == J_QUEUED) { // forget the partial file too
            char p[1900];
            snprintf(p, sizeof p, "%s.part", j->path);
            unlink(p);
            snprintf(p, sizeof p, "%s.part.state", j->path);
            unlink(p);
        }
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
    if (connections >= 1 && connections <= MAX_CONNS) g_connections = connections;
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
                     "\"conns\":%d,\"active\":%d,\"waiting\":%d,\"http\":%ld,\"name\":",
                  j->id, j->kind, state_names[j->state], (long long)j->total, (long long)j->done, j->speed, j->conns,
                  j->active, j->waiting, j->last_code);
        char shown[800];
        snprintf(shown, sizeof shown, "%s%s%s", j->subdir, j->subdir[0] ? "/" : "", j->name);
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
    for (int i = 0; i < 100 && g_running; i++) plat_sleep_ms(100); // the jobs save their state
    pthread_mutex_lock(&g_lock);
    save_queue_locked();
    pthread_mutex_unlock(&g_lock);
}
