#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * CMOS RTC (v0.8.0): the hardware wall clock. QEMU is started with
 * `-rtc base=localtime`, so the CMOS registers hold local time and we
 * display them verbatim -- no timezone machinery, honestly.
 */

struct rtc_time {
    uint16_t year;
    uint8_t mon;         /* 1..12 */
    uint8_t day;         /* 1..31 */
    uint8_t hour;        /* 0..23 */
    uint8_t min;
    uint8_t sec;
};

void rtc_init(void);                       /* read once, remember boot time */
bool rtc_valid(void);
int rtc_read(struct rtc_time *t);          /* live read, UIP-safe */
uint64_t rtc_unix_seconds(void);           /* seconds since 1970-01-01 */
const struct rtc_time *rtc_boot_time(void);

#endif /* RTC_H */
