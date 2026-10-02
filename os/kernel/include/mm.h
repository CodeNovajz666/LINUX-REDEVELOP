#ifndef MM_H
#define MM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "novix.h"

/*
 * v0.6.0 memory manager: per-process VMA list + demand paging + swap.
 *
 * The task keeps `mm` (the address-space description) beside `page_dir`
 * (its physical CR3). #PF on a non-present page looks the address up in
 * the VMA list: a FILE vma pages the content in from its inode, an ANON
 * vma maps a zeroed frame, and a swap-marker PTE reads the frame back
 * from its slot. Anything else stays the v0.4.0 SIGSEGV semantics.
 */

/* ---- raw PTE flag bits (mirror paging.rs; C side interprets) ---- */

#define PTE_P     0x001u        /* present */
#define PTE_RW    0x002u        /* writable */
#define PTE_U     0x004u        /* user */
#define PTE_A     0x020u        /* accessed (clock hand clears it) */
#define PTE_D     0x040u        /* dirty (file writeback hint) */
#define PTE_COW   0x200u        /* copy-on-write marker (v0.4.0) */
#define PTE_SWP   0x400u        /* non-present: frame lives in swap slot */
#define PTE_FRAME 0x000FFFFFFFFFF000ull
#define PTE_SLOT(n) ((uint64_t)(n) << 12)

/* ---- VMA ---- */

#define VMA_R 0x1
#define VMA_W 0x2
#define VMA_X 0x4

#define VMA_ANON 0x0           /* zero-filled (stack / brk heap / mmap) */
#define VMA_FILE 0x1           /* backed by an inode (ELF segments) */

struct vma {
    uint64_t start, end;       /* page-aligned, [start, end) */
    uint8_t prot;              /* VMA_R|VMA_W|VMA_X */
    uint8_t flags;             /* VMA_ANON / VMA_FILE */
    struct inode *file;        /* VMA_FILE: backing inode */
    uint64_t foff;             /* VMA_FILE: file byte of `start` */
    struct vma *next;          /* sorted by start */
};

struct mm {
    struct vma *vmas;
    uint64_t mmap_top;         /* next mmap base, grows up from USER_HEAP_LIMIT */
};

/* whole-kernel accounting for `mem -v` and the ktest gates */
struct mm_stat {
    uint32_t fault_file;       /* file pages paged in */
    uint32_t fault_anon;       /* zero pages mapped */
    uint32_t fault_swapin;     /* swap slots read back */
    uint32_t fault_cow;        /* COW write faults resolved */
    uint32_t swap_out;         /* anon + private-dirty pages written to swap */
    uint32_t file_drop;        /* clean file pages dropped */
};
extern struct mm_stat g_mmstat;

struct task;

struct mm *mm_create(void);
/* reap-time cleanup: free swapped slots, the page tables and the vma list */
void mm_release(struct task *t);
/* free just the vma list + struct (no page tables): failure-path rollback */
void mm_free_vmas(struct mm *mm);

int vma_add(struct mm *mm, uint64_t start, uint64_t end, int prot, int flags,
            struct inode *file, uint64_t foff);
struct vma *vma_find(const struct mm *mm, uint64_t addr);
/* cut every vma that intersects [from, to): fully-covered vmas die,
 * straddling ones are truncated at `from` (pages already unmapped by caller) */
void vma_remove_range(struct mm *mm, uint64_t from, uint64_t to);
/* fork: shallow copy of the list (FILE vmas share the inode) */
struct mm *mm_dup(const struct mm *src);

/* #PF entry point (isr.c): 0 = resolved, -1 = not ours -> SIGSEGV */
int mm_fault(struct task *t, uint64_t cr2, int write);

/* sbrk (v0.6.0): grow/shrink only registers ANON vmas; pages fault in */
uint64_t mm_brk_grow(struct task *t, uint64_t old_brk, uint64_t new_brk);
void mm_brk_shrink(struct task *t, uint64_t old_brk, uint64_t new_brk);

/* anonymous mmap (sys_mmap/munmap backend): va or 0 */
uint64_t mm_mmap(struct task *t, uint64_t len);
int mm_munmap(struct task *t, uint64_t va, uint64_t len);

/* ---- swap (mm/swap.c): slot store + kswapd ---- */

void swap_init(void);                     /* reserve the slot frames at boot */
int swap_slot_alloc(void);                /* slot index or -1 */
void swap_slot_free(int slot);
void swap_read(int slot, uint64_t frame_pa);
void swap_write(int slot, uint64_t frame_pa);
uint32_t swap_free_slots(void);
uint32_t swap_total_slots(void);
void swap_set_watermark(uint32_t pages);  /* ktest knob: force/relax pressure
                                             (0 restores the default) */
/* fork prefault: fault every swapped page of `pd` back in, so the COW dup
 * below sees only present PTEs (dup does not copy swap markers) */
void swap_prefault(uint64_t pd, const struct mm *mm);
void swap_create_daemon(void);            /* the kswapd kernel thread */

#endif /* MM_H */
