#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* four-level paging, implemented in kernel/paging.rs (Rust staticlib) since
 * v0.3.0 milestone C -- the first Rust module of the kernel */

void paging_init(uint64_t ram_top);            /* adopts the boot.S tables */
uint64_t paging_kernel_dir(void);              /* physical CR3 for kernel */
uint64_t paging_create_user_dir(void);         /* fresh user PML4 or 0 */
uint64_t paging_dup_user_dir(uint64_t pml4_pa);/* fork: COW twin of a dir or 0 */
int paging_cow_fault(uint64_t pml4_pa, uint64_t va); /* resolve COW write fault */
void paging_destroy_user_dir(uint64_t pml4_pa);/* frees user tables/pages */
int paging_map_user(uint64_t pml4_pa, uint64_t va, uint64_t pa, int writable);
int paging_unmap_user(uint64_t pml4_pa, uint64_t va);    /* sbrk shrink: free leaf */
uint64_t paging_pte_read(uint64_t pml4_pa, uint64_t va); /* raw leaf PTE; 0 if no walk */
int paging_pte_write(uint64_t pml4_pa, uint64_t va, uint64_t pte); /* raw set; allocs tables */
uint64_t paging_cur_cr3(void);
void paging_load_cr3(uint64_t pa);

#endif /* PAGING_H */
