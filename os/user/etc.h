#ifndef ETC_H
#define ETC_H

/* /etc parsing shared by the login-world programs (login, id, whoami, who,
 * passwd, useradd, sh). One struct, one parser -- the alternative is five
 * hand-rolled copies drifting apart. */

struct pwent {
    char name[24];
    int  uid;
    int  gid;
    char home[48];
    char shell[32];
};

/* whole-file helpers (used for the /etc files and /var/run/utmp) */
int etc_read_file(const char *path, char *buf, int max);
int etc_write_file(const char *path, const char *buf, int len);

/* password length every hasher honours: the salt||password message is
 * 8 + ETC_PW_MAX bytes (fits the 64-byte hash input). set (passwd) and
 * verify (login) must clamp identically, or a long password could be
 * set but never verified */
#define ETC_PW_MAX 40

/* passwd lookups: 0 = found */
int etc_getpwnam(const char *name, struct pwent *out);
int etc_getpwuid(int uid, struct pwent *out);
int etc_max_uid(void);                        /* highest uid in /etc/passwd */

/* v1.0.0: how many accounts /etc/passwd holds. getty treats 0 as "fresh
 * machine" and runs the registration wizard instead of login. */
int etc_user_count(void);
/* account-name rule shared by useradd and the register wizard: fits
 * pwent.name and contains no ':' */
bool etc_valid_name(const char *name);

/* v1.1.3: shadow lines are name:salt:iters:hash with PBKDF2-HMAC-SHA256
 * (iters = ETC_PBKDF2_ITERS for everything written now). Lines from older
 * disks lack the iters field (name:salt:hash, bare salted SHA-256): they
 * still verify, and login rewrites them in the new format on success. */
#define ETC_PBKDF2_ITERS 4096

/* lookup: 0 = found. *iters (may be NULL) receives the line's iteration
 * count, 0 for a legacy line. */
int etc_shadow_lookup(const char *name, char salt[9], unsigned *iters, char hash[65]);
/* 0 = password matches, 1 = no such account, 2 = mismatch (either format) */
int etc_verify(const char *name, const char *password);
/* replace the user's shadow line (append if absent): 0 = ok. v1.1.3:
 * writes the PBKDF2 format -- pass iters = ETC_PBKDF2_ITERS (0 writes a
 * legacy line; only tests do that). */
int etc_shadow_set(const char *name, const char *salt, unsigned iters, const char *hash);
/* the ONE password hasher (v1.1.3, was three drifting copies): fresh salt,
 * PBKDF2 digest, 0 = ok */
int etc_hash_password(const char *password, char salt_out[9], char hash_out[65]);
/* login's upgrade hook: when `name` verifies against a LEGACY line,
 * rehash the just-proven password in PBKDF2 format and rewrite the line.
 * 1 = upgraded, 0 = nothing to do (already new / no line), -1 = error */
int etc_shadow_upgrade(const char *name, const char *password);
int etc_passwd_append(const struct pwent *e);

/* n chars of uptime+pid entropy (salts and generated passwords). Not
 * crypto-grade -- teaching scope, recorded in DESIGN.md; PBKDF2's slow
 * factor (v1.1.3) is what actually buys brute-force resistance. */
void etc_gen_token(char out[9], int n);

/* session ledger for `who`: one line "user:tty:pid" per tty, rewritten on
 * every login (latest login on a tty wins). Logout leaves a stale line --
 * documented teaching scope, purged by the next login on the same tty. */
void etc_utmp_login(const char *user);

#endif /* ETC_H */
