#include "novix.h"
#include "mm.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "vfs.h"
#include "lib.h"

/*
 * VMA list + demand paging (v0.6.0).
 *
 * Every user mapping the kernel promises to resolve lives in the owning
 * task's VMA list. The page-fault path asks vma_find(); the ELF loader,
 * sbrk and mmap only *register* -- frames materialise on first touch.
 */

struct mm_stat g_mmstat;

struct mm *mm_create(void) {
    struct mm *mm = kmalloc(sizeof(*mm));
    if (!mm) return NULL;
    mm->vmas = NULL;
    mm->mmap_top = USER_HEAP_LIMIT;    /* mmap region grows up from 2 GiB */
    return mm;
}

/* ---- inode byte access without an open file (frames are identity-mapped) ---- */

static int inode_read_at(struct inode *ino, uint64_t off, void *buf, uint32_t len) {
    if (!ino || !ino->f_op || !ino->f_op->read || off >= ino->size || len == 0)
        return 0;
    if (ino->size - off < len) len = ino->size - off;
    struct file f;
    memset(&f, 0, sizeof(f));
    f.inode = ino;
    f.pos = (uint32_t)off;
    return ino->f_op->read(&f, buf, len);
}

/* ---- list management ---- */

int vma_add(struct mm *mm, uint64_t start, uint64_t end, int prot, int flags,
            struct inode *file, uint64_t foff) {
    if (!mm || start >= end || (start & (PAGE_SIZE - 1)) || (end & (PAGE_SIZE - 1)))
        return -1;

    /* anon vmas abutting an identical anon vma extend it (sbrk churn) */
    if (flags == VMA_ANON) {
        for (struct vma *v = mm->vmas; v; v = v->next) {
            if (v->flags != VMA_ANON || v->prot != prot) continue;
            if (v->end == start) { v->end = end; return 0; }
            if (v->start == end) { v->start = start; return 0; }
        }
    }
    for (struct vma *v = mm->vmas; v; v = v->next)
        if (start < v->end && v->start < end)
            return -1;                          /* overlap: caller bug */

    struct vma *v = kmalloc(sizeof(*v));
    if (!v) return -1;
    v->start = start;
    v->end = end;
    v->prot = (uint8_t)prot;
    v->flags = (uint8_t)flags;
    v->file = file;
    v->foff = foff;

    struct vma **pp = &mm->vmas;
    while (*pp && (*pp)->start < start) pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
    return 0;
}

struct vma *vma_find(const struct mm *mm, uint64_t addr) {
    for (const struct vma *v = mm ? mm->vmas : NULL; v; v = v->next)
        if (addr >= v->start && addr < v->end)
            return (struct vma *)v;
    return NULL;
}

void vma_remove_range(struct mm *mm, uint64_t from, uint64_t to) {
    if (!mm) return;
    struct vma **pp = &mm->vmas;
    while (*pp) {
        struct vma *v = *pp;
        if (v->start < to && v->end > from) {   /* intersects */
            if (v->start >= from) {
                *pp = v->next;                  /* fully covered: dies */
                kfree(v);
                continue;
            }
            v->end = from;                      /* straddles: truncate */
        }
        pp = &v->next;
    }
}

struct mm *mm_dup(const struct mm *src) {
    struct mm *mm = mm_create();
    if (!mm) return NULL;
    for (const struct vma *v = src ? src->vmas : NULL; v; v = v->next) {
        if (vma_add(mm, v->start, v->end, v->prot, v->flags, v->file, v->foff) != 0) {
            mm_free_vmas(mm);                   /* partial state: fail the fork */
            return NULL;
        }
    }
    mm->mmap_top = src ? src->mmap_top : USER_HEAP_LIMIT;
    return mm;
}

/* ---- the fault path ---- */

