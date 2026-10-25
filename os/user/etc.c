#include "ulib.h"
#include "etc.h"
#include "sha256.h"

int etc_read_file(const char *path, char *buf, int max) {
    int fd = uopen(path, U_O_RDONLY);
    if (fd < 0) return -1;
    int total = 0, n;
    while (total < max - 1 && (n = uread(fd, buf + total, max - 1 - total)) > 0)
        total += n;
    uclose(fd);
    buf[total] = 0;
    return total;
}

int etc_write_file(const char *path, const char *buf, int len) {
    /* O_CREAT: /var/run/utmp does not exist until the first login
     * writes it -- shadow rewrites rely on the file already being there.
     * Known window (teaching scope): the O_TRUNC hits the target in
     * place, so a crash mid-write loses the old contents. Callers order
     * writes so the survivable state is shadow-only (register's rule);
     * a rename-based atomic replace needs a VFS rename that does not
     * exist yet. */
    int fd = uopen(path, U_O_WRONLY | U_O_CREAT | U_O_TRUNC);
    if (fd < 0) return -1;
    int n = uwrite_fd(fd, buf, len);
    uclose(fd);
    return n == len ? 0 : -1;
}

static int parse_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* bounded appends -- the shadow/utmp rewriters build output with these */
static int app_str(char *out, int olen, int cap, const char *s) {
    while (*s && olen < cap - 1) out[olen++] = *s++;
    return olen;
}

static int app_int(char *out, int olen, int cap, int v) {
    char tmp[12];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0 && i < 11) { tmp[i++] = '0' + v % 10; v /= 10; }
    while (i > 0 && olen < cap - 1) out[olen++] = tmp[--i];
    return olen;
}

static char pw_buf[2048];

/* one parser, three questions: lookup by name, lookup by uid, max uid.
 * The match and the struct fill are independent (v1.0.1 fix): a lookup
 * with out == NULL (useradd's duplicate check, passwd's target probe)
 * must still report "found" -- the old code only cleared `found` inside
 * the fill guard, so every out == NULL lookup returned -1. */
static int passwd_scan(struct pwent *out, const char *want_name, int want_uid,
                       int *max_uid) {
    if (etc_read_file("/etc/passwd", pw_buf, sizeof(pw_buf)) < 0) return -1;
    int found = -1;
    for (char *line = pw_buf; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        char *f[7] = { 0 };
        char *p = line;
        for (int i = 0; i < 7 && p; i++) {
            f[i] = p;
            char *c = ustrchr(p, ':');
            if (c) { *c = 0; p = c + 1; } else p = 0;
        }
        if (f[0] && f[2] && f[3]) {
            int uid = parse_int(f[2]);
            if (max_uid && uid > *max_uid) *max_uid = uid;
            if ((want_name && ustrcmp(f[0], want_name) == 0) ||
                (!want_name && uid == want_uid)) {
                found = 0;                      /* match regardless of out */
                if (out) {
                    ustrncpy(out->name, f[0], sizeof(out->name) - 1);
                    out->name[sizeof(out->name) - 1] = 0;
                    out->uid = uid;
                    out->gid = parse_int(f[3]);
                    /* a short line may lack home/shell: default them
                     * instead of failing the match (mangled lines are
                     * still found -- callers decide what to do) */
                    const char *home = (f[5] && f[5][0]) ? f[5] : "/home";
                    ustrncpy(out->home, home, sizeof(out->home) - 1);
                    out->home[sizeof(out->home) - 1] = 0;
                    const char *sh = (f[6] && f[6][0]) ? f[6] : "/bin/osh";
                    ustrncpy(out->shell, sh, sizeof(out->shell) - 1);
                    out->shell[sizeof(out->shell) - 1] = 0;
                }
            }
        }
        line = nl ? nl + 1 : 0;
    }
    return found;
}

int etc_getpwnam(const char *name, struct pwent *out) {
    return passwd_scan(out, name, 0, 0);
}

int etc_getpwuid(int uid, struct pwent *out) {
    return passwd_scan(out, 0, uid, 0);
}

int etc_max_uid(void) {
    int max = 0;
    passwd_scan(0, 0, 0, &max);
    return max;
}

