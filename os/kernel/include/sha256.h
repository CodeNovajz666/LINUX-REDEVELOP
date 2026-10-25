#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * SHA-256 (FIPS 180-4), v0.9.0. v1.1.3 adds HMAC (RFC 2104) and PBKDF2
 * (RFC 8018, one block) -- /etc/shadow now stores PBKDF2-HMAC-SHA256
 * digests with a per-line iteration count; the old sha256(salt||password)
 * lines are verified and upgraded at login (see etc.c / tools/mkpasswd.py).
 */

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;                   /* total bytes hashed */
    uint8_t buf[64];
    size_t buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, size_t len);
void sha256_final(struct sha256_ctx *c, uint8_t out[32]);

void sha256(const void *data, size_t len, uint8_t out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);

void hmac_sha256(const void *key, size_t keylen,
                 const void *msg, size_t msglen, uint8_t out[32]);
void pbkdf2_sha256(const void *password, size_t pwlen,
                   const void *salt, size_t saltlen,
                   unsigned iters, uint8_t out[32]);
void pbkdf2_sha256_hex(const void *password, size_t pwlen,
                       const void *salt, size_t saltlen,
                       unsigned iters, char out[65]);

#endif /* SHA256_H */
