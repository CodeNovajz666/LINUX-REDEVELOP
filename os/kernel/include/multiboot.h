#ifndef MULTIBOOT_H
#define MULTIBOOT_H

/* Multiboot 1 info structure (subset we actually read) */
#define MULTIBOOT_BOOT_MAGIC 0x2BADB002

#define MB_FLAG_MEMINFO     0x00000001u

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;      /* KB below 1 MiB */
    uint32_t mem_upper;      /* KB above 1 MiB */
    uint32_t boot_device;
    uint32_t cmdline;        /* physical pointer */
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
};

#endif /* MULTIBOOT_H */
