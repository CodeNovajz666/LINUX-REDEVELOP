#include "ulib.h"

/* euidtest (v1.1.7): the euid/rgid split end to end. Runs as root (the
 * ktest uid suite spawns it):
 *   1. setreuid(0, 100): effective 100, real 0
 *   2. open(/etc/shadow) is DENIED (effective 100) ...
 *   3. ... but access(/etc/shadow, R_OK) says YES (real 0)
 *   4. setreuid(-1, 0) climbs back (0 is the real uid -- allowed)
 *   5. open works again
 *   6. setregid swap down/up mirrors it
 *   7. a true mortal (0,100) -> (100,0) is refused: no new privileges
 * Exit 46 = every step held. */

int main(void) {
    if (ugetuid() != 0) {
        uputs("euidtest: needs root (the ktest uid suite spawns it)\n");
        return 1;
    }

    if (usetreuid(0, 100) != 0) { uputs("euidtest: setreuid(0,100) failed\n"); return 2; }
    if (ugetuid() != 100 || ugetruid() != 0) { uputs("euidtest: split ids wrong\n"); return 3; }

    int fd = uopen("/etc/shadow", U_O_RDONLY);
    if (fd >= 0) { uputs("euidtest: euid 100 read shadow?!\n"); uclose(fd); return 4; }
    if (uaccess("/etc/shadow", U_R_OK) != 0) { uputs("euidtest: access with real id failed\n"); return 5; }

    if (usetreuid(-1, 0) != 0) { uputs("euidtest: climb back to 0 failed\n"); return 6; }
    fd = uopen("/etc/shadow", U_O_RDONLY);
    if (fd < 0) { uputs("euidtest: root open failed\n"); return 7; }
    uclose(fd);

    if (usetregid(0, 100) != 0) { uputs("euidtest: setregid(0,100) failed\n"); return 8; }
    if (ugetgid() != 100) { uputs("euidtest: egid wrong\n"); return 9; }
    if (usetregid(-1, 0) != 0) { uputs("euidtest: egid climb back failed\n"); return 10; }
    if (ugetgid() != 0) { uputs("euidtest: egid not restored\n"); return 11; }

    /* become a true mortal: real 100, effective 100 -- escalation refused */
    if (usetreuid(100, 100) != 0) { uputs("euidtest: drop to mortal failed\n"); return 12; }
    if (usetreuid(100, 0) != -1) { uputs("euidtest: mortal escalated!\n"); return 13; }
    if (uaccess("/etc/shadow", U_R_OK) != -1) { uputs("euidtest: mortal access granted!\n"); return 14; }

    uputs("EUIDTEST-OK\n");
    return 46;
}
