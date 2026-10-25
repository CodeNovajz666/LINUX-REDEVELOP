#include "ulib.h"
#include "sha256.h"

/* SHA-256 (FIPS 180-4), user side. This is a twin of kernel/sha256.c with
 * umemcpy in place of the kernel memcpy -- same rounds, same constants.
 * The shadow_roundtrip ktest proves the pair (plus tools/mkpasswd.py)
 * produce identical digests. */

static const unsigned int K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static unsigned int ror(unsigned int v, int n) {
    return (v >> n) | (v << (32 - n));
}

static void sha256_block(unsigned int h[8], const unsigned char *p) {
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[4 * i] << 24) | ((unsigned int)p[4 * i + 1] << 16) |
               ((unsigned int)p[4 * i + 2] << 8) | (unsigned int)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    unsigned int a = h[0], b = h[1], c = h[2], d = h[3];
    unsigned int e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = hh + S1 + ch + K[i] + w[i];
        unsigned int S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(struct sha256_ctx *c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0;
    c->buflen = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, unsigned long len) {
    const unsigned char *p = data;
    c->len += len;
    while (len) {
        unsigned long take = 64 - c->buflen;
        if (take > len) take = len;
        umemcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_block(c->h, c->buf);
            c->buflen = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *c, unsigned char out[32]) {
    unsigned long long bits = c->len * 8;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->buflen != 56)
        sha256_update(c, &pad, 1);
    unsigned char tail[8];
    for (int i = 0; i < 8; i++)
        tail[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, tail, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (unsigned char)(c->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)(c->h[i]);
    }
}

void sha256(const void *data, unsigned long len, unsigned char out[32]) {
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void sha256_hex(const void *data, unsigned long len, char out[65]) {
    unsigned char d[32];
    sha256(data, len, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 15];
    }
    out[64] = 0;
}

/* ---- v1.1.3: HMAC-SHA256 (RFC 2104) + PBKDF2 (RFC 8018, one block) ----
 * The password hash behind /etc/shadow is now PBKDF2-HMAC-SHA256 with a
 * per-line iteration count (see etc.h / mkpasswd.py); the bare
 * sha256(salt||password) construction survives only as the LEGACY line
 * format, verified and auto-upgraded at login. */

void hmac_sha256(const void *key, unsigned long keylen,
                 const void *msg, unsigned long msglen, unsigned char out[32]) {
    unsigned char k[64], ipad[64], opad[64], inner[32];
    umemset(k, 0, sizeof(k));
    if (keylen > 64) sha256(key, keylen, k);
    else umemcpy(k, key, keylen);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, msglen);
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}

/* dkLen = 32 (one T-block): T1 = U1 ^ U2 ^ ... ^ Uc with
 * U1 = HMAC(password, salt || INT(1)) and Uj = HMAC(password, U_{j-1}) */
void pbkdf2_sha256(const void *password, unsigned long pwlen,
                   const void *salt, unsigned long saltlen,
                   unsigned iters, unsigned char out[32]) {
    unsigned char sblock[64];
    unsigned long sb = saltlen > 60 ? 60 : saltlen;
    umemcpy(sblock, salt, sb);
    sblock[sb] = 0; sblock[sb + 1] = 0; sblock[sb + 2] = 0; sblock[sb + 3] = 1;
    unsigned char u[32], t[32];
    hmac_sha256(password, pwlen, sblock, sb + 4, u);
    umemcpy(t, u, 32);
    for (unsigned i = 1; i < iters; i++) {
        hmac_sha256(password, pwlen, u, 32, u);
        for (int j = 0; j < 32; j++) t[j] ^= u[j];
    }
    umemcpy(out, t, 32);
}

void pbkdf2_sha256_hex(const void *password, unsigned long pwlen,
                       const void *salt, unsigned long saltlen,
                       unsigned iters, char out[65]) {
    unsigned char d[32];
    pbkdf2_sha256(password, pwlen, salt, saltlen, iters, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 0xF];
    }
    out[64] = 0;
}
