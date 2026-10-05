#include "ulib.h"
#include "etc.h"

/* whoami: the account name behind the effective uid. */

int main(void) {
    struct pwent pw;
    if (etc_getpwuid(ugetuid(), &pw) != 0) {
        uprintf("whoami: no passwd entry for uid %d\n", ugetuid());
        return 1;
    }
    uprintf("%s\n", pw.name);
    return 0;
}
