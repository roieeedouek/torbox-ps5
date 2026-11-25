// Real-Debrid REST client (https://api.real-debrid.com/rest/1.0), with the private API token as Bearer.
//
// Real-Debrid allows about 250 requests a minute (refused ones count too): calls are spaced and an HTTP 429 is
// waited out. Errors come as {"error": "...", "error_code": N} and are turned into readable text.
#include "app.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RD_MIN_GAP_MS 260 // at most ~230 requests a minute

static char g_base[256];
static pthread_mutex_t g_rate_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_last_call_ms;

void rd_init(void) {
    const char *base = getenv("RD_API_BASE"); // host tests point this at tests/mock_torbox.py
    snprintf(g_base, sizeof g_base, "%s", base ? base : "https://api.real-debrid.com/rest/1.0");
}

static const char *code_text(long code) {
    switch (code) {
    case -1: return "Real-Debrid internal error";
    case 1: return "missing parameter";
    case 2: return "bad parameter value";
    case 5: return "slow down (too many requests)";
    case 6: return "resource unreachable";
    case 7: return "not found";
    case 8: return "bad or expired API token: log in again";
    case 9: return "permission denied (is the account premium?)";
    case 14: return "account locked";
    case 15: return "account not activated";
    case 16: return "unsupported hoster";
    case 17: return "hoster in maintenance";
    case 18: return "hoster limit reached";
    case 19: return "hoster temporarily unavailable";
    case 20: return "hoster not available for free users";
    case 21: return "too many active downloads";
    case 22: return "IP address not allowed";
    case 23: return "traffic exhausted";
    case 24: return "file unavailable on the hoster";
    case 25: return "Real-Debrid service unavailable";
    case 34: return "too many requests";
    case 35: return "infringing file";
    case 36: return "fair usage limit reached";
    case 37: return "endpoint disabled";
    default: return NULL;
    }
}

static void rd_error(const sbuf *b, int status, long *code_out, char *err, size_t errlen) {
    char msg[200] = "";
    long code = 0;
    if (b->data) {
        json_string(json_find(b->data, "error"), msg, sizeof msg);
        code = (long)json_int(json_find(b->data, "error_code"), 0);
    }
    if (code_out) *code_out = code;
    const char *text = code_text(code);
    if (text) snprintf(err, errlen, "Real-Debrid: %s (error %ld)", text, code);
    else if (msg[0]) snprintf(err, errlen, "Real-Debrid: %s", msg);
    else snprintf(err, errlen, "Real-Debrid answered HTTP %d", status);
}

// One API call: spaced to stay under the rate limit, HTTP 429 retried with back-off. key NULL = the stored token.
// Returns the HTTP status (0 = no answer, err set); on an error status err and *code hold Real-Debrid's error.
static int rd_call(const char *method, const char *path, const char *key, const char *form, sbuf *body,
                   long long *total, long *code, char *err, size_t errlen) {
    char stored[256];
    if (code) *code = 0;
    if (!key) {
        if (cred_get(PROV_RD, stored, sizeof stored)) {
            snprintf(err, errlen, "not logged in to Real-Debrid");
            return 0;
        }
        key = stored;
    }
    char url[4096];
    snprintf(url, sizeof url, "%s/%s", g_base, path);
    int status = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        pthread_mutex_lock(&g_rate_lock);
        int64_t wait = g_last_call_ms + RD_MIN_GAP_MS - plat_now_ms();
        if (wait > 0) plat_sleep_ms((int)wait);
        g_last_call_ms = plat_now_ms();
        pthread_mutex_unlock(&g_rate_lock);
        body->len = 0;
        if (body->data) body->data[0] = 0;
        status = net_request(method, url, key, form, body, total, err, errlen);
        if (status != 429) break;
        plat_log("Real-Debrid rate limit (HTTP 429), waiting");
        plat_sleep_ms(2000 << attempt);
    }
    memset(stored, 0, sizeof stored);
    if (status >= 400) rd_error(body, status, code, err, errlen);
    return status;
}

int rd_check_key(const char *key, sbuf *user, char *err, size_t errlen) {
    sbuf b;
    sb_init(&b);
    long code = 0;
    int status = rd_call("GET", "user", key, NULL, &b, NULL, &code, err, errlen);
    int rc = -1;
    if (status == 200 && b.data) {
        char name[200] = "", type[32] = "", expires[64] = "";
        json_string(json_find(b.data, "username"), name, sizeof name);
        json_string(json_find(b.data, "type"), type, sizeof type);
        json_string(json_find(b.data, "expiration"), expires, sizeof expires);
        sb_puts(user, "{\"name\":");
        sb_json_str(user, name);
        sb_puts(user, ",\"type\":");
        sb_json_str(user, type);
        sb_puts(user, ",\"expires\":");
        sb_json_str(user, expires);
        sb_printf(user, ",\"premium_seconds\":%lld}", json_int(json_find(b.data, "premium"), 0));
        rc = name[0] ? 1 : -1;
        if (rc < 0) snprintf(err, errlen, "unexpected answer from Real-Debrid");
    } else if (status == 401 || status == 403) rc = 0;
    sb_free(&b);
    return rc;
}

