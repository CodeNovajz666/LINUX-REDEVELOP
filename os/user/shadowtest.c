#include "ulib.h"
#include "etc.h"
#include "sha256.h"

/* shadowtest: the runtime leg of the v1.1.3 hash contract (the kernel leg
 * is shadow_roundtrip, the host leg is tools/mkpasswd.py). Runs as root
 * (ktest's uid suite spawns it): plants a LEGACY shadow line, proves the
 * legacy verify path, runs login's own upgrade hook and proves the line
 * came back as PBKDF2 that still verifies. Cleans up after itself.
 * Exit 45 = every assertion held. */

#define NAME "lgyt"
#define SALT "legacysa"
#define PASS "legacy-pass-9"

int main(void) {
    if (ugetuid() != 0) {
        uputs("shadowtest: needs root (the ktest uid suite spawns it)\n");
        return 1;
    }

    /* 1. plant a legacy line: name:salt:sha256hex(salt||pass) */
    char msg[64], legacy_hash[65];
    unsigned long pl = ustrlen(PASS);
    if (pl > ETC_PW_MAX) pl = ETC_PW_MAX;
    umemcpy(msg, SALT, 8);
    umemcpy(msg + 8, PASS, pl);
    sha256_hex(msg, 8 + pl, legacy_hash);
    if (etc_shadow_set(NAME, SALT, 0, legacy_hash) != 0) {
        uputs("shadowtest: cannot plant legacy line\n");
        return 2;
    }

    /* 2. the legacy line verifies through the same etc_verify login uses */
    if (etc_verify(NAME, PASS) != 0) {
        uputs("shadowtest: legacy verify failed\n");
        return 3;
    }

    /* 3. login's upgrade hook: legacy + correct password -> rewritten */
    if (etc_shadow_upgrade(NAME, PASS) != 1) {
        uputs("shadowtest: upgrade did not happen\n");
        return 4;
    }

    /* 4. the line is now PBKDF2 (iters > 0) and still verifies */
    char salt[9], hash[65];
    unsigned iters;
    if (etc_shadow_lookup(NAME, salt, &iters, hash) != 0) {
        uputs("shadowtest: upgraded line missing\n");
        return 5;
    }
    if (iters != ETC_PBKDF2_ITERS) {
        uprintf("shadowtest: wrong iters %u\n", iters);
        return 6;
    }
    if (ustrcmp(hash, legacy_hash) == 0) {
        uputs("shadowtest: hash did not change\n");
        return 7;
    }
    if (etc_verify(NAME, PASS) != 0) {
        uputs("shadowtest: upgraded verify failed\n");
        return 8;
    }
    /* a second upgrade is a no-op */
    if (etc_shadow_upgrade(NAME, PASS) != 0) {
        uputs("shadowtest: second upgrade was not a no-op\n");
        return 9;
    }

    /* 5. cleanup: drop the planted line */
    static char in[2048], out[2048];
    if (etc_read_file("/etc/shadow", in, sizeof(in)) < 0) in[0] = 0;
    int olen = 0;
    for (char *line = in; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        if (line[0] && ustrncmp(line, NAME ":", 5) != 0) {
            for (const char *p = line; *p && olen < (int)sizeof(out) - 2; p++)
                out[olen++] = *p;
            out[olen++] = '\n';
        }
        line = nl ? nl + 1 : 0;
    }
    out[olen] = 0;
    if (etc_write_file("/etc/shadow", out, olen) != 0) {
        uputs("shadowtest: cleanup failed\n");
        return 10;
    }

    uputs("SHADOWTEST-OK\n");
    return 45;
}
