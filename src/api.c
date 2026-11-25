// Routes of the web UI's JSON API and the static files.
#include "app.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static pthread_mutex_t g_user_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_user_json[PROV_COUNT][1024]; // cached account summary per provider, "" = unknown

static void json_err(int fd, int status, const char *msg) {
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"ok\":false,\"error\":");
    sb_json_str(&b, msg);
    sb_puts(&b, "}");
    http_send_json(fd, status, &b);
    sb_free(&b);
}

static void json_ok(int fd) {
    sbuf b = {(char *)"{\"ok\":true}", 11, 0};
    http_send_json(fd, 200, &b);
}

// Validates key with the provider; on success caches the account summary. 1 valid, 0 refused, -1 could not ask.
static int check_key(int p, const char *key, char *err, size_t errlen) {
    sbuf u;
    sb_init(&u);
    int rc = p == PROV_RD ? rd_check_key(key, &u, err, errlen) : tb_check_key(key, &u, err, errlen);
    if (rc == 1) {
        pthread_mutex_lock(&g_user_lock);
        snprintf(g_user_json[p], sizeof g_user_json[p], "%s", u.data ? u.data : "{}");
        pthread_mutex_unlock(&g_user_lock);
    } else if (!err[0]) snprintf(err, errlen, "%s refused the key", providers[p].title);
    sb_free(&u);
    return rc;
}

// The provider a request names (?provider= or form field), TorBox when none is given.
static int req_provider(http_req *r) {
    char id[32];
    if (!form_get(r->query, "provider", id, sizeof id) && !(r->body && form_get(r->body, "provider", id, sizeof id)))
        return PROV_TORBOX;
    return prov_find(id);
}

static void route_status(http_req *r) {
    char key[256], ip[64], err[PROV_COUNT][300];
    int logged[PROV_COUNT];
    for (int p = 0; p < PROV_COUNT; p++) {
        err[p][0] = 0;
        logged[p] = cred_get(p, key, sizeof key) == 0;
        pthread_mutex_lock(&g_user_lock);
        int known = g_user_json[p][0] != 0;
        pthread_mutex_unlock(&g_user_lock);
        if (logged[p] && !known) check_key(p, key, err[p], sizeof err[p]);
        memset(key, 0, sizeof key);
    }
    plat_local_ip(ip, sizeof ip);
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"version\":\"%s\",\"mode\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"default_folder\":",
              APP_VERSION, plat_mode(), ip, APP_PORT);
    sb_json_str(&b, plat_default_folder());
    sb_puts(&b, ",\"providers\":{");
    for (int p = 0; p < PROV_COUNT; p++) {
        sb_printf(&b, "%s\"%s\":{\"title\":\"%s\",\"logged_in\":%s,\"key_file\":", p ? "," : "", providers[p].id,
                  providers[p].title, logged[p] ? "true" : "false");
        sb_json_str(&b, cred_path(p));
        pthread_mutex_lock(&g_user_lock);
        sb_printf(&b, ",\"user\":%s", logged[p] && g_user_json[p][0] ? g_user_json[p] : "null");
        pthread_mutex_unlock(&g_user_lock);
        sb_puts(&b, ",\"user_error\":");
        sb_json_str(&b, err[p]);
        sb_puts(&b, "}");
    }
    sb_puts(&b, "}}");
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_login(http_req *r) {
    int p = req_provider(r);
    if (p < 0) {
        json_err(r->fd, 400, "unknown provider");
        return;
    }
    char key[256], err[300] = "";
    form_get(r->body, "key", key, sizeof key);
    // Tokens are plain ASCII; trim spaces a paste may bring.
    char *k = key;
    while (*k == ' ' || *k == '\t') k++;
    size_t n = strlen(k);
    while (n && (k[n - 1] == ' ' || k[n - 1] == '\t' || k[n - 1] == '\r' || k[n - 1] == '\n')) k[--n] = 0;
    for (char *c = k; *c; c++)
        if (*c < 0x21 || *c > 0x7e) {
            memset(key, 0, sizeof key);
            json_err(r->fd, 400, "the key has unexpected characters");
            return;
        }
    if (n < 8) {
        char msg[100];
        snprintf(msg, sizeof msg, "paste your %s API %s", providers[p].title, p == PROV_RD ? "token" : "key");
        json_err(r->fd, 400, msg);
        return;
    }
    int rc = check_key(p, k, err, sizeof err);
    if (rc == 1) {
        if (cred_set(p, k)) {
            memset(key, 0, sizeof key);
            json_err(r->fd, 500, "could not save the key on the console");
            return;
        }
        plat_log("login: %s key accepted and stored", providers[p].title);
        memset(key, 0, sizeof key);
        route_status(r);
        return;
    }
    plat_log("login: %s key not accepted (%s)", providers[p].title, err);
    json_err(r->fd, rc == 0 ? 401 : 502, err);
    memset(key, 0, sizeof key);
}

