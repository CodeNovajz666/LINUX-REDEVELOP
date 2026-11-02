#include "novix.h"
#include "heap.h"
#include "lib.h"
#include "spinlock.h"

/* First-fit free-list allocator over [heap_base, HEAP_END). Block layout:
 * [header][payload ...]. Headers live in-band; neighbours are coalesced on
 * free. v0.7.0: heap_lock (leaf) replaces cli/sti -- it fences every CPU,
 * not just this one's interrupt sources.
 *
 * v1.1.1: the heap starts AFTER the kernel image. HEAP_START used to be a
 * fixed 0x400000 and the v1.1.0 kernel's .bss ended 0x4458 bytes below it;
 * the next bit of embedded ramfs growth pushed __bss_end past the constant
 * and the allocator began handing out the kernel's own .bss (ramfs nodes on
 * the boot stack, wild memcpys, #DF at boot). The base is now computed from
 * the linker's __bss_end at init time -- growing the kernel just moves the
 * heap up. */

extern char __bss_end[];               /* linker.ld (boot.S zeroes to it) */

struct blk {
    uint32_t size;          /* total block bytes incl. header */
    uint32_t used;
    struct blk *next;       /* address-ordered singly linked */
};

#define HDR_SIZE    ((sizeof(struct blk) + 15u) & ~15u)
#define MIN_SPLIT   (HDR_SIZE + 32u)

static struct blk *head;
static size_t used_bytes;
static size_t free_bytes;                       /* mirrors the used counter;
                                                 * coalescing moves size between
                                                 * free blocks, so it never
                                                 * touches these two */
static uintptr_t heap_lo;                       /* actual first heap byte */
static spinlock_t heap_lock = SPINLOCK_INIT;

size_t heap_base(void) { return heap_lo; }

void heap_init(void) {
    uintptr_t base = ((uintptr_t)__bss_end + 0xFFFFu) & ~0xFFFFu;  /* 64 KiB up */
    if (base < HEAP_START) base = HEAP_START;   /* small kernels: old spot */
    if (base >= HEAP_END)
        panic("heap: kernel image (%llx) leaves no room below 16 MiB",
              (unsigned long long)base);
    heap_lo = base;
    head = (struct blk *)base;
    head->size = HEAP_END - base;
    head->used = 0;
    head->next = NULL;
    free_bytes = head->size;
}

static void heap_check(struct blk *b, const char *where) {
    (void)b; (void)where;
    if (b && (b->size < HDR_SIZE || (uintptr_t)b + b->size > HEAP_END
             || (uintptr_t)b < heap_lo))
        panic("heap corrupt (%s): blk %llx size %08x", where,
              (unsigned long long)(uintptr_t)b, b->size);
}

void *kmalloc(size_t n) {
    if (n == 0 || n > HEAP_END - heap_lo) return NULL;
    irq_flags_t f; spin_lock_irqsave(&heap_lock, &f);
    size_t need = HDR_SIZE + ((n + 15u) & ~15u);
    for (struct blk *b = head; b; b = b->next) {
        heap_check(b, "scan");
        if (!b->used && b->size >= need) {
            if (b->size >= need + MIN_SPLIT) {
                struct blk *rest = (struct blk *)((char *)b + need);
                rest->size = b->size - need;
                rest->used = 0;
                rest->next = b->next;
                b->size = need;
                b->next = rest;
            }
            b->used = 1;
            used_bytes += b->size;
            free_bytes -= b->size;
            spin_unlock_irqrestore(&heap_lock, f);
            return (void *)((char *)b + HDR_SIZE);
        }
    }
    spin_unlock_irqrestore(&heap_lock, f);
    return NULL;
}

void kfree(void *p) {
    if (!p) return;
    irq_flags_t f; spin_lock_irqsave(&heap_lock, &f);
    struct blk *b = (struct blk *)((char *)p - HDR_SIZE);
    heap_check(b, "free");
    if (!b->used) panic("kfree: double free %llx", (unsigned long long)(uintptr_t)p);
    b->used = 0;
    used_bytes -= b->size;
    free_bytes += b->size;
    /* coalesce forward */
    struct blk *cur = b;
    while (cur->next && !cur->next->used &&
           (char *)cur + cur->size == (char *)cur->next) {
        cur->size += cur->next->size;
        cur->next = cur->next->next;
    }
    /* coalesce backward: find predecessor */
    for (struct blk *a = head; a && a != b; a = a->next) {
        if (!a->used && (char *)a + a->size == (char *)b) {
            a->size += b->size;
            a->next = b->next;
            break;
        }
    }
    spin_unlock_irqrestore(&heap_lock, f);
}

size_t heap_free_bytes(void) {
    return free_bytes;              /* maintained at alloc/free; was an
                                     * O(free-list) walk under the lock */
}

size_t heap_used_bytes(void) { return used_bytes; }
