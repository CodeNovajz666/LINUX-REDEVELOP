#include "novix.h"
#include "mm.h"
#include "task.h"
#include "sched.h"
#include "timer.h"
#include "console.h"
#include "paging.h"
#include "pmm.h"
#include "vfs.h"
#include "percpu.h"
#include "smp.h"
#include "spinlock.h"
#include "lib.h"

/*
 * Swap (v0.6.0). The backing store is a pool of frames reserved from the
 * PMM at boot -- physically RAM, logically a disk (the roadmap's ramfs
 * /swapfile lives in the kernel heap, which is the same substance; a
 * dedicated pool avoids one 4 MiB contiguous heap allocation).
 *
 * Eviction (kswapd, a plain kernel thread): scan every user task's VMAs,
 * second-chance on the accessed bit. A clean FILE page simply drops (the
 * inode holds the original bytes); an ANON page -- or a *dirty* file page,
 * which is a private modification, never written back into the ELF inode
 * that everyone else spawns from -- copies to a free slot and its PTE
 * becomes a non-present swap marker. COW-shared pages (PTE_COW) are
 * untouchable, per the v0.4.0 contract.
 *
 * TLB safety under SMP (v0.7.0): the pass holds sched_lock, so the task
 * list and every cpus[i].current view is atomic, and it SKIPS any task
 * that is executing on some CPU. A running task's stale TLB entry would
 * keep hitting an evicted frame until invlpg, and our shootdown IPI is
 * fire-and-forget -- so a task must be off-CPU (blocked, sleeping or
 * preempted) before its pages become evictable. On switch-in the CR3
 * reload flushes whatever entries survived, closing the loop. The IPI
 * broadcast after every PTE rewrite is defense in depth: it invalidates
 * on any CPU that happens to run the same CR3 (normally none, since the
 * victim is off-CPU -- which is also why the fire-and-forget mailbox
 * race is harmless here).
 */

#define SWAP_SLOTS      1024                     /* 4 MiB of 4 KiB frames */
#define SWAP_BATCH      8                        /* evictions per scan pass */
#define SWAP_IDLE_TICKS 2                        /* 20 ms between passes */
#define SWAP_WATERMARK  64                       /* scan when free < this */

static uint64_t swap_store[SWAP_SLOTS];          /* frame PA per slot */
static uint64_t swap_map[(SWAP_SLOTS + 63) / 64];/* used bitmap */
static int swap_slots;                           /* reserved (may be < max) */
static uint32_t swap_watermark = SWAP_WATERMARK;

void swap_init(void) {
    for (int i = 0; i < SWAP_SLOTS; i++) {
        uint64_t f = pmm_alloc();
        if (!f) break;
        swap_store[i] = f;
        swap_slots = i + 1;
    }
}

int swap_slot_alloc(void) {
    for (int w = 0; w < (int)(sizeof(swap_map) / sizeof(swap_map[0])); w++) {
        if (swap_map[w] == ~0ull) continue;
        for (int b = 0; b < 64; b++) {
            int s = w * 64 + b;
            if (s >= swap_slots) return -1;
            if (!(swap_map[w] & (1ull << b))) {
                swap_map[w] |= 1ull << b;
                return s;
            }
        }
    }
    return -1;
}

void swap_slot_free(int slot) {
    if (slot < 0 || slot >= swap_slots) return;
    swap_map[slot / 64] &= ~(1ull << (slot % 64));
}

void swap_read(int slot, uint64_t frame_pa) {
    if (slot < 0 || slot >= swap_slots) return;
    memcpy((void *)(uintptr_t)frame_pa,
           (const void *)(uintptr_t)swap_store[slot], PAGE_SIZE);
}

void swap_write(int slot, uint64_t frame_pa) {
    if (slot < 0 || slot >= swap_slots) return;
    memcpy((void *)(uintptr_t)swap_store[slot],
           (const void *)(uintptr_t)frame_pa, PAGE_SIZE);
}