int etc_user_count(void) {
    /* same validity rule as passwd_scan (name:..:uid:gid:..): blank and
     * mangled lines are not accounts. A missing file also reads as 0 --
     * an unreadable passwd means nobody can log in, same as an empty one */
    if (etc_read_file("/etc/passwd", pw_buf, sizeof(pw_buf)) < 0) return 0;
    int n = 0;
    for (char *line = pw_buf; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        char *p = line;
        int fields = 0;
        while (p) {
            fields++;
            char *c = ustrchr(p, ':');
            if (c) { *c = 0; p = c + 1; } else p = 0;
        }
        if (line[0] && fields >= 4) n++;
        line = nl ? nl + 1 : 0;
    }
    return n;
}

bool etc_valid_name(const char *name) {
    /* the name must survive every consumer intact: pwent.name is 24 bytes,
     * and a ':' would split the passwd/shadow line early (getpwnam would
     * then disagree with shadow_lookup and the account could never log in) */
    unsigned long n = ustrlen(name);
    return n > 0 && n < sizeof(((struct pwent *)0)->name) && !ustrchr(name, ':');
}

int etc_shadow_lookup(const char *name, char salt[9], unsigned *iters, char hash[65]) {
    static char buf[2048];
    if (etc_read_file("/etc/shadow", buf, sizeof(buf)) < 0) return -1;
    unsigned long nlen = ustrlen(name);
    for (char *line = buf; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        if (ustrncmp(line, name, nlen) == 0 && line[nlen] == ':') {
            char *saltp = line + nlen + 1;
            char *c2 = ustrchr(saltp, ':');
            if (!c2 || c2 - saltp != 8) return -1;      /* 8-char salt */
            umemcpy(salt, saltp, 8);
            salt[8] = 0;
            /* v1.1.3: name:salt:iters:hash (PBKDF2) or the legacy
             * name:salt:hash -- the iters field is what tells them apart */
            char *c3 = ustrchr(c2 + 1, ':');
            unsigned it = 0;
            const char *hashp = c2 + 1;
            if (c3) {
                it = (unsigned)parse_int(c2 + 1);
                hashp = c3 + 1;
            }
            if (iters) *iters = it;
            ustrncpy(hash, hashp, 64);
            hash[64] = 0;
            return 0;
        }
        line = nl ? nl + 1 : 0;
    }
    return -1;
}

/* the ETC_PW_MAX clamp every hasher honours (set and verify must agree,
 * or a long password could be set but never verified) */
static unsigned long pw_len(const char *password) {
    unsigned long pl = ustrlen(password);
    return pl > ETC_PW_MAX ? ETC_PW_MAX : pl;
}

int etc_verify(const char *name, const char *password) {
    char salt[9], want[65];
    unsigned iters;
    if (etc_shadow_lookup(name, salt, &iters, want) != 0) return 1;
    unsigned long pl = pw_len(password);
    char got[65];
    if (iters == 0) {
        /* legacy line: sha256(salt || password) */
        char msg[64];
        umemcpy(msg, salt, 8);
        umemcpy(msg + 8, password, pl);
        sha256_hex(msg, 8 + pl, got);
    } else {
        pbkdf2_sha256_hex(password, pl, salt, 8, iters, got);
    }
    return ustrcmp(got, want) == 0 ? 0 : 2;
}

int etc_hash_password(const char *password, char salt_out[9], char hash_out[65]) {
    etc_gen_token(salt_out, 8);
    pbkdf2_sha256_hex(password, pw_len(password), salt_out, 8,
                      ETC_PBKDF2_ITERS, hash_out);
    return 0;
}

