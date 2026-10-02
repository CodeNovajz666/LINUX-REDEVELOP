#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

void pmm_init(uint64_t ram_top);
uint64_t pmm_alloc(void);             /* returns physical addr, 0 on failure */
uint64_t pmm_alloc_zero(void);
void pmm_free(uint64_t pa);           /* refcount-aware: shared frames drop a ref */
void pmm_ref_inc(uint64_t pa);        /* add a COW reference */
uint32_t pmm_ref_get(uint64_t pa);    /* extra refs: 0 = exclusively owned */
uint32_t pmm_free_pages(void);
uint32_t pmm_total_pages(void);

#endif /* PMM_H */
