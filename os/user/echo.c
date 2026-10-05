#include "ulib.h"

/* echo [-n] ARG...: print the arguments joined by single spaces. External
 * (not an osh built-in) so pipelines can use it from any stage. No escape
 * sequences -- the shell does the variable expansion before we run. */

int main(int argc, char **argv) {
    int i = 1;
    bool newline = true;
    if (argc > 1 && ustrcmp(argv[1], "-n") == 0) {
        newline = false;
        i = 2;
    }
    for (; i < argc; i++) {
        if (i > (newline ? 1 : 2)) uwrite(" ", 1);
        uwrite(argv[i], (int)ustrlen(argv[i]));
    }
    if (newline) uwrite("\n", 1);
    return 0;
}
