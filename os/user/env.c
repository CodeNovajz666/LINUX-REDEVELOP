#include "ulib.h"

/* env (v1.1.5): print the environment this process was exec'd with --
 * crt0 fills `environ` from the initial stack, so this is the exact
 * NAME=value set the parent handed over (osh: its exported variables). */

int main(void) {
    if (!environ) {
        uputs("\n");
        return 0;
    }
    for (int i = 0; environ[i]; i++) {
        uputs(environ[i]);
        uputs("\n");
    }
    return 0;
}
