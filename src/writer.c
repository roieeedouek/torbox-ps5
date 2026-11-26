// Decoupled asynchronous disk writer: coalesces contiguous 128 KiB blocks up to 2 MiB per pwrite()
// on a dedicated thread so FreeBSD/PS5 vnode locks and slow disk flushes never stall libcurl reception.
#include "app.h"

#include <curl/curl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define WSTAGE_SIZE (2 * 1024 * 1024)
#define WBATCH_MAX (WSTAGE_SIZE / WBLOCK_SIZE)

typedef struct {
    wblock *b;
    int seg_idx;
    int64_t offset;
    size_t len;
} wcmd;

struct writer {
    int fd;
    void *wakeup_multi;
    writer_commit_fn on_commit;
    writer_state_fn on_state;
    void *cb_ctx;

    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;

    char *slab;
    wblock *blocks;
    wblock **free_list;
    int pool_blocks;
    int free_count;

    wcmd *queue;
    int q_head, q_tail, q_count, q_cap;

    char *staging;
    size_t buffered;
    int64_t written;
    uint64_t write_calls;
    unsigned write_max_us;

    volatile int disk_error;
    int closing;
    int dirty;
    int64_t last_state_ms;

    int test_delay_ms;
    int test_reject_gaps;
};

static int write_all_pos(writer *w, const char *buf, size_t len, int64_t offset) {
#ifndef __PROSPERO__
    if (w->test_delay_ms > 0) plat_sleep_ms(w->test_delay_ms);
    if (w->test_reject_gaps) {
        struct stat st;
        if (fstat(w->fd, &st) != 0) return errno ? errno : EIO;
        if (offset > (int64_t)st.st_size) return EIO;
    }
#endif
    size_t at = 0;
    while (at < len) {
        ssize_t n = pwrite(w->fd, buf + at, len - at, (off_t)(offset + (int64_t)at));
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno ? errno : EIO;
        }
        if (n == 0) return EIO;
        at += (size_t)n;
    }
    return 0;
}

static void *writer_thread(void *arg) {
    writer *w = arg;
    wcmd batch[WBATCH_MAX];
    w->last_state_ms = plat_now_ms();

    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (w->q_count == 0 && !w->closing) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            struct timespec ts;
            ts.tv_sec = tv.tv_sec;
            ts.tv_nsec = (tv.tv_usec + 250000L) * 1000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec %= 1000000000L;
            }
            pthread_cond_timedwait(&w->cv, &w->mu, &ts);
            if (w->q_count == 0 && !w->closing && w->dirty && w->on_state &&
                plat_now_ms() - w->last_state_ms >= 5000)
                break;
        }

        int nbatch = 0;
        if (w->q_count > 0) {
            batch[nbatch++] = w->queue[w->q_head];
            w->q_head = (w->q_head + 1) % w->q_cap;
            w->q_count--;

            // Gather contiguous blocks for the same segment up to WSTAGE_SIZE.
            int seg_idx = batch[0].seg_idx;
            int64_t end = batch[0].offset + (int64_t)batch[0].len;
            size_t total = batch[0].len;
            int scan = w->q_count;
            for (int i = 0; i < scan && nbatch < WBATCH_MAX; i++) {
                int idx = (w->q_head + i) % w->q_cap;
                wcmd c = w->queue[idx];
                if (c.seg_idx == seg_idx && c.offset == end && total + c.len <= WSTAGE_SIZE) {
                    batch[nbatch++] = c;
                    end += (int64_t)c.len;
                    total += c.len;
                    // Remove idx by shifting the prefix [q_head .. idx) forward by 1.
                    for (int k = i; k > 0; k--) {
                        int dst = (w->q_head + k) % w->q_cap;
                        int src = (w->q_head + k - 1) % w->q_cap;
                        w->queue[dst] = w->queue[src];
                    }
                    w->q_head = (w->q_head + 1) % w->q_cap;
                    w->q_count--;
                    i--;
                    scan--;
                }
            }
        }
        int done = w->closing && w->q_count == 0 && nbatch == 0;
        int should_state = w->dirty && w->on_state &&
                           (done || plat_now_ms() - w->last_state_ms >= 5000);
        if (should_state) w->dirty = 0;
        pthread_mutex_unlock(&w->mu);

        if (nbatch > 0) {
            size_t total = 0;
            for (int i = 0; i < nbatch; i++) total += batch[i].len;
            int err = __atomic_load_n(&w->disk_error, __ATOMIC_ACQUIRE);
            unsigned elapsed_us = 0;
            if (!err) {
                const char *src;
                if (nbatch == 1) {
                    src = batch[0].b->data;
                } else {
                    size_t off = 0;
                    for (int i = 0; i < nbatch; i++) {
                        memcpy(w->staging + off, batch[i].b->data, batch[i].len);
                        off += batch[i].len;
                    }
                    src = w->staging;
                }
                int64_t t0 = plat_now_us();
                err = write_all_pos(w, src, total, batch[0].offset);
                int64_t dt = plat_now_us() - t0;
                if (dt > 0) elapsed_us = (unsigned)(dt > 60000000LL ? 60000000LL : dt);
                if (err) __atomic_store_n(&w->disk_error, err, __ATOMIC_RELEASE);
                else if (w->on_commit) w->on_commit(w->cb_ctx, batch[0].seg_idx, batch[0].offset + (int64_t)total);
            }

            pthread_mutex_lock(&w->mu);
            for (int i = 0; i < nbatch; i++) {
                w->free_list[w->free_count++] = batch[i].b;
                w->buffered -= batch[i].len;
            }
            if (!err) {
                w->written += (int64_t)total;
                w->write_calls++;
                if (elapsed_us > w->write_max_us) w->write_max_us = elapsed_us;
                w->dirty = 1;
            }
            pthread_mutex_unlock(&w->mu);

            if (w->wakeup_multi) curl_multi_wakeup((CURLM *)w->wakeup_multi);
        }

        if (should_state && !__atomic_load_n(&w->disk_error, __ATOMIC_ACQUIRE)) {
            w->on_state(w->cb_ctx);
            w->last_state_ms = plat_now_ms();
        }

        if (done) break;
    }
    return NULL;
}