static void route_logout(http_req *r) {
    int p = req_provider(r);
    if (p < 0) {
        json_err(r->fd, 400, "unknown provider");
        return;
    }
    cred_clear(p);
    pthread_mutex_lock(&g_user_lock);
    g_user_json[p][0] = 0;
    pthread_mutex_unlock(&g_user_lock);
    plat_log("logout: %s key removed", providers[p].title);
    json_ok(r->fd);
}

static void route_list(http_req *r) {
    char kind[16], fresh[8], key[256], err[300] = "";
    form_get(r->query, "kind", kind, sizeof kind);
    form_get(r->query, "fresh", fresh, sizeof fresh);
    if (strcmp(kind, "torrents") && strcmp(kind, "webdl") && strcmp(kind, "usenet")) {
        json_err(r->fd, 400, "kind must be torrents, webdl or usenet");
        return;
    }
    if (cred_get(PROV_TORBOX, key, sizeof key)) {
        json_err(r->fd, 401, "not logged in");
        return;
    }
    char path[128];
    snprintf(path, sizeof path, "%s/mylist?limit=1000%s", kind, fresh[0] == '1' ? "&bypass_cache=true" : "");
    sbuf b;
    sb_init(&b);
    int status = tb_get(path, key, &b, err, sizeof err);
    memset(key, 0, sizeof key);
    if (!status) json_err(r->fd, 502, err);
    else http_send(r->fd, status, "application/json; charset=utf-8", b.data ? b.data : "{}", b.data ? b.len : 2, NULL);
    sb_free(&b);
}

