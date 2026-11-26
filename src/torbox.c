// TorBox REST client (https://api.torbox.app/v1/api).
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_api_base[256];

void tb_init(void) {
    const char *base = getenv("TORBOX_API_BASE"); // host tests point this at tests/mock_torbox.py
    snprintf(g_api_base, sizeof g_api_base, "%s", base ? base : "https://api.torbox.app/v1/api");
}

int tb_get(const char *path_and_query, const char *key, sbuf *body, char *err, size_t errlen) {
    char url[2048];
    snprintf(url, sizeof url, "%s/%s", g_api_base, path_and_query);
    int status = net_request("GET", url, key, NULL, body, NULL, err, errlen);
    memset(url, 0, sizeof url); // may hold the key (requestdl)
    return status;
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

int tb_check_key(const char *key, sbuf *user, char *err, size_t errlen) {
    sbuf b;
    sb_init(&b);
    int status = tb_get("user/me", key, &b, err, errlen);
    int rc = -1;
    if (status == 200 && b.data && json_bool(json_find(b.data, "success")) == 1) {
        const char *d = json_find(b.data, "data");
        char email[200] = "", expires[64] = "";
        json_string(json_find(d, "email"), email, sizeof email);
        json_string(json_find(d, "premium_expires_at"), expires, sizeof expires);
        sb_puts(user, "{\"name\":");
        sb_json_str(user, email);
        sb_printf(user, ",\"plan\":%lld,\"expires\":", json_int(json_find(d, "plan"), -1));
        sb_json_str(user, expires);
        sb_puts(user, "}");
        rc = 1;
    } else if (status) {
        api_error(&b, status, err, errlen);
        rc = status == 401 || status == 403 ? 0 : -1;
    }
    sb_free(&b);
    return rc;
}

int tb_request_link(const char *kind, long long item_id, long long file_id, char *url, size_t urllen, char *err,
                    size_t errlen) {
    char key[256];
    if (cred_get(PROV_TORBOX, key, sizeof key)) {
        snprintf(err, errlen, "not logged in to TorBox");
        return -1;
    }
    const char *idname = !strcmp(kind, "torrents") ? "torrent_id" : !strcmp(kind, "webdl") ? "web_id" : "usenet_id";
    char q[600], ekey[400];
    url_encode(key, ekey, sizeof ekey);
    if (file_id >= 0)
        snprintf(q, sizeof q,
                 "%s/requestdl?token=%s&%s=%lld&file_id=%lld&redirect=false&zip_link=false&append_name=false", kind,
                 ekey, idname, item_id, file_id);
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
