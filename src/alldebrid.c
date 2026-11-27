// AllDebrid API client (https://api.alldebrid.com, /v4 and /v4.1), with the API key as Bearer.
//
// AllDebrid allows 12 requests a second and 600 a minute: calls are spaced and HTTP 429/503 are waited out.
// Answers are {"status":"success","data":{...}} or {"status":"error","error":{"code":"...","message":"..."}};
// error codes are turned into readable text. Magnet files and saved links are "unlocked" into direct links when a
// download starts (/v4/link/unlock, waiting for "delayed" links); direct links are never stored or logged.
#include "app.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AD_MIN_GAP_MS 110 // at most ~9 requests a second (limits: 12/s and 600/min)

static char g_base[256];
static int g_delay_poll_ms = 5000; // how often a "delayed" link is asked for (AllDebrid: at most every 5 s)
static pthread_mutex_t g_rate_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_last_call_ms;

void ad_init(void) {
    const char *base = getenv("AD_API_BASE"); // host tests point this at tests/mock_torbox.py
    snprintf(g_base, sizeof g_base, "%s", base ? base : "https://api.alldebrid.com");
#ifndef __PROSPERO__
    const char *poll = getenv("AD_DELAYED_POLL_MS");
    if (poll && atoi(poll) > 0) g_delay_poll_ms = atoi(poll);
#endif
}

static const char *code_text(const char *code) {
    static const char *const table[][2] = {
        {"AUTH_MISSING_APIKEY", "no API key"},
        {"AUTH_BAD_APIKEY", "bad API key: log in again"},
        {"AUTH_BLOCKED", "login blocked from a new location: confirm it in the e-mail AllDebrid sent you"},
        {"AUTH_USER_BANNED", "account banned"},
        {"MUST_BE_PREMIUM", "a premium account is needed"},
        {"MAGNET_MUST_BE_PREMIUM", "a premium account is needed"},
        {"FREE_TRIAL_LIMIT_REACHED", "free trial limit reached"},
        {"LINK_IS_MISSING", "no link given"},
        {"BAD_LINK", "bad link"},
        {"LINK_HOST_NOT_SUPPORTED", "unsupported host"},
        {"LINK_NOT_SUPPORTED", "unsupported link"},
        {"LINK_DOWN", "file unavailable on the host"},
        {"LINK_PASS_PROTECTED", "the link is password protected"},
        {"LINK_HOST_UNAVAILABLE", "host temporarily unavailable"},
        {"LINK_TOO_MANY_DOWNLOADS", "too many downloads on this host"},
        {"LINK_HOST_FULL", "host servers are full"},
        {"LINK_HOST_LIMIT_REACHED", "host download limit reached"},
        {"LINK_TEMPORARY_UNAVAILABLE", "link temporarily unavailable"},
        {"LINK_ERROR", "could not unlock the link"},
        {"NO_SERVER", "AllDebrid refuses servers and VPNs for this host"},
        {"MAGNET_INVALID_ID", "unknown magnet"},
        {"MAGNET_PROCESSING", "the magnet is still processing"},
        {"MAGNET_LINKS_REMOVED", "the magnet's files were removed"},
        {"DELAYED_INVALID_ID", "the delayed link expired"},
        {"MAINTENANCE", "AllDebrid is in maintenance"},
    };
    for (unsigned i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(code, table[i][0])) return table[i][1];
    return NULL;
}

static void ad_error(const sbuf *b, int status, char *code, size_t codelen, char *err, size_t errlen) {
    char msg[200] = "";
    code[0] = 0;
    const char *e = b->data ? json_find(b->data, "error") : NULL;
    if (e) {
        json_string(json_find(e, "code"), code, codelen);
        json_string(json_find(e, "message"), msg, sizeof msg);
    }
    const char *text = code[0] ? code_text(code) : NULL;
    if (text) snprintf(err, errlen, "AllDebrid: %s (%s)", text, code);
    else if (msg[0]) snprintf(err, errlen, "AllDebrid: %s%s%s%s", msg, code[0] ? " (" : "", code, code[0] ? ")" : "");
    else if (status) snprintf(err, errlen, "AllDebrid answered HTTP %d", status);
}