int rd_list(const char *what, int page, int limit, sbuf *out, char *err, size_t errlen) {
    char path[128];
    snprintf(path, sizeof path, "%s?page=%d&limit=%d", what, page < 1 ? 1 : page, limit < 1 ? 100 : limit);
    sbuf b;
    sb_init(&b);
    long long total = -1;
    int status = rd_call("GET", path, NULL, NULL, &b, &total, NULL, err, errlen);
    if (status == 200 || status == 204) {
        int empty = !b.data || !*b.data;
        sb_printf(out, "{\"ok\":true,\"page\":%d,\"total\":%lld,\"data\":%s}", page, total, empty ? "[]" : b.data);
    }
    sb_free(&b);
    return status;
}

// Torrent details with each selected file paired with its link. Real-Debrid lists one link per selected file, in
// file order; when the counts differ (files packed into an archive) the links are returned on their own.
int rd_info(const char *id, sbuf *out, char *err, size_t errlen) {
    for (const char *p = id; *p; p++)
        if (!isalnum((unsigned char)*p)) {
            snprintf(err, errlen, "bad torrent id");
            return 400;
        }
    char path[128];
    snprintf(path, sizeof path, "torrents/info/%s", id);
    sbuf b;
    sb_init(&b);
    int status = rd_call("GET", path, NULL, NULL, &b, NULL, NULL, err, errlen);
    if (status != 200 || !b.data) {
        sb_free(&b);
        return status;
    }
    const char *files = json_find(b.data, "files"), *links = json_find(b.data, "links");
    int nsel = 0, nlinks = 0;
    for (const char *f = json_array_first(files); f; f = json_array_next(f))
        nsel += json_int(json_find(f, "selected"), 0) == 1;
    for (const char *l = json_array_first(links); l; l = json_array_next(l)) nlinks++;
    int paired = nsel == nlinks;
    char *s = malloc(8192);
    if (!s) {
        sb_free(&b);
        return 500;
    }
    static const char *copy[] = {"id", "filename", "status", "hash"};
    sb_puts(out, "{\"ok\":true");
    for (unsigned i = 0; i < sizeof copy / sizeof *copy; i++) {
        s[0] = 0;
        json_string(json_find(b.data, copy[i]), s, 8192);
        sb_printf(out, ",\"%s\":", copy[i]);
        sb_json_str(out, s);
    }
    sb_printf(out, ",\"bytes\":%lld,\"progress\":%lld,\"paired\":%s,\"files\":[",
              json_int(json_find(b.data, "bytes"), 0), json_int(json_find(b.data, "progress"), 0),
              paired ? "true" : "false");
    const char *l = json_array_first(links);
    int first = 1;
    for (const char *f = json_array_first(files); f; f = json_array_next(f)) {
        if (json_int(json_find(f, "selected"), 0) != 1) continue;
        sb_puts(out, first ? "{\"id\":" : ",{\"id\":");
        first = 0;
        sb_printf(out, "%lld,\"bytes\":%lld,\"path\":", json_int(json_find(f, "id"), -1), json_int(json_find(f, "bytes"), 0));
        s[0] = 0;
        json_string(json_find(f, "path"), s, 8192);
        sb_json_str(out, s);
        sb_puts(out, ",\"link\":");
        if (paired && l) {
            s[0] = 0;
            json_string(l, s, 8192);
            sb_json_str(out, s);
            l = json_array_next(l);
        } else sb_puts(out, "null");
        sb_puts(out, "}");
    }
    sb_puts(out, "],\"links\":[");
    first = 1;
    for (l = paired ? NULL : json_array_first(links); l; l = json_array_next(l)) {
        s[0] = 0;
        json_string(l, s, 8192);
        if (!first) sb_puts(out, ",");
        first = 0;
        sb_json_str(out, s);
    }
    sb_puts(out, "]}");
    free(s);
    sb_free(&b);
    return 200;
}

int rd_unrestrict(const char *link, char *url, size_t urllen, char *name, size_t namelen, int *chunks, int *permanent,
                  char *err, size_t errlen) {
    *permanent = 0;
    *chunks = 0;
    size_t n = strlen(link) * 3 + 8;
    char *form = malloc(n);
    if (!form) return -1;
    memcpy(form, "link=", 5);
    url_encode(link, form + 5, n - 5);
    sbuf b;
    sb_init(&b);
    long code = 0;
    int status = rd_call("POST", "unrestrict/link", NULL, form, &b, NULL, &code, err, errlen);
    free(form);
    int rc = -1;
    if (status == 200 && b.data && !json_string(json_find(b.data, "download"), url, urllen) &&
        !strncmp(url, "http", 4)) {
        if (name) json_string(json_find(b.data, "filename"), name, namelen);
        *chunks = (int)json_int(json_find(b.data, "chunks"), 0);
        rc = 0;
    } else if (status == 200) snprintf(err, errlen, "Real-Debrid gave no download link");
    else if (!status && !strncmp(err, "not logged in", 13)) *permanent = 1;
    // Retrying cannot fix these: token, account, hoster support, missing file.
    if (rc && (code == 8 || code == 9 || code == 14 || code == 15 || code == 16 || code == 20 || code == 22 ||
               code == 23 || code == 24 || code == 28 || code == 35 || code == 36 || status == 401))
        *permanent = 1;
    sb_free(&b);
    return rc;
}
