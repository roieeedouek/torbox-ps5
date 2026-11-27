// Shared HTTPS client (libcurl) and per-provider credential storage.
// Credentials are never logged and never sent to the browser.
#include "app.h"

#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------- providers and their stored keys ----------

const provider providers[PROV_COUNT] = {
    {"torbox", "TorBox", "torbox-key.txt"},
    {"realdebrid", "Real-Debrid", "realdebrid-token.txt"},
    {"alldebrid", "AllDebrid", "alldebrid-key.txt"},
};

static pthread_mutex_t g_cred_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_cred_path[PROV_COUNT][600];

int prov_find(const char *id) {
    for (int i = 0; i < PROV_COUNT; i++)
        if (id && !strcmp(id, providers[i].id)) return i;
    return -1;
}

const char *cred_path(int p) {
    if (!g_cred_path[p][0]) snprintf(g_cred_path[p], sizeof g_cred_path[p], "%s/%s", plat_app_dir(), providers[p].key_file);
    return g_cred_path[p];
}

int cred_get(int p, char *out, size_t len) {
    pthread_mutex_lock(&g_cred_lock);
    int rc = -1;
    FILE *f = fopen(cred_path(p), "r");
    if (f) {
        if (fgets(out, (int)len, f)) {
            out[strcspn(out, "\r\n")] = 0;
            rc = out[0] ? 0 : -1;
        }
        fclose(f);
    }
    pthread_mutex_unlock(&g_cred_lock);
    return rc;
}

int cred_set(int p, const char *key) {
    pthread_mutex_lock(&g_cred_lock);
    int rc = -1;
    int fd = open(cred_path(p), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        size_t n = strlen(key);
        rc = write(fd, key, n) == (ssize_t)n ? 0 : -1;
        close(fd);
    }
    pthread_mutex_unlock(&g_cred_lock);
    return rc;
}

void cred_clear(int p) {
    pthread_mutex_lock(&g_cred_lock);
    unlink(cred_path(p));
    pthread_mutex_unlock(&g_cred_lock);
}

// ---------- HTTPS ----------

// The console gives sockets a 64 KiB receive buffer, which caps one connection at window / RTT (about 1.2 MB/s at
// 60 ms). Probe descending sizes without shrinking the system default, and record what the kernel granted.
static int big_rcvbuf(void *ud, curl_socket_t fd, curlsocktype purpose) {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    net_sock_info *si = ud;
    int before = 0, after = 0, last_err = 0;
    socklen_t l = sizeof before;
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &before, &l)) {
        last_err = errno;
        before = 0;
    }
    after = before;
    static const int sizes[] = {2 << 20, 1536 << 10, 1 << 20, 896 << 10, 768 << 10, 512 << 10, 384 << 10, 256 << 10};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        if (sizes[i] <= before) break;
        int got = 0;
        l = sizeof got;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sizes[i], sizeof sizes[i])) {
            last_err = errno;
            continue;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &l)) {
            last_err = errno;
            continue;
        }
        after = got;
        if (got > before || got >= sizes[i] / 2) {
            last_err = 0;
            break;
        }
    }
    if (si) {
        si->before = before;
        si->after = after;
        si->err = last_err;
    }
    return CURL_SOCKOPT_OK;
}

void net_apply(void *h) {
    CURL *c = h;
    struct curl_blob blob = {(void *)cacert_pem, cacert_pem_len, CURL_BLOB_NOCOPY};
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "torbox-ps5/" APP_VERSION);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, big_rcvbuf);
    curl_easy_setopt(c, CURLOPT_SOCKOPTDATA, NULL);
#ifdef __PROSPERO__
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
#endif
}

void net_apply_dl(void *h, net_sock_info *si) {
    CURL *c = h;
    net_apply(c);
    curl_easy_setopt(c, CURLOPT_SOCKOPTDATA, si);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024);
}

void net_host_of(const char *url, char *out, size_t len) {
    if (!len) return;
    out[0] = 0;
    if (!url) return;
    const char *s = strstr(url, "://");
    s = s ? s + 3 : url;
    const char *end = s + strcspn(s, "/?#");
    const char *at = memchr(s, '@', (size_t)(end - s));
    if (at) s = at + 1;
    if (*s == '[') {
        const char *rb = memchr(s, ']', (size_t)(end - s));
        if (rb) end = rb + 1;
    } else {
        const char *colon = memchr(s, ':', (size_t)(end - s));
        if (colon) end = colon;
    }
    size_t n = (size_t)(end - s);
    if (n >= len) n = len - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[n] = 0;
}

static int host_has_suffix(const char *host, const char *domain) {
    size_t hl = strlen(host), dl = strlen(domain);
    if (hl == dl) return !strcmp(host, domain);
    return hl > dl && host[hl - dl - 1] == '.' && !strcmp(host + hl - dl, domain);
}

int net_is_debrid_cdn(const char *url) {
    char host[256];
    net_host_of(url, host, sizeof host);
    if (!host[0]) return 0;
    static const char *const domains[] = {
        "tb-cdn.pw", "tb-cdn.to", "tb-cdn.earth", "tb-cdn.st", "tb-cdn.io", "tb-cdn.cx",
        "real-debrid.com", "premiumize.me",
    };
    for (unsigned i = 0; i < sizeof domains / sizeof *domains; i++)
        if (host_has_suffix(host, domains[i])) return 1;
    if (!strcmp(host, "storage.torbox.app")) return 1;
    if (!strncmp(host, "storage-", 8) && host_has_suffix(host, "torbox.app")) return 1;
    return 0;
}

static size_t to_sbuf(char *p, size_t sz, size_t n, void *ud) {
    sbuf *b = ud;
    if (b->len + sz * n > 64u * 1024 * 1024) return 0; // a list never gets this big
    return sb_add(b, p, sz * n) ? 0 : sz * n;
}

static size_t total_header(char *p, size_t sz, size_t n, void *ud) {
    size_t len = sz * n;
    if (len > 14 && !strncasecmp(p, "X-Total-Count:", 14)) *(long long *)ud = atoll(p + 14);
    return len;
}

int net_request(const char *method, const char *url, const char *bearer, const char *form, sbuf *body,
                long long *total, char *err, size_t errlen) {
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(err, errlen, "curl init failed");
        return 0;
    }
    char auth[600];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", bearer ? bearer : "");
    struct curl_slist *hdr = bearer ? curl_slist_append(NULL, auth) : NULL;
    memset(auth, 0, sizeof auth);
    net_apply(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    if (!strcmp(method, "POST")) curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, form ? form : "");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // lists compress well
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    if (total) {
        *total = -1;
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, total_header);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, total);
    }
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else snprintf(err, errlen, "%s", curl_easy_strerror(rc));
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return (int)status;
}