// One API call, spaced to stay under the rate limit, with HTTP 429/503 retried. key NULL = the stored key.
// Returns a pointer to "data" inside body on success; NULL with err (and code: AllDebrid's error code) set otherwise.
static const char *ad_call(const char *method, const char *path, const char *key, const char *form, sbuf *body,
                           char *code, size_t codelen, char *err, size_t errlen) {
    char stored[256];
    code[0] = 0;
    if (!key) {
        if (cred_get(PROV_AD, stored, sizeof stored)) {
            snprintf(err, errlen, "not logged in to AllDebrid");
            return NULL;
        }
        key = stored;
    }
    char url[512];
    snprintf(url, sizeof url, "%s%s", g_base, path);
    int status = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        pthread_mutex_lock(&g_rate_lock);
        int64_t wait = g_last_call_ms + AD_MIN_GAP_MS - plat_now_ms();
        if (wait > 0) plat_sleep_ms((int)wait);
        g_last_call_ms = plat_now_ms();
        pthread_mutex_unlock(&g_rate_lock);
        body->len = 0;
        if (body->data) body->data[0] = 0;
        status = net_request(method, url, key, form, body, NULL, err, errlen);
        if (status != 429 && status != 503) break;
        plat_log("AllDebrid rate limit (HTTP %d), waiting", status);
        plat_sleep_ms(1000 << attempt);
    }
    memset(stored, 0, sizeof stored);
    if (!status) return NULL; // transport error, err set
    char st[16] = "";
    if (body->data) json_string(json_find(body->data, "status"), st, sizeof st);
    const char *data = body->data ? json_find(body->data, "data") : NULL;
    if (status == 200 && !strcmp(st, "success") && data) return data;
    ad_error(body, status, code, codelen, err, errlen);
    if (!err[0]) snprintf(err, errlen, "unexpected answer from AllDebrid");
    return NULL;
}

static int is_auth_refusal(const char *code) {
    return !strcmp(code, "AUTH_BAD_APIKEY") || !strcmp(code, "AUTH_MISSING_APIKEY") || !strcmp(code, "AUTH_USER_BANNED");
}

int ad_check_key(const char *key, sbuf *user, char *err, size_t errlen) {
    sbuf b;
    sb_init(&b);
    char code[64];
    const char *data = ad_call("GET", "/v4/user", key, NULL, &b, code, sizeof code, err, errlen);
    const char *u = data ? json_find(data, "user") : NULL;
    int rc = -1;
    char name[200] = "";
    if (u && !json_string(json_find(u, "username"), name, sizeof name) && name[0]) {
        const char *pu = json_find(u, "premiumUntil"); // documented as an integer, shown as a string in examples
        long long until = pu && *pu == '"' ? strtoll(pu + 1, NULL, 10) : json_int(pu, 0);
        sb_puts(user, "{\"name\":");
        sb_json_str(user, name);
        sb_printf(user, ",\"premium\":%s,\"premium_until\":%lld,\"trial\":%s}",
                  json_bool(json_find(u, "isPremium")) == 1 ? "true" : "false", until,
                  json_bool(json_find(u, "isTrial")) == 1 ? "true" : "false");
        rc = 1;
    } else if (data) snprintf(err, errlen, "unexpected answer from AllDebrid");
    else if (is_auth_refusal(code)) rc = 0;
    sb_free(&b);
    return rc;
}

// Copies the JSON value at val (an array, or an object of entries) into out as {"ok":true,"<field>":...}.
static void put_value(sbuf *out, const char *field, const char *val) {
    const char *end = json_end(val);
    sb_printf(out, "{\"ok\":true,\"%s\":", field);
    if (val && end && end > val) sb_add(out, val, (size_t)(end - val));
    else sb_puts(out, "[]");
    sb_puts(out, "}");
}

int ad_list(const char *what, sbuf *out, char *err, size_t errlen) {
    const char *method, *path, *field;
    if (!strcmp(what, "magnets")) method = "POST", path = "/v4.1/magnet/status", field = "magnets";
    else if (!strcmp(what, "links")) method = "GET", path = "/v4/user/links", field = "links";
    else if (!strcmp(what, "history")) method = "GET", path = "/v4/user/history", field = "links";
    else {
        snprintf(err, errlen, "unknown list");
        return 404;
    }
    sbuf b;
    sb_init(&b);
    char code[64];
    const char *data = ad_call(method, path, NULL, !strcmp(method, "POST") ? "" : NULL, &b, code, sizeof code, err,
                               errlen);
    int status = 502;
    if (data) {
        put_value(out, "data", json_find(data, field));
        status = 200;
    } else if (is_auth_refusal(code) || !strncmp(err, "not logged in", 13)) status = 401;
    sb_free(&b);
    return status;
}

