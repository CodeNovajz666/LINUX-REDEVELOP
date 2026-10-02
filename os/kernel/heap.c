#include "novix.h"
#include "heap.h"
#include "lib.h"
#include "spinlock.h"

/* First-fit free-list allocator over the fixed [HEAP_START, HEAP_END) region.
 * Block layout: [header][payload ...]. Headers live in-band; neighbours are
 * coalesced on free. v0.7.0: heap_lock (leaf) replaces cli/sti -- it fences
 * every CPU, not just this one's interrupt sources. */

struct blk {
    uint32_t size;          /* total block bytes incl. header */
    uint32_t used;
    struct blk *next;       /* address-ordered singly linked */
};

#define HDR_SIZE    ((sizeof(struct blk) + 15u) & ~15u)
#define MIN_SPLIT   (HDR_SIZE + 32u)

static struct blk *head;
static size_t used_bytes;
static spinlock_t heap_lock = SPINLOCK_INIT;

void heap_init(void) {
    head = (struct blk *)HEAP_START;
    head->size = HEAP_END - HEAP_START;
    head->used = 0;
    head->next = NULL;
}

static void heap_check(struct blk *b, const char *where) {
    (void)b; (void)where;
    if (b && (b->size < HDR_SIZE || (uintptr_t)b + b->size > HEAP_END))
        panic("heap corrupt (%s): blk %llx size %08x", where,
              (unsigned long long)(uintptr_t)b, b->size);
}

void *kmalloc(size_t n) {
    if (n == 0 || n > HEAP_END - HEAP_START) return NULL;
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
    irq_flags_t f;
    spin_lock_irqsave(&heap_lock, &f);
    size_t n = 0;
    for (struct blk *b = head; b; b = b->next)
        if (!b->used) n += b->size;
    spin_unlock_irqrestore(&heap_lock, f);
    return n;
}

size_t heap_used_bytes(void) { return used_bytes; }
