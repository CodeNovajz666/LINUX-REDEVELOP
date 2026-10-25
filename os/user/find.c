#include "ulib.h"

/* find (v1.1.6): find DIR [-name SUBSTR] -- recursive walk, printing
 * every matching path. Directory enumeration rides SYS_GETDENTS (the
 * kernel-formatted listing); rows are "  NAME  <dir>"/"  NAME  N bytes"
 * with a trailing "N entries" line.
 *
 * v1.1.7 fix: the listing buffer is shared, so recursion may NOT re-enter
 * ugetdents while a parent is still iterating it (the first cut recursed
 * inline and the parent's line pointer turned into garbage -- a #GP took
 * the session down). Sub-directories are now collected first and walked
 * after the parent's loop finishes. Depth is capped at 8, paths at 160
 * bytes -- teaching scope, honestly. */

#define F_DEPTH_MAX 8
#define F_SUB_MAX   16

static const char *g_pat;

static bool name_matches(const char *name) {
    return !g_pat || ustrstr(name, g_pat) != 0;
}

static void join(char *out, int cap, const char *dir, const char *name) {
    int o = 0;
    while (*dir && o < cap - 2) out[o++] = *dir++;
    if (o > 0 && out[o - 1] != '/' && o < cap - 2) out[o++] = '/';
    while (*name && o < cap - 1) out[o++] = *name++;
    out[o] = 0;
}

static void walk(const char *dir, int depth) {
    static char list[4096];                 /* shared: no recursion while live */
    int n = ugetdents(dir, list, (int)sizeof(list) - 1);
    if (n <= 0) return;
    list[n] = 0;

    char subs[F_SUB_MAX][40];
    int nsub = 0;
    for (char *line = list; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        char *p = line;
        while (*p == ' ') p++;
        char *name = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
        while (*p == ' ') p++;
        bool is_dir = ustrncmp(p, "<dir>", 5) == 0;
        if (name[0] && name[0] != '.' && ustrncmp(name, "entr", 4) != 0) {
            char child[160];
            join(child, sizeof(child), dir, name);
            if (name_matches(name))
                uprintf("%s\n", child);
            if (is_dir && depth < F_DEPTH_MAX && nsub < F_SUB_MAX) {
                ustrncpy(subs[nsub], name, sizeof(subs[0]) - 1);
                subs[nsub][sizeof(subs[0]) - 1] = 0;
                nsub++;
            }
        }
        line = nl ? nl + 1 : 0;
    }

    /* the listing loop is done -- ugetdents may now reuse `list` safely */
    for (int i = 0; i < nsub; i++) {
        char child[160];
        join(child, sizeof(child), dir, subs[i]);
        walk(child, depth + 1);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { uputs("usage: find DIR [-name SUBSTR]\n"); return 1; }
    const char *dir = argv[1];
    if (argc > 3 && ustrcmp(argv[2], "-name") == 0) g_pat = argv[3];
    struct ustat st;
    if (ustat(dir, &st) != 0 || (st.st_mode & 0xF000) != 0x4000) {
        uprintf("find: %s: no such directory\n", dir);
        return 1;
    }
    if (name_matches(dir))                     /* the root itself matches too */
        uprintf("%s\n", dir);
    walk(dir, 1);
    return 0;
}
