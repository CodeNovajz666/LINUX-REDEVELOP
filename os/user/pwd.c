#include "ulib.h"

/* pwd: print the working directory (SYS_GETCWD, cwd is tracked per task). */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char cwd[80];
    if (ugetcwd(cwd, sizeof(cwd)) < 0) {
        uputs("pwd: cannot get working directory\n");
        return 1;
    }
    uprintf("%s\n", cwd);
    return 0;
}