int ad_files(const char *id, sbuf *out, char *err, size_t errlen) {
    if (!id[0] || strlen(id) > 20) {
        snprintf(err, errlen, "bad magnet id");
        return 400;
    }
    for (const char *p = id; *p; p++)
        if (!isdigit((unsigned char)*p)) {
            snprintf(err, errlen, "bad magnet id");
            return 400;
        }
    char form[64];
    snprintf(form, sizeof form, "id%%5B%%5D=%s", id);
    sbuf b;
    sb_init(&b);
    char code[64];
    const char *data = ad_call("POST", "/v4/magnet/files", NULL, form, &b, code, sizeof code, err, errlen);
    int status = 502;
    const char *m = data ? json_array_first(json_find(data, "magnets")) : NULL;
    if (m && json_find(m, "files")) {
        put_value(out, "files", json_find(m, "files"));
        status = 200;
    } else if (m && json_find(m, "error")) {
        sbuf e = {(char *)m, strlen(m), 0}; // ad_error reads {"error":{...}} from the magnet entry
        ad_error(&e, 0, code, sizeof code, err, errlen);
        status = 404;
    } else if (data) snprintf(err, errlen, "AllDebrid listed no files for this magnet");
    else if (is_auth_refusal(code) || !strncmp(err, "not logged in", 13)) status = 401;
    sb_free(&b);
    return status;
}

static int permanent_code(const char *code) {
    static const char *const codes[] = {
        "AUTH_MISSING_APIKEY", "AUTH_BAD_APIKEY", "AUTH_BLOCKED", "AUTH_USER_BANNED", "MUST_BE_PREMIUM",
        "FREE_TRIAL_LIMIT_REACHED", "LINK_IS_MISSING", "BAD_LINK", "LINK_HOST_NOT_SUPPORTED", "LINK_NOT_SUPPORTED",
        "LINK_DOWN", "LINK_PASS_PROTECTED", "NO_SERVER", "MAGNET_LINKS_REMOVED",
    };
    for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++)
        if (!strcmp(code, codes[i])) return 1;
    return 0;
}

int ad_unlock(const char *link, char *url, size_t urllen, char *name, size_t namelen, int *permanent, char *err,
              size_t errlen) {
    *permanent = 0;
    size_t n = strlen(link) * 3 + 8;
    char *form = malloc(n);
    if (!form) return -1;
    memcpy(form, "link=", 5);
    url_encode(link, form + 5, n - 5);
    sbuf b;
    sb_init(&b);
    char code[64];
    const char *data = ad_call("POST", "/v4/link/unlock", NULL, form, &b, code, sizeof code, err, errlen);
    free(form);
    int rc = -1;
    if (data) {
        if (name) json_string(json_find(data, "filename"), name, namelen);
        long long delayed = json_int(json_find(data, "delayed"), 0);
        url[0] = 0;
        json_string(json_find(data, "link"), url, urllen);
        if (delayed > 0) {
            // The host needs time to prepare the file: poll /link/delayed every 5 s, 24 times (2 minutes).
            plat_log("AllDebrid: link is being prepared, waiting");
            url[0] = 0;
            char df[48];
            snprintf(df, sizeof df, "id=%lld", delayed);
            for (int i = 0; i < 24 && !url[0]; i++) {
                plat_sleep_ms(g_delay_poll_ms);
                data = ad_call("POST", "/v4/link/delayed", NULL, df, &b, code, sizeof code, err, errlen);
                if (!data) break;
                long long st = json_int(json_find(data, "status"), 0);
                if (st == 2) json_string(json_find(data, "link"), url, urllen);
                else if (st == 3) {
                    snprintf(err, errlen, "AllDebrid could not prepare the link");
                    break;
                }
            }
            if (!url[0] && !err[0]) snprintf(err, errlen, "AllDebrid took too long to prepare the link");
        }
        if (!strncmp(url, "http", 4)) rc = 0;
        else if (!err[0]) snprintf(err, errlen, "AllDebrid gave no download link");
    } else if (!strncmp(err, "not logged in", 13)) *permanent = 1;
    if (rc && permanent_code(code)) *permanent = 1;
    if (rc) url[0] = 0;
    sb_free(&b);
    return rc;
}
