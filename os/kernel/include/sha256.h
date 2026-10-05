#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * SHA-256 (FIPS 180-4), v0.9.0 -- the password hash behind /etc/shadow.
 * Teaching scope: no slow factor (real systems use bcrypt/PBKDF2), which
 * is recorded as a known limitation in DESIGN.md.
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

#endif /* SHA256_H */
