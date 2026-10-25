#include "ulib.h"

/* find (v1.1.6): find DIR [-name SUBSTR] -- recursive walk, printing
 * every matching path. Directory enumeration rides SYS_GETDENTS (the
 * kernel-formatted listing); rows are "  NAME  <dir>"/"  NAME  N bytes"
 * with a trailing "N entries" line. Depth is capped at 8, paths at 128
 * bytes -- teaching scope, honestly. */

#define F_DEPTH_MAX 8

static const char *g_pat;

static bool name_matches(const char *name) {
    return !g_pat || ustrstr(name, g_pat) != 0;
}

static void walk(const char *dir, int depth) {
    static char list[4096];
    int n = ugetdents(dir, list, (int)sizeof(list) - 1);
    if (n <= 0) return;
    list[n] = 0;

    for (char *line = list; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        /* "  NAME  TYPE" -- NAME is one blank-free token */
        char *p = line;
        while (*p == ' ') p++;
        char *name = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
        while (*p == ' ') p++;
        bool is_dir = ustrncmp(p, "<dir>", 5) == 0;
        if (name[0] && name[0] != '.' && ustrncmp(name, "entr", 4) != 0) {
            static char child[128];
            int o = 0;
            const char *d = dir, *w = name;
            while (*d && o < (int)sizeof(child) - 2) child[o++] = *d++;
            if (o > 0 && child[o - 1] != '/' && o < (int)sizeof(child) - 2)
                child[o++] = '/';
            while (*w && o < (int)sizeof(child) - 1) child[o++] = *w++;
            child[o] = 0;
            if (name_matches(name))
                uprintf("%s\n", child);
            if (is_dir && depth < F_DEPTH_MAX)
                walk(child, depth + 1);
        }
        line = nl ? nl + 1 : 0;
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
