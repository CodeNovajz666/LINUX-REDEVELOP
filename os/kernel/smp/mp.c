#include "novix.h"
#include "smp.h"
#include "apic.h"
#include "console.h"
#include "lib.h"

/*
 * MP Floating Pointer / Configuration Table parsing (Intel MP spec 1.4).
 * QEMU's SeaBIOS builds an MPTable whenever -smp > 1, which is how we
 * learn the APIC id list without ACPI. Search order per spec: EBDA,
 * last KiB of base memory, then the BIOS ROM area [0xE0000, 0xFFFFF].
 *
 * Identity map note: everything scanned lives below 1 MiB, inside the
 * kernel's boot identity map, so plain pointer reads are fine.
 */

struct mp_floating {
    char     sig[4];        /* "_MP_" */
    uint32_t cfg_pa;        /* physical address of the config table */
    uint8_t  length;        /* in 16-byte paragraphs */
    uint8_t  spec_rev;
    uint8_t  checksum;
    uint8_t  features[5];
} __attribute__((packed));

struct mp_config {
    char     sig[4];        /* "PCMP" */
    uint16_t length;        /* base table length */
    uint8_t  spec_rev;
    uint8_t  checksum;
    char     oem[8];
    char     product[12];
    uint32_t oem_ptr;       /* OEM table pointer (ignored) */
    uint16_t oem_size;      /* OEM table size */
    uint16_t entry_count;   /* number of entries that follow */
    uint32_t lapic_pa;      /* default local APIC MMIO address */
    uint16_t ext_length;    /* extended table length */
    uint8_t  ext_checksum;
    uint8_t  reserved;
} __attribute__((packed));  /* 44-byte header; entries follow */

struct mp_cpu_entry {
    uint8_t  type;          /* 0 = processor */
    uint8_t  apic_id;
    uint8_t  apic_ver;
    uint8_t  flags;         /* bit0 = enabled, bit1 = BSP */
    uint32_t signature;
    uint32_t features;
    uint8_t  reserved[8];
} __attribute__((packed));

bool mp_imcr_present;       /* MP feature byte 2, bit 7: IMCR register exists */

static bool mpf_scan_range(uint64_t from, uint64_t to, const struct mp_floating **out) {
    for (uint64_t p = from; p + sizeof(struct mp_floating) <= to; p += 16) {
        const struct mp_floating *fp = (const struct mp_floating *)(uintptr_t)p;
        if (fp->sig[0] != '_' || fp->sig[1] != 'M' ||
            fp->sig[2] != 'P' || fp->sig[3] != '_')
            continue;
        /* checksum: the structure must sum to zero */
        uint8_t sum = 0;
        const uint8_t *b = (const uint8_t *)fp;
        for (size_t i = 0; i < sizeof(*fp); i++) sum += b[i];
        if (sum != 0) continue;
        *out = fp;
        return true;
    }
    return false;
}

/*
 * Fill cpus[].apic_id and ncpu. cpus[0] is the BSP (flag bit 1). Returns
 * the count found (>= 1; 1 when no MP table exists, e.g. -smp 1).
 */
int mp_parse(void) {
    const struct mp_floating *fp = NULL;

    /* EBDA: segment pointer at 0x40E (BDA) */
    uint16_t ebda_seg = *(const uint16_t *)(uintptr_t)0x40E;
    if (ebda_seg) {
        uint64_t ebda = (uint64_t)ebda_seg << 4;
        if (ebda >= 0x400 && ebda < 0x9FC00)
            mpf_scan_range(ebda, ebda + 1024, &fp);
    }
    if (!fp) {
        /* last KiB of base memory (BDA word at 0x413, in KiB) */
        uint16_t base_kb = *(const uint16_t *)(uintptr_t)0x413;
        if (base_kb >= 512 && base_kb <= 640)
            mpf_scan_range(((uint64_t)base_kb << 10) - 1024, (uint64_t)base_kb << 10, &fp);
    }
    if (!fp)
        mpf_scan_range(0xE0000, 0xFFFFF, &fp);

    cpus[0].apic_id = (int)(rdmsr(0x1B) >> 24) & 0xFF;   /* bootstrap id */
    if (!fp || fp->cfg_pa == 0) {
        mp_imcr_present = false;
        return 1;                                        /* single processor */
    }

    mp_imcr_present = (fp->features[2] & 0x80) != 0;

    const struct mp_config *cfg = (const struct mp_config *)(uintptr_t)fp->cfg_pa;
    if (cfg->sig[0] != 'P' || cfg->sig[1] != 'C' || cfg->sig[2] != 'M' ||
        cfg->sig[3] != 'P' || cfg->length < sizeof(*cfg)) {
        kprintf("smp: bad PCMP table, running single-cpu\n");
        return 1;
    }

    int found = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)(fp->cfg_pa + sizeof(*cfg));
    const uint8_t *end = (const uint8_t *)(uintptr_t)(fp->cfg_pa + cfg->length);
    while (p < end && found < SMP_MAX - 1) {
        if (*p == 0) {
            const struct mp_cpu_entry *e = (const struct mp_cpu_entry *)p;
            p += sizeof(*e);
            if (!(e->flags & 0x01)) continue;            /* disabled CPU */
            if (e->flags & 0x02) {
                cpus[0].apic_id = e->apic_id;            /* the BSP */
                continue;                                /* cpus[0] is slot 0 */
            }
            found++;                                     /* an AP */
            cpus[found].apic_id = e->apic_id;
        } else {
            p += 8;                                      /* bus/ioapic/int entries */
        }
    }
    return found + 1;
}
