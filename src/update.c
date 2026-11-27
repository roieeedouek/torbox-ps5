// Update check: asks GitHub for the newest release of APP_REPO (no key, no account data sent) and compares its tag
// with APP_VERSION. The answer is kept for 6 hours (GitHub allows 60 unauthenticated calls an hour per address).
#include "app.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_CACHE_MS (6ll * 3600 * 1000)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_api[512], g_latest[64], g_url[512], g_name[200], g_published[32], g_err[200];
static int64_t g_checked_ms;
static int g_checking;

void update_init(void) {
    const char *u = getenv("TORBOX_UPDATE_URL"); // host tests point this at tests/mock_torbox.py
    snprintf(g_api, sizeof g_api, "%s", u ? u : "https://api.github.com/repos/" APP_REPO "/releases/latest");
}

// Compares "v1.2.3" style versions: <0, 0, >0. Missing parts count as 0; anything after the numbers is ignored.
static int version_cmp(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    for (int i = 0; i < 4; i++) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (ea == a) x = 0;
        if (eb == b) y = 0;
        if (x != y) return x < y ? -1 : 1;
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
    }
    return 0;
}

static void check_now(void) {
    sbuf b;
    sb_init(&b);
    char err[200] = "", tag[64] = "", url[512] = "", name[200] = "", published[32] = "";
    int status = net_request("GET", g_api, NULL, NULL, &b, NULL, err, sizeof err);
    if (status == 200 && b.data && !json_string(json_find(b.data, "tag_name"), tag, sizeof tag) && tag[0]) {
        json_string(json_find(b.data, "html_url"), url, sizeof url);
        json_string(json_find(b.data, "name"), name, sizeof name);
        json_string(json_find(b.data, "published_at"), published, sizeof published);
        err[0] = 0;
    } else if (status == 404) snprintf(err, sizeof err, "no release published yet");
    else if (status == 403 || status == 429) snprintf(err, sizeof err, "GitHub refused for now (rate limit), try later");
    else if (status) snprintf(err, sizeof err, "GitHub answered HTTP %d", status);
    else if (!err[0]) snprintf(err, sizeof err, "could not reach GitHub");
    sb_free(&b);
    pthread_mutex_lock(&g_lock);
    if (tag[0]) {
        snprintf(g_latest, sizeof g_latest, "%s", tag);
        snprintf(g_url, sizeof g_url, "%s", url);
        snprintf(g_name, sizeof g_name, "%s", name);
        snprintf(g_published, sizeof g_published, "%s", published);
    }
    snprintf(g_err, sizeof g_err, "%s", err);
    g_checked_ms = plat_now_ms();
    pthread_mutex_unlock(&g_lock);
    if (err[0]) plat_log("update check: %s", err);
    else plat_log("update check: latest release %s (running %s)", tag, APP_VERSION);
}

void update_json(sbuf *out, int fresh) {
    pthread_mutex_lock(&g_lock);
    int stale = !g_checked_ms || fresh || plat_now_ms() - g_checked_ms >= UPDATE_CACHE_MS;
    int run = stale && !g_checking;
    if (run) g_checking = 1;
    pthread_mutex_unlock(&g_lock);
    if (run) {
        check_now();
        pthread_mutex_lock(&g_lock);
        g_checking = 0;
        pthread_mutex_unlock(&g_lock);
    }
    pthread_mutex_lock(&g_lock);
    int newer = g_latest[0] && version_cmp(g_latest, APP_VERSION) > 0;
    sb_printf(out, "{\"ok\":true,\"current\":\"%s\",\"latest\":", APP_VERSION);
    sb_json_str(out, g_latest);
    sb_printf(out, ",\"newer\":%s,\"url\":", newer ? "true" : "false");
    sb_json_str(out, g_url);
    sb_puts(out, ",\"name\":");
    sb_json_str(out, g_name);
    sb_puts(out, ",\"published\":");
    sb_json_str(out, g_published);
    sb_puts(out, ",\"error\":");
    sb_json_str(out, g_err);
    sb_printf(out, ",\"checked_ago_s\":%lld}", g_checked_ms ? (long long)((plat_now_ms() - g_checked_ms) / 1000) : -1);
    pthread_mutex_unlock(&g_lock);
}
