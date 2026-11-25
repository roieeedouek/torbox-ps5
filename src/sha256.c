// SHA-256 (FIPS 180-4) of a file, for checking downloads.
#include "app.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t used;
} sha256_ctx;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(sha256_ctx *c, const unsigned char *p) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, c->h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25);
        uint32_t ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
        uint32_t t1 = s[7] + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22);
        uint32_t mj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
        memmove(s + 1, s, 7 * sizeof *s);
        s[4] += t1;
        s[0] = t1 + S0 + mj;
    }
    for (int i = 0; i < 8; i++) c->h[i] += s[i];
}

static void update(sha256_ctx *c, const unsigned char *p, size_t n) {
    c->len += n;
    while (n) {
        if (!c->used && n >= 64) {
            block(c, p);
            p += 64, n -= 64;
            continue;
        }
        size_t take = 64 - c->used < n ? 64 - c->used : n;
        memcpy(c->buf + c->used, p, take);
        c->used += take, p += take, n -= take;
        if (c->used == 64) block(c, c->buf), c->used = 0;
    }
}

int sha256_file(const char *path, char hex[65], long long *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    sha256_ctx c = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
    size_t bufsize = 4 * 1024 * 1024;
    unsigned char *buf = malloc(bufsize);
    if (!buf) {
        close(fd);
        return -1;
    }
    ssize_t n;
    while ((n = read(fd, buf, bufsize)) > 0) update(&c, buf, (size_t)n);
    free(buf);
    close(fd);
    if (n < 0) return -1;
    *size = (long long)c.len;
    uint64_t bits = c.len * 8;
    unsigned char pad[72] = {0x80};
    size_t padlen = (c.used < 56 ? 56 : 120) - c.used;
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    update(&c, pad, padlen);
    update(&c, lenb, 8);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", c.h[i]);
    return 0;
}