writer *writer_start(int fd, int pool_blocks, void *wakeup_multi, writer_commit_fn on_commit, writer_state_fn on_state,
                     void *cb_ctx) {
    if (pool_blocks < 8) pool_blocks = 8;
    writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->fd = fd;
    w->wakeup_multi = wakeup_multi;
    w->on_commit = on_commit;
    w->on_state = on_state;
    w->cb_ctx = cb_ctx;
    w->pool_blocks = pool_blocks;
    w->q_cap = pool_blocks + 8;

    w->slab = malloc((size_t)pool_blocks * WBLOCK_SIZE);
    w->blocks = calloc((size_t)pool_blocks, sizeof *w->blocks);
    w->free_list = malloc((size_t)pool_blocks * sizeof *w->free_list);
    w->queue = malloc((size_t)w->q_cap * sizeof *w->queue);
    w->staging = malloc(WSTAGE_SIZE);
    if (!w->slab || !w->blocks || !w->free_list || !w->queue || !w->staging) {
        free(w->slab), free(w->blocks), free(w->free_list), free(w->queue), free(w->staging), free(w);
        return NULL;
    }
    for (int i = 0; i < pool_blocks; i++) {
        w->blocks[i].data = w->slab + (size_t)i * WBLOCK_SIZE;
        w->free_list[i] = &w->blocks[i];
    }
    w->free_count = pool_blocks;

#ifndef __PROSPERO__
    const char *dly = getenv("TORBOX_TEST_WRITE_DELAY_MS");
    if (dly && atoi(dly) > 0) w->test_delay_ms = atoi(dly);
    const char *gaps = getenv("TORBOX_TEST_REJECT_GAPS");
    if (gaps && !strcmp(gaps, "1")) w->test_reject_gaps = 1;
#endif

    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    int rc = pthread_create(&w->thread, &attr, writer_thread, w);
    pthread_attr_destroy(&attr);
    if (rc) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        free(w->slab), free(w->blocks), free(w->free_list), free(w->queue), free(w->staging), free(w);
        return NULL;
    }
    return w;
}

wblock *writer_take(writer *w) {
    pthread_mutex_lock(&w->mu);
    wblock *b = w->free_count > 0 ? w->free_list[--w->free_count] : NULL;
    pthread_mutex_unlock(&w->mu);
    return b;
}

void writer_release(writer *w, wblock *b) {
    if (!b) return;
    pthread_mutex_lock(&w->mu);
    w->free_list[w->free_count++] = b;
    pthread_mutex_unlock(&w->mu);
    if (w->wakeup_multi) curl_multi_wakeup((CURLM *)w->wakeup_multi);
}

void writer_enqueue(writer *w, wblock *b, int seg_idx, int64_t offset, size_t len) {
    if (!b || len == 0) {
        if (b) writer_release(w, b);
        return;
    }
    pthread_mutex_lock(&w->mu);
    w->queue[w->q_tail] = (wcmd){b, seg_idx, offset, len};
    w->q_tail = (w->q_tail + 1) % w->q_cap;
    w->q_count++;
    w->buffered += len;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

int writer_has_free(writer *w) {
    pthread_mutex_lock(&w->mu);
    int has = w->free_count > 0;
    pthread_mutex_unlock(&w->mu);
    return has;
}

int writer_disk_error(writer *w) { return w ? __atomic_load_n(&w->disk_error, __ATOMIC_ACQUIRE) : 0; }

void writer_stats(writer *w, int64_t *written, size_t *buffered, unsigned *write_max_ms, uint64_t *write_calls) {
    pthread_mutex_lock(&w->mu);
    if (written) *written = w->written;
    if (buffered) *buffered = w->buffered;
    if (write_max_ms) {
        *write_max_ms = w->write_max_us / 1000;
        w->write_max_us = 0;
    }
    if (write_calls) *write_calls = w->write_calls;
    pthread_mutex_unlock(&w->mu);
}

int writer_finish(writer *w, int do_fsync) {
    if (!w) return 0;
    pthread_mutex_lock(&w->mu);
    w->closing = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);

    pthread_join(w->thread, NULL);

    int err = __atomic_load_n(&w->disk_error, __ATOMIC_ACQUIRE);
    if (do_fsync && !err) {
        while (fsync(w->fd) != 0) {
            if (errno == EINTR) continue;
            err = errno ? errno : EIO;
            break;
        }
    }

    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    free(w->slab);
    free(w->blocks);
    free(w->free_list);
    free(w->queue);
    free(w->staging);
    free(w);
    return err;
}