int mm_fault(struct task *t, uint64_t cr2, int write) {
    struct mm *mm = t->mm;
    if (!mm) return -1;
    uint64_t va = cr2 & ~(uint64_t)(PAGE_SIZE - 1);
    struct vma *v = vma_find(mm, va);
    if (!v) return -1;
    if (write && !(v->prot & VMA_W)) return -1; /* protection, not absence */

    uint64_t pte = paging_pte_read(t->page_dir, va);

    if (pte & PTE_SWP) {                        /* evicted earlier: read back */
        int slot = (int)(pte >> 12);
        uint64_t frame = pmm_alloc();
        if (!frame) return -1;
        swap_read(slot, frame);
        swap_slot_free(slot);
        if (paging_pte_write(t->page_dir, va, frame | PTE_P | PTE_U | PTE_RW) != 0) {
            pmm_free(frame);
            return -1;
        }
        g_mmstat.fault_swapin++;
        return 0;
    }

    uint64_t frame = pmm_alloc_zero();
    if (!frame) return -1;
    uint64_t fl = PTE_P | PTE_U | (v->prot & VMA_W ? PTE_RW : 0);

    if (v->flags == VMA_FILE) {
        inode_read_at(v->file, v->foff + (va - v->start),
                      (void *)(uintptr_t)frame, PAGE_SIZE);   /* tail stays zero */
        g_mmstat.fault_file++;
    } else {
        g_mmstat.fault_anon++;
    }
    if (paging_pte_write(t->page_dir, va, frame | fl) != 0) {
        pmm_free(frame);
        return -1;
    }
    return 0;
}

/* ---- brk: register only, never map ---- */

uint64_t mm_brk_grow(struct task *t, uint64_t old_brk, uint64_t new_brk) {
    uint64_t from = (old_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t to = (new_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (to <= from) return 0;
    if (vma_add(t->mm, from, to, VMA_R | VMA_W, VMA_ANON, NULL, 0) != 0)
        return (uint64_t)-1;
    return to - from;
}

void mm_brk_shrink(struct task *t, uint64_t old_brk, uint64_t new_brk) {
    uint64_t from = (new_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t to = (old_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t va = from; va < to; va += PAGE_SIZE) {
        uint64_t pte = paging_pte_read(t->page_dir, va);
        if (pte & PTE_SWP) swap_slot_free((int)(pte >> 12));
        paging_unmap_user(t->page_dir, va);     /* frees the frame if present */
    }
    vma_remove_range(t->mm, from, to);
}

/* ---- anonymous mmap ---- */

uint64_t mm_mmap(struct task *t, uint64_t len) {
    if (!t->mm || len == 0 || len > (1ull << 30)) return 0;
    uint64_t al = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t va = t->mm->mmap_top;
    if (vma_add(t->mm, va, va + al, VMA_R | VMA_W, VMA_ANON, NULL, 0) != 0)
        return 0;
    t->mm->mmap_top += al;
    return va;
}

int mm_munmap(struct task *t, uint64_t va, uint64_t len) {
    struct vma *v = vma_find(t->mm, va);
    if (!v || va < v->start || len == 0 || va + len > v->end)
        return -1;                              /* whole vmas only */
    for (uint64_t p = va; p < va + len; p += PAGE_SIZE) {
        uint64_t pte = paging_pte_read(t->page_dir, p);
        if (pte & PTE_SWP) swap_slot_free((int)(pte >> 12));
        paging_unmap_user(t->page_dir, p);
    }
    vma_remove_range(t->mm, va, va + len);
    return 0;
}

void mm_free_vmas(struct mm *mm) {
    if (!mm) return;
    struct vma *v = mm->vmas;
    while (v) { struct vma *x = v->next; kfree(v); v = x; }
    kfree(mm);
}

/* ---- reap / exec teardown ---- */

void mm_release(struct task *t) {
    if (!t || !t->is_user) return;
    if (t->mm) {
        for (struct vma *v = t->mm->vmas; v; v = v->next) {
            for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
                uint64_t pte = paging_pte_read(t->page_dir, va);
                if (pte & PTE_SWP) swap_slot_free((int)(pte >> 12));
            }
        }
    }
    paging_destroy_user_dir(t->page_dir);       /* frees present frames + tables */
    mm_free_vmas(t->mm);
    t->mm = NULL;
}