uint32_t swap_free_slots(void) {
    uint32_t n = 0;
    for (int s = 0; s < swap_slots; s++)
        if (!(swap_map[s / 64] & (1ull << (s % 64)))) n++;
    return n;
}

uint32_t swap_total_slots(void) { return (uint32_t)swap_slots; }

/* 0 restores the default (ktest: force pressure, then relax) */
void swap_set_watermark(uint32_t pages) {
    swap_watermark = pages ? pages : SWAP_WATERMARK;
}

/* fork helper: bring every swapped page back so paging_dup_user_dir (which
 * clones present PTEs only) sees the whole address space. The slot is
 * released; the fresh mapping becomes ordinary COW material downstream. */
void swap_prefault(uint64_t pd, const struct mm *mm) {
    if (!mm) return;
    for (const struct vma *v = mm->vmas; v; v = v->next) {
        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
            uint64_t pte = paging_pte_read(pd, va);
            if (!(pte & PTE_SWP)) continue;
            uint64_t frame = pmm_alloc();
            if (!frame) return;                  /* OOM: leave the rest swapped */
            swap_read((int)(pte >> 12), frame);
            swap_slot_free((int)(pte >> 12));
            paging_pte_write(pd, va, frame | PTE_P | PTE_U | PTE_RW);
        }
    }
}

/* one second-chance pass over every user address space. v0.7.0: the whole
 * pass runs under sched_lock -- task list, task->mm and the cpus[i].current
 * snapshot must agree, or the scan could chase a page directory another CPU
 * is tearing down (exit/exec) mid-walk. */
static void swap_scan_pass(void) {
    int evicted = 0;
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    for (struct task *t = task_first(); t; t = task_next(t)) {
        if (!t->is_user || !t->mm || t->state == TASK_UNUSED) continue;
        if (task_executing_anywhere(t)) continue;   /* on a CPU right now */
        for (struct vma *v = t->mm->vmas; v; v = v->next) {
            for (uint64_t va = v->start; va < v->end && evicted < SWAP_BATCH;
                 va += PAGE_SIZE) {
                uint64_t pte = paging_pte_read(t->page_dir, va);
                if (!(pte & PTE_P) || !(pte & PTE_U) || (pte & PTE_COW))
                    continue;                    /* absent, kernel or shared */
                if (pte & PTE_A) {               /* second chance */
                    paging_pte_write(t->page_dir, va, pte & ~PTE_A);
                    continue;
                }
                uint64_t frame = pte & PTE_FRAME;
                if (v->flags == VMA_FILE && !(pte & PTE_D)) {
                    paging_pte_write(t->page_dir, va, 0);   /* drop, keep slot-less */
                    smp_tlb_shootdown(t->page_dir, va);
                    pmm_free(frame);
                    g_mmstat.file_drop++;
                } else {
                    /* anon page, or a dirty file page (private change: the
                     * bytes live only in this frame now, not in the inode) */
                    int slot = swap_slot_alloc();
                    if (slot < 0) { spin_unlock_irqrestore(&sched_lock, f); return; }
                    swap_write(slot, frame);
                    paging_pte_write(t->page_dir, va, PTE_SWP | PTE_SLOT(slot));
                    smp_tlb_shootdown(t->page_dir, va);
                    pmm_free(frame);
                    g_mmstat.swap_out++;
                }
                evicted++;
            }
            if (evicted >= SWAP_BATCH) { spin_unlock_irqrestore(&sched_lock, f); return; }
        }
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

static void kswapd_main(void *arg) {
    (void)arg;
    for (;;) {
        if (pmm_free_pages() < swap_watermark)
            swap_scan_pass();
        task_sleep_until(current_task(), timer_ticks() + SWAP_IDLE_TICKS);
    }
}

void swap_create_daemon(void) {
    if (!task_create_kernel("kswapd", kswapd_main, NULL, 8))
        kprintf("kswapd: failed to start\n");
}
