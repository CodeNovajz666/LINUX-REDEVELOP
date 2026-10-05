#ifndef USHA256_H
#define USHA256_H

/* user-side twin of kernel/include/sha256.h -- byte-for-byte the same
 * algorithm; the shadow_roundtrip ktest plus the login path prove the two
 * copies (and tools/mkpasswd.py) agree. */

struct sha256_ctx {
    unsigned int h[8];
    unsigned long long len;
    unsigned char buf[64];
    unsigned long buflen;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, unsigned long len);
void sha256_final(struct sha256_ctx *c, unsigned char out[32]);

void sha256(const void *data, unsigned long len, unsigned char out[32]);
void sha256_hex(const void *data, unsigned long len, char out[65]);

#endif /* USHA256_H */
