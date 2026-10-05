#include "novix.h"
#include "rtc.h"
#include "console.h"

/*
 * CMOS RTC driver (v0.8.0). Ports 0x70 (index, bit7 = NMI mask) and
 * 0x71 (data). The classic read algorithm: wait out the update-in-progress
 * cycle, read all registers, then re-read the seconds to prove the set is
 * stable. BCD is decoded per register B's DM flag; QEMU defaults to BCD.
 * The century register (0x32) is not universal, so years are 2000+yy --
 * a fair assumption for a machine that boots QEMU.
 */

#define RTC_IDX_SEC     0x00
#define RTC_IDX_MIN     0x02
#define RTC_IDX_HOUR    0x04
#define RTC_IDX_DAY     0x07
#define RTC_IDX_MON     0x08
#define RTC_IDX_YEAR    0x09
#define RTC_IDX_A       0x0A
#define RTC_IDX_B       0x0B

#define RTC_A_UIP       0x80    /* update in progress */
#define RTC_B_DM        0x04    /* 1 = binary, 0 = BCD */
#define RTC_B_24H       0x02    /* 1 = 24-hour mode */

static struct rtc_time boot_time;
static bool rtc_ok;

static uint8_t cmos_read(uint8_t idx) {
    outb(0x70, idx | 0x80);                 /* keep NMI masked */
    io_wait();
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

static void decode(const uint8_t raw[6], uint8_t regb, struct rtc_time *t) {
    uint8_t vals[6];
    if (regb & RTC_B_DM)
        memcpy(vals, raw, 6);
    else
        for (int i = 0; i < 6; i++) vals[i] = bcd_to_bin(raw[i]);

    /* 12-hour mode: bit7 set = PM (we run 24h on QEMU, handle anyway) */
    if (!(regb & RTC_B_24H) && (vals[2] & 0x80)) {
        vals[2] = (uint8_t)(((vals[2] & 0x7F) + 12) % 24);
    }
    t->sec = vals[0];
    t->min = vals[1];
    t->hour = vals[2];
    t->day = vals[3];
    t->mon = vals[4];
    t->year = (uint16_t)(2000 + vals[5]);
}

int rtc_read(struct rtc_time *t) {
    if (!t) return -1;

    /* wait until the update cycle is not in progress (max ~2ms) */
    for (int i = 0; i < 20000; i++)
        if (!(cmos_read(RTC_IDX_A) & RTC_A_UIP)) break;

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t raw[6], raw2[6], regb;
        uint8_t idxs[6] = { RTC_IDX_SEC, RTC_IDX_MIN, RTC_IDX_HOUR,
                            RTC_IDX_DAY, RTC_IDX_MON, RTC_IDX_YEAR };
        for (int i = 0; i < 6; i++) raw[i] = cmos_read(idxs[i]);
        regb = cmos_read(RTC_IDX_B);
        for (int i = 0; i < 6; i++) raw2[i] = cmos_read(idxs[i]);

        bool stable = true;
        for (int i = 0; i < 6; i++)
            if (raw[i] != raw2[i]) { stable = false; break; }
        if (!stable) continue;

        decode(raw, regb, t);
        if (t->mon >= 1 && t->mon <= 12 && t->day >= 1 && t->day <= 31 &&
            t->hour <= 23 && t->min <= 59 && t->sec <= 59)
            return 0;
    }
    return -1;
}

void rtc_init(void) {
    if (rtc_read(&boot_time) == 0) {
        rtc_ok = true;
        kprintf("rtc: boot %04u-%02u-%02u %02u:%02u:%02u (localtime)\n",
                boot_time.year, boot_time.mon, boot_time.day,
                boot_time.hour, boot_time.min, boot_time.sec);
    } else {
        kprintf("rtc: CMOS clock not sane, wall time unavailable\n");
    }
}

bool rtc_valid(void) { return rtc_ok; }
const struct rtc_time *rtc_boot_time(void) { return rtc_ok ? &boot_time : NULL; }

/* days since epoch for a y/m/d (valid 1970..2099 -- matches the RTC's
 * 2000+yy assumption) */
static uint64_t days_from_civil(uint64_t y, unsigned m, unsigned d) {
    uint64_t days = 0;
    for (uint64_t yy = 1970; yy < y; yy++)
        days += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366 : 365;
    static const unsigned mdays[13] = { 0,31,28,31,30,31,30,31,31,30,31,30,31 };
    for (unsigned mm = 1; mm < m; mm++) {
        days += mdays[mm];
        if (mm == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    }
    return days + d - 1;
}

uint64_t rtc_unix_seconds(void) {
    struct rtc_time t;
    if (rtc_read(&t) != 0) return 0;
    return days_from_civil(t.year, t.mon, t.day) * 86400ull +
           t.hour * 3600ull + t.min * 60ull + t.sec;
}
