#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

void heap_init(void);
size_t heap_base(void);      /* v1.1.1: actual first heap byte (past __bss_end) */
void *kmalloc(size_t n);
void kfree(void *p);
size_t heap_free_bytes(void);
size_t heap_used_bytes(void);

#endif /* HEAP_H */