// Real-Debrid: /api/rd/torrents?page=&limit=, /api/rd/downloads?page=&limit=, /api/rd/info?id=
static void route_rd(http_req *r, const char *what) {
    char page[16], limit[16], id[64], err[300] = "", tok[256];
    int logged = cred_get(PROV_RD, tok, sizeof tok) == 0;
    memset(tok, 0, sizeof tok);
    if (!logged) {
        json_err(r->fd, 401, "not logged in");
        return;
    }
    sbuf b;
    sb_init(&b);
    int status;
    if (!strcmp(what, "info")) {
        form_get(r->query, "id", id, sizeof id);
        status = rd_info(id, &b, err, sizeof err);
    } else if (!strcmp(what, "torrents") || !strcmp(what, "downloads")) {
        form_get(r->query, "page", page, sizeof page);
        form_get(r->query, "limit", limit, sizeof limit);
        int lim = atoi(limit);
        status = rd_list(what, atoi(page), lim > 0 && lim <= 1000 ? lim : 100, &b, err, sizeof err);
    } else {
        sb_free(&b);
        json_err(r->fd, 404, "unknown API call");
        return;
    }
    if ((status == 200 || status == 204) && b.data) http_send_json(r->fd, 200, &b);
    else json_err(r->fd, status == 401 ? 401 : status >= 400 && status < 500 ? status : 502, err[0] ? err : "no answer");
    sb_free(&b);
}

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void add_roots(sbuf *b) {
    sb_puts(b, "\"roots\":[");
    int first = 1;
#ifdef __PROSPERO__
    sb_puts(b, "\"/data\"");
    first = 0;
    DIR *d = opendir("/mnt");
    struct stat mnt;
    if (d && stat("/mnt", &mnt)) closedir(d), d = NULL;
    if (d) {
        struct dirent *e;
        char *names[64];
        int n = 0;
        while ((e = readdir(d)) && n < 64) {
            char p[300];
            snprintf(p, sizeof p, "/mnt/%s", e->d_name);
            struct stat st; // an empty mount point lives on /mnt's own device: no drive there
            if (plat_path_allowed(p) && !stat(p, &st) && S_ISDIR(st.st_mode) && st.st_dev != mnt.st_dev)
                names[n++] = strdup(p);
        }
        closedir(d);
        qsort(names, n, sizeof *names, cmp_str);
        for (int i = 0; i < n; i++) {
            sb_puts(b, ",");
            sb_json_str(b, names[i]);
            free(names[i]);
        }
    }
#else
    const char *roots = getenv("TORBOX_ROOTS");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", roots ? roots : "/tmp");
    for (char *save, *t = strtok_r(buf, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
        if (!first) sb_puts(b, ",");
        sb_json_str(b, t);
        first = 0;
    }
#endif
    (void)first;
    sb_puts(b, "]");
}

static void route_dirs(http_req *r) {
    char path[1024];
    form_get(r->query, "path", path, sizeof path);
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    sbuf b;
    sb_init(&b);
    sb_puts(&b, "{\"ok\":true,");
    add_roots(&b);
    if (path[0]) {
        if (!plat_path_allowed(path)) {
            sb_free(&b);
            json_err(r->fd, 403, "this folder is outside /data and the USB drives");
            return;
        }
        DIR *d = opendir(path);
        if (!d) {
            char msg[1200];
            snprintf(msg, sizeof msg, "cannot open %s: %s", path, strerror(errno));
            sb_free(&b);
            json_err(r->fd, 404, msg);
            return;
        }
        char **names = NULL;
        int count = 0, cap = 0, nfiles = 0;
        sbuf files;
        sb_init(&files);
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char full[1300];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            int is_dir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
                struct stat st;
                is_dir = !stat(full, &st) && S_ISDIR(st.st_mode);
            }
            if (!is_dir) {
                struct stat st;
                if (nfiles < 2000 && !stat(full, &st) && S_ISREG(st.st_mode)) {
                    sb_puts(&files, nfiles++ ? ",{\"name\":" : "{\"name\":");
                    sb_json_str(&files, e->d_name);
                    sb_printf(&files, ",\"size\":%lld}", (long long)st.st_size);
                }
                continue;
            }
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                char **nn = realloc(names, cap * sizeof *names);
                if (!nn) break;
                names = nn;
            }
            names[count++] = strdup(e->d_name);
            if (count >= 5000) break;
        }
        closedir(d);
        qsort(names, count, sizeof *names, cmp_str);
        sb_puts(&b, ",\"path\":");
        sb_json_str(&b, path);
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) *slash = 0;
        else snprintf(parent, sizeof parent, "/");
        sb_puts(&b, ",\"parent\":");
        if (plat_path_allowed(parent) && strcmp(parent, path)) sb_json_str(&b, parent);
        else sb_puts(&b, "null");
        struct statvfs vs;
        if (!statvfs(path, &vs)) sb_printf(&b, ",\"free\":%llu", (unsigned long long)vs.f_bavail * vs.f_frsize);
        sb_puts(&b, ",\"dirs\":[");
        for (int i = 0; i < count; i++) {
            if (i) sb_puts(&b, ",");
            sb_json_str(&b, names[i]);
            free(names[i]);
        }
        free(names);
        sb_printf(&b, "],\"files\":[%s]", files.data ? files.data : "");
        sb_free(&files);
    }
    sb_puts(&b, "}");
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_mkdir(http_req *r) {
    char path[1024];
    form_get(r->body, "path", path, sizeof path);
    if (!plat_path_allowed(path)) {
        json_err(r->fd, 403, "folders can be made under /data or a USB drive only");
        return;
    }
    if (mkdir_p(path)) {
        char msg[1200];
        snprintf(msg, sizeof msg, "cannot create %s: %s", path, strerror(errno));
        json_err(r->fd, 500, msg);
        return;
    }
    json_ok(r->fd);
}

