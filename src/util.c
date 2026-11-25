// Small helpers: growable buffer, JSON string output, paths, URL/form encoding.
#include "app.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void sb_init(sbuf *b) { b->data = NULL, b->len = b->cap = 0; }
void sb_free(sbuf *b) { free(b->data), sb_init(b); }

int sb_add(sbuf *b, const void *data, size_t len) {
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + len + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p, b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = 0;
    return 0;
}

int sb_puts(sbuf *b, const char *s) { return sb_add(b, s, strlen(s)); }

int sb_printf(sbuf *b, const char *fmt, ...) {
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof small) return sb_add(b, small, n);
    char *big = malloc(n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    int rc = sb_add(b, big, n);
    free(big);
    return rc;
}

void sb_json_str(sbuf *b, const char *s) {
    sb_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': sb_add(b, "\\\"", 2); break;
        case '\\': sb_add(b, "\\\\", 2); break;
        case '\n': sb_add(b, "\\n", 2); break;
        case '\r': sb_add(b, "\\r", 2); break;
        case '\t': sb_add(b, "\\t", 2); break;
        default:
            if (*p < 0x20) sb_printf(b, "\\u%04x", *p);
            else sb_add(b, p, 1);
        }
    }
    sb_add(b, "\"", 1);
}

int mkdir_p(const char *path) {
    char tmp[1024];
    if (strlen(path) >= sizeof tmp) return -1;
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0777) && errno != EEXIST) return -1;
    return 0;
}

void sanitize_name(char *s) {
    for (unsigned char *p = (unsigned char *)s; *p; p++)
        if (*p < 0x20 || strchr("/\\:*?\"<>|", *p)) *p = '_';
    // No leading dots (".", "..", hidden files) and no trailing dots or spaces (exFAT drops them).
    size_t n = strlen(s);
    while (n && (s[n - 1] == '.' || s[n - 1] == ' ')) s[--n] = 0;
    size_t lead = 0;
    while (s[lead] == '.' || s[lead] == ' ') lead++;
    if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
    if (strlen(s) > 240) s[240] = 0; // keep room for ".part"
}

int sanitize_relpath(const char *in, char *out, size_t len) {
    size_t used = 0;
    out[0] = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e > p) {
            char part[256];
            size_t n = (size_t)(e - p) < sizeof part - 1 ? (size_t)(e - p) : sizeof part - 1;
            memcpy(part, p, n);
            part[n] = 0;
            sanitize_name(part);
            if (part[0]) {
                size_t pl = strlen(part);
                if (used + pl + 2 > len) return -1;
                if (used) out[used++] = '/';
                memcpy(out + used, part, pl + 1);
                used += pl;
            }
        }
        p = e;
    }
    return 0;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') *o++ = ' ';
        else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *o++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else *o++ = *p;
    }
    *o = 0;
    return (int)(o - s);
}

const char *form_get(const char *form, const char *key, char *out, size_t len) {
    size_t kl = strlen(key);
    const char *p = form;
    out[0] = 0;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        if ((size_t)(end - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t n = end - (p + kl + 1);
            if (n >= len) n = len - 1;
            memcpy(out, p + kl + 1, n);
            out[n] = 0;
            url_decode(out);
            return out;
        }
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

void url_encode(const char *in, char *out, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < len; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p))
            out[o++] = *p;
        else out[o++] = '%', out[o++] = hex[*p >> 4], out[o++] = hex[*p & 15];
    }
    out[o] = 0;
}
