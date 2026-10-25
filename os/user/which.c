#include "ulib.h"

/* which (v1.1.6): print the first $PATH entry that names an existing
 * regular file for each argument (empty/absent PATH means /bin). A name
 * with '/' is reported as-is if it exists. */

static int resolve(const char *word, char *out, int cap) {
    struct ustat st;
    if (word[0] == '/' || word[0] == '.') {
        if (ustat(word, &st) == 0 && (st.st_mode & 0xF000) == 0x8000) {
            ustrncpy(out, word, cap - 1);
            out[cap - 1] = 0;
            return 0;
        }
        return -1;
    }
    const char *path = "/bin";
    if (environ) {
        for (int i = 0; environ[i]; i++) {
            if (ustrncmp(environ[i], "PATH=", 5) == 0 && environ[i][5]) {
                path = environ[i] + 5;
                break;
            }
        }
    }
    const char *p = path;
    for (;;) {
        const char *e = p;
        while (*e && *e != ':') e++;
        int n = 0;
        while (p < e && n < cap - 2) out[n++] = *p++;
        if (n > 0 && out[n - 1] != '/' && n < cap - 2) out[n++] = '/';
        const char *w = word;
        while (*w && n < cap - 1) out[n++] = *w++;
        out[n] = 0;
        if (ustat(out, &st) == 0 && (st.st_mode & 0xF000) == 0x8000) return 0;
        if (!*e) break;
        p = e + 1;
        if (!*p) break;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) { uputs("usage: which NAME...\n"); return 1; }
    int rc = 0;
    char out[96];
    for (int i = 1; i < argc; i++) {
        if (resolve(argv[i], out, sizeof(out)) == 0)
            uprintf("%s\n", out);
        else
            rc = 1;
    }
    return rc;
}
