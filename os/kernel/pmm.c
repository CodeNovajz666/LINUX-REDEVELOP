#include "novix.h"
#include "pmm.h"
#include "lib.h"
#include "spinlock.h"

/* Bitmap allocator for pages in [PMM_START, ram_top). Identity-mapped, so
 * pa == va for everything we hand out. Frame numbers are u64 since the
 * v0.3.0 port (paging.rs passes them across the C ABI).
 *
 * v0.4.0: COW sharing needs per-frame reference counts. Model: refcount[i]
 * counts references BEYOND the first ("extra" refs) -- 0 means exclusively
 * owned, so plain alloc/free behaviour is unchanged for non-shared frames.
 * pmm_free() decrements and only returns the frame to the bitmap at 0. */
static uint64_t ram_top;
static uint32_t bitmap[RAM_TOP_MAX / PAGE_SIZE / 32];
static uint8_t refcount[RAM_TOP_MAX / PAGE_SIZE];
static uint32_t free_count, total_count;

/* v0.7.0: cli/sti only fenced off THIS CPU's interrupts; any other CPU
 * could race the bitmap scan. Leaf lock in the global order (spinlock.h):
 * never taken while holding another lock. */
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline uint32_t page_to_bit_index(uint64_t pa) {
    return (uint32_t)((pa - PMM_START) / PAGE_SIZE);
}

void pmm_init(uint64_t top) {
    ram_top = top;
    total_count = (uint32_t)((ram_top - PMM_START) / PAGE_SIZE);
    memset(bitmap, 0xFF, sizeof(bitmap));       /* all used; mark free below */
    memset(refcount, 0, sizeof(refcount));
    free_count = 0;
    for (uint32_t i = 0; i < total_count; i++) {
        bitmap[i >> 5] &= ~(1u << (i & 31));
        free_count++;
    }
}

uint64_t pmm_alloc(void) {
    irq_flags_t f; spin_lock_irqsave(&pmm_lock, &f);
    for (uint32_t i = 0; i < total_count; i++) {
        uint32_t w = bitmap[i >> 5];
        if (w == 0xFFFFFFFFu) { i += 31; continue; }
        uint32_t b = i & 31;
        if (!(w & (1u << b))) {
            bitmap[i >> 5] |= 1u << b;
            refcount[i] = 0;                    /* exclusive owner */
            free_count--;
            spin_unlock_irqrestore(&pmm_lock, f);
            return PMM_START + (uint64_t)i * PAGE_SIZE;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, f);
    return 0;
}

uint64_t pmm_alloc_zero(void) {
    uint64_t pa = pmm_alloc();
    if (pa) memset((void *)(uintptr_t)pa, 0, PAGE_SIZE);
    return pa;
}

void pmm_ref_inc(uint64_t pa) {
    irq_flags_t f; spin_lock_irqsave(&pmm_lock, &f);
    uint32_t i = page_to_bit_index(pa);
    if (refcount[i] < 0xFF) refcount[i]++;
    spin_unlock_irqrestore(&pmm_lock, f);
}

uint32_t pmm_ref_get(uint64_t pa) {
    irq_flags_t f; spin_lock_irqsave(&pmm_lock, &f);
    uint32_t r = refcount[page_to_bit_index(pa)];
    spin_unlock_irqrestore(&pmm_lock, f);
    return r;
}

/* v1.0.1 (F5): serializes the whole COW fault decision -- refcount sample,
 * frame copy, ref drop, PTE rebase -- across CPUs. Before, two CPUs could
 * COW-fault the same shared frame concurrently and each act on a stale
 * refcount sample: one CPU's pmm_free could free the frame (or the other's
 * memcpy could read from a frame that had just been released and handed
 * back out) while this CPU was mid-fault. Nesting: cow_lock > pmm_lock
 * (the fault allocates and frees while holding it); nothing takes
 * cow_lock while holding pmm_lock, so the order stays acyclic. The #PF
 * path runs with interrupts off (interrupt gates), but keep the lock
 * irqsave so a stray caller with IF=1 cannot self-deadlock. */
static spinlock_t cow_lock = SPINLOCK_INIT;

uint32_t cow_lock_enter(void) {
    irq_flags_t f; spin_lock_irqsave(&cow_lock, &f);
    return f;
}

void cow_lock_leave(uint32_t f) {
    spin_unlock_irqrestore(&cow_lock, f);
}

void pmm_free(uint64_t pa) {
    irq_flags_t f; spin_lock_irqsave(&pmm_lock, &f);
    if (pa < PMM_START || pa >= ram_top || (pa & (PAGE_SIZE - 1))) {
        panic("pmm_free: bad pa %llx", (unsigned long long)pa);
    }
    uint32_t i = page_to_bit_index(pa);
    uint32_t b = 1u << (i & 31);
    if (!(bitmap[i >> 5] & b)) panic("pmm_free: double free %llx", (unsigned long long)pa);
    if (refcount[i] > 0) {                      /* shared: drop one ref.
                                                 * reaching 0 leaves exactly
                                                 * one live owner -- the frame
                                                 * stays allocated until THAT
                                                 * owner frees it */
        refcount[i]--;
        spin_unlock_irqrestore(&pmm_lock, f);
        return;
    }
    bitmap[i >> 5] &= ~b;
    free_count++;
    spin_unlock_irqrestore(&pmm_lock, f);
}

uint32_t pmm_free_pages(void) { return free_count; }
uint32_t pmm_total_pages(void) { return total_count; }
