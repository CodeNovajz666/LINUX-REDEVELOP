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

/* shadow: name:salt:sha256hex */
int etc_shadow_lookup(const char *name, char salt[9], char hash[65]);
/* 0 = password matches, 1 = no such account, 2 = mismatch */
int etc_verify(const char *name, const char *password);
/* replace the user's shadow line (append if absent): 0 = ok */
int etc_shadow_set(const char *name, const char *salt, const char *hash);
int etc_passwd_append(const struct pwent *e);

/* n chars of uptime+pid entropy (salts and generated passwords). Not
 * crypto-grade -- teaching scope, like the unkeyed hash itself. */
void etc_gen_token(char out[9], int n);

/* session ledger for `who`: one line "user:tty:pid" per tty, rewritten on
 * every login (latest login on a tty wins). Logout leaves a stale line --
 * documented teaching scope, purged by the next login on the same tty. */
void etc_utmp_login(const char *user);

#endif /* ETC_H */
