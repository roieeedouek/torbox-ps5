// TorBox REST client (https://api.torbox.app/v1/api) and API key storage.
// The key is never logged and never sent to the browser.
#include "app.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_key_path[600];
static char g_api_base[256];
static pthread_mutex_t g_key_lock = PTHREAD_MUTEX_INITIALIZER;

void tb_init(void) {
    snprintf(g_key_path, sizeof g_key_path, "%s/torbox-key.txt", plat_app_dir());
    const char *base = getenv("TORBOX_API_BASE"); // host tests point this at tests/mock_torbox.py
    snprintf(g_api_base, sizeof g_api_base, "%s", base ? base : "https://api.torbox.app/v1/api");
}

const char *tb_api_base(void) { return g_api_base; }
const char *tb_key_path(void) { return g_key_path; }

int tb_get_key(char *out, size_t len) {
    pthread_mutex_lock(&g_key_lock);
    int rc = -1;
    FILE *f = fopen(g_key_path, "r");
    if (f) {
        if (fgets(out, (int)len, f)) {
            out[strcspn(out, "\r\n")] = 0;
            rc = out[0] ? 0 : -1;
        }
        fclose(f);
    }
    pthread_mutex_unlock(&g_key_lock);
    return rc;
}

int tb_have_key(void) {
    char k[256];
    int rc = tb_get_key(k, sizeof k) == 0;
    memset(k, 0, sizeof k);
    return rc;
}

int tb_set_key(const char *key) {
    pthread_mutex_lock(&g_key_lock);
    int rc = -1;
    int fd = open(g_key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        size_t n = strlen(key);
        rc = write(fd, key, n) == (ssize_t)n ? 0 : -1;
        close(fd);
    }
    pthread_mutex_unlock(&g_key_lock);
    return rc;
}

void tb_clear_key(void) {
    pthread_mutex_lock(&g_key_lock);
    unlink(g_key_path);
    pthread_mutex_unlock(&g_key_lock);
}

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

void tb_apply_tls(void *h) {
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

int tb_get(const char *path_and_query, const char *key, sbuf *body, char *err, size_t errlen) {
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(err, errlen, "curl init failed");
        return 0;
    }
    char url[2048], auth[300];
    snprintf(url, sizeof url, "%s/%s", g_api_base, path_and_query);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
    struct curl_slist *hdr = curl_slist_append(NULL, auth);
    memset(auth, 0, sizeof auth);
    tb_apply_tls(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); // lists compress well
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_sbuf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    else snprintf(err, errlen, "%s", curl_easy_strerror(rc));
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return (int)status;
}

// Puts TorBox's "detail" (or "error") of a failed response into err.
static void api_error(const sbuf *b, int status, char *err, size_t errlen) {
    char detail[300] = "", code[64] = "";
    if (b->data) {
        json_string(json_find(b->data, "detail"), detail, sizeof detail);
        json_string(json_find(b->data, "error"), code, sizeof code);
    }
    if (detail[0]) snprintf(err, errlen, "%s%s%s", detail, code[0] ? " " : "", code);
    else snprintf(err, errlen, "TorBox answered HTTP %d", status);
}

int tb_request_link(const char *kind, long long item_id, long long file_id, char *url, size_t urllen, char *err,
                    size_t errlen) {
    char key[256];
    if (tb_get_key(key, sizeof key)) {
        snprintf(err, errlen, "not logged in");
        return -1;
    }
    const char *idname = !strcmp(kind, "torrents") ? "torrent_id" : !strcmp(kind, "webdl") ? "web_id" : "usenet_id";
    char q[600], ekey[400];
    url_encode(key, ekey, sizeof ekey);
    if (file_id >= 0)
        snprintf(q, sizeof q, "%s/requestdl?token=%s&%s=%lld&file_id=%lld", kind, ekey, idname, item_id, file_id);
    else snprintf(q, sizeof q, "%s/requestdl?token=%s&%s=%lld&zip_link=true", kind, ekey, idname, item_id);
    sbuf b;
    sb_init(&b);
    int status = tb_get(q, key, &b, err, errlen);
    memset(key, 0, sizeof key), memset(ekey, 0, sizeof ekey), memset(q, 0, sizeof q);
    int rc = -1;
    if (status == 200 && b.data && json_bool(json_find(b.data, "success")) == 1 &&
        !json_string(json_find(b.data, "data"), url, urllen) && !strncmp(url, "http", 4))
        rc = 0;
    else if (status) api_error(&b, status, err, errlen);
    sb_free(&b);
    return rc;
}