static void route_download(http_req *r) {
    char kind[16], id[32], file_id[32], folder[1024], subdir[512], name[256], err[300] = "";
    char *url = malloc(8192);
    if (!url) return;
    form_get(r->body, "kind", kind, sizeof kind);
    form_get(r->body, "id", id, sizeof id);
    form_get(r->body, "file_id", file_id, sizeof file_id);
    form_get(r->body, "folder", folder, sizeof folder);
    form_get(r->body, "subdir", subdir, sizeof subdir);
    form_get(r->body, "name", name, sizeof name);
    form_get(r->body, "url", url, 8192);
    size_t n = strlen(folder);
    while (n > 1 && folder[n - 1] == '/') folder[--n] = 0;
    long long fid = file_id[0] ? atoll(file_id) : -1;
    int jid = dl_add(kind, atoll(id), fid, url, folder, subdir, name, err, sizeof err);
    free(url);
    if (jid < 0) {
        json_err(r->fd, 400, err);
        return;
    }
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"id\":%d}", jid);
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_jobs(http_req *r) {
    sbuf b;
    sb_init(&b);
    dl_jobs_json(&b);
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

static void route_job_action(http_req *r, const char *action) {
    char id[32];
    form_get(r->body, "id", id, sizeof id);
    int jid = atoi(id), rc = 0;
    if (!strcmp(action, "cancel")) rc = dl_cancel(jid);
    else if (!strcmp(action, "pause")) rc = dl_pause(jid);
    else if (!strcmp(action, "resume")) rc = dl_retry(jid);
    else if (!strcmp(action, "remove")) rc = dl_remove(jid);
    else if (!strcmp(action, "clear")) dl_clear_finished();
    else {
        json_err(r->fd, 404, "unknown action");
        return;
    }
    if (rc) json_err(r->fd, 409, "not possible in the job's current state");
    else json_ok(r->fd);
}

static void route_settings(http_req *r) {
    char par[8], con[8];
    form_get(r->body, "parallel", par, sizeof par);
    form_get(r->body, "connections", con, sizeof con);
    dl_settings(par[0] ? atoi(par) : 0, con[0] ? atoi(con) : 0);
    route_jobs(r);
}

// SHA-256 of a downloaded file, to check it against a known checksum.
static void route_sha256(http_req *r) {
    char path[1024], hex[65];
    form_get(r->query, "path", path, sizeof path);
    if (!plat_path_allowed(path)) {
        json_err(r->fd, 403, "this file is outside /data and the USB drives");
        return;
    }
    long long size = 0;
    if (sha256_file(path, hex, &size)) {
        json_err(r->fd, 404, strerror(errno));
        return;
    }
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "{\"ok\":true,\"size\":%lld,\"sha256\":\"%s\"}", size, hex);
    http_send_json(r->fd, 200, &b);
    sb_free(&b);
}

// LAN speed test: streams N MiB of zeros, so a browser on the network can see what the console's link carries.
static void route_speedtest(http_req *r) {
    char mb[16];
    form_get(r->query, "mb", mb, sizeof mb);
    long n = atol(mb);
    if (n <= 0 || n > 4096) n = 200;
    static const char zeros[256 * 1024];
    char head[256];
    int len = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                                          "Content-Length: %ld\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                       n << 20);
    if (http_send_raw(r->fd, head, len)) return;
    for (long i = 0; i < n * 4; i++)
        if (http_send_raw(r->fd, zeros, sizeof zeros)) return;
}

static void route_static(http_req *r) {
    const char *name = r->path[1] ? r->path + 1 : "index.html";
    const asset *a = asset_find(name);
    if (!a) {
        http_send(r->fd, 404, "text/plain", "not found", 9, NULL);
        return;
    }
    http_send(r->fd, 200, a->ctype, a->data, a->len, NULL);
}

void api_handle(http_req *r) {
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    if (strncmp(r->path, "/api/", 5)) {
        if (get) route_static(r);
        else http_send(r->fd, 405, "text/plain", "method not allowed", 18, NULL);
        return;
    }
    const char *p = r->path + 5;
    if (post && !r->has_app_header) { // a page on another site cannot send this header without CORS
        json_err(r->fd, 403, "missing X-TorBox-PS5 header");
        return;
    }
    if (get && !strcmp(p, "status")) route_status(r);
    else if (get && !strcmp(p, "list")) route_list(r);
    else if (get && !strncmp(p, "rd/", 3)) route_rd(r, p + 3);
    else if (get && !strcmp(p, "dirs")) route_dirs(r);
    else if (get && !strcmp(p, "jobs")) route_jobs(r);
    else if (get && !strcmp(p, "sha256")) route_sha256(r);
    else if (get && !strcmp(p, "speedtest")) route_speedtest(r);
    else if (post && !strcmp(p, "login")) route_login(r);
    else if (post && !strcmp(p, "logout")) route_logout(r);
    else if (post && !strcmp(p, "mkdir")) route_mkdir(r);
    else if (post && !strcmp(p, "download")) route_download(r);
    else if (post && !strcmp(p, "settings")) route_settings(r);
    else if (post && !strncmp(p, "jobs/", 5)) route_job_action(r, p + 5);
    else if (post && !strcmp(p, "quit")) {
        json_ok(r->fd);
        plat_log("quit requested from the web UI");
        httpd_stop();
    } else json_err(r->fd, 404, "unknown API call");
}