int etc_shadow_set(const char *name, const char *salt, unsigned iters, const char *hash) {
    static char in[2048], out[2048];
    if (etc_read_file("/etc/shadow", in, sizeof(in)) < 0) in[0] = 0;
    int olen = 0;
    unsigned long nlen = ustrlen(name);
    for (char *line = in; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        /* the target user's old line is dropped; the fresh one goes on the
         * tail, so an absent account gets appended and a present one moves */
        if (line[0] && !(ustrncmp(line, name, nlen) == 0 && line[nlen] == ':')) {
            olen = app_str(out, olen, sizeof(out), line);
            olen = app_str(out, olen, sizeof(out), "\n");
        }
        line = nl ? nl + 1 : 0;
    }
    olen = app_str(out, olen, sizeof(out), name);
    olen = app_str(out, olen, sizeof(out), ":");
    olen = app_str(out, olen, sizeof(out), salt);
    olen = app_str(out, olen, sizeof(out), ":");
    if (iters) olen = app_int(out, olen, sizeof(out), (int)iters);
    if (iters) olen = app_str(out, olen, sizeof(out), ":");
    olen = app_str(out, olen, sizeof(out), hash);
    olen = app_str(out, olen, sizeof(out), "\n");
    return etc_write_file("/etc/shadow", out, olen);
}

int etc_shadow_upgrade(const char *name, const char *password) {
    char salt[9], old_hash[65];
    unsigned iters;
    if (etc_shadow_lookup(name, salt, &iters, old_hash) != 0) return 0;
    if (iters != 0) return 0;                  /* already the new format */
    if (etc_verify(name, password) != 0) return -1;   /* not actually ours */
    char nsalt[9], nhash[65];
    etc_hash_password(password, nsalt, nhash);
    return etc_shadow_set(name, nsalt, ETC_PBKDF2_ITERS, nhash) == 0 ? 1 : -1;
}

void etc_gen_token(char out[9], int n) {
    static const char alphabet[] =
        "abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789";
    /* state carries across calls so two tokens in the same millisecond
     * (same uptime, same pid) still differ */
    static unsigned long x = 0;
    if (x == 0)
        x = uuptime() * 2654435761UL +
            (unsigned long)ugetpid() * 40503UL + 0x9E3779B9UL;
    for (int i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;      /* xorshift64 */
        out[i] = alphabet[x % (sizeof(alphabet) - 1)];
    }
    out[n] = 0;
}

int etc_passwd_append(const struct pwent *e) {
    char line[160];
    int olen = 0;
    olen = app_str(line, olen, sizeof(line), e->name);
    olen = app_str(line, olen, sizeof(line), ":x:");
    olen = app_int(line, olen, sizeof(line), e->uid);
    olen = app_str(line, olen, sizeof(line), ":");
    olen = app_int(line, olen, sizeof(line), e->gid);
    olen = app_str(line, olen, sizeof(line), "::");
    olen = app_str(line, olen, sizeof(line), e->home);
    olen = app_str(line, olen, sizeof(line), ":/bin/osh\n");
    /* O_CREAT: the out-of-box image ships an empty /etc/passwd -- the
     * first append ever is the registration wizard writing root's line */
    int fd = uopen("/etc/passwd", U_O_WRONLY | U_O_APPEND | U_O_CREAT);
    if (fd < 0) return -1;
    int w = uwrite_fd(fd, line, olen);
    uclose(fd);
    return w == olen ? 0 : -1;
}

void etc_utmp_login(const char *user) {
    static char in[512], out[512];
    if (etc_read_file("/var/run/utmp", in, sizeof(in)) < 0) in[0] = 0;
    int olen = 0;
    for (char *line = in; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        /* user:tty:pid -- drop every prior tty1 session; the newest login
         * on the tty owns it (a logout leaves a stale line until the next
         * login rewrites it: documented teaching scope) */
        char *c1 = ustrchr(line, ':');
        char *c2 = c1 ? ustrchr(c1 + 1, ':') : 0;
        if (c1 && c2 && c2 - (c1 + 1) == 4 && ustrncmp(c1 + 1, "tty1", 4) == 0) {
            line = nl ? nl + 1 : 0;
            continue;
        }
        if (line[0]) {
            olen = app_str(out, olen, sizeof(out), line);
            olen = app_str(out, olen, sizeof(out), "\n");
        }
        line = nl ? nl + 1 : 0;
    }
    olen = app_str(out, olen, sizeof(out), user);
    olen = app_str(out, olen, sizeof(out), ":tty1:");
    olen = app_int(out, olen, sizeof(out), ugetpid());
    olen = app_str(out, olen, sizeof(out), "\n");
    etc_write_file("/var/run/utmp", out, olen);
}
