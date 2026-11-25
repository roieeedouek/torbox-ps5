// Shared HTTPS client (libcurl) and per-provider credential storage.
// Credentials are never logged and never sent to the browser.
#include "app.h"

#include <curl/curl.h>
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
// 60 ms). 512 KiB, the most it grants, measured 7.9 MB/s on one connection. Set before connect() so the TCP
// handshake announces the bigger window.
static int big_rcvbuf(void *ud, curl_socket_t fd, curlsocktype purpose) {
    (void)ud;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    int sizes[] = {4 << 20, 1 << 20, 512 << 10, 256 << 10};
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int got = 0;
        socklen_t l = sizeof got;
        if (!setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sizes[i], sizeof sizes[i]) &&
            !getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &l) && got >= (256 << 10))
            break;
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
#ifdef __PROSPERO__
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
#endif
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
