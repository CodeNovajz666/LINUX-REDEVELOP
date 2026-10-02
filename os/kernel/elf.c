#include "novix.h"
#include "elf.h"
#include "mm.h"
#include "paging.h"
#include "pmm.h"
#include "vfs.h"
#include "lib.h"

/* minimal ELF64 loader (ET_EXEC, x86_64, PT_LOAD only).
 *
 * v0.6.0: registration-only. PT_LOAD segments become FILE/ANON VMAs and
 * nothing is mapped; frames materialise on first touch through the #PF
 * path (mm_fault reads from the inode). Only the stack's top page is
 * eager -- the loader must push the NULL argv terminator into it.
 */

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

#define PT_LOAD 1
#define EM_X86_64 62
#define PF_X 1
#define PF_W 2
#define PF_R 4

static int img_read(struct inode *ino, uint64_t off, void *buf, uint32_t len) {
    if (!ino || !ino->f_op || !ino->f_op->read) return -1;
    struct file f;
    memset(&f, 0, sizeof(f));
    f.inode = ino;
    f.pos = (uint32_t)off;
    int n = ino->f_op->read(&f, buf, len);
    return (n == (int)len) ? 0 : -1;
}

int elf_load(struct inode *ino, struct mm *mm,
             uint64_t *pd_out, uint64_t *entry_out, uint64_t *uesp_out,
             uint64_t *brk_out) {
    if (!ino || !ino->f_op || !ino->f_op->read) return -1;
    uint64_t size = ino->size;

    struct elf64_ehdr e;
    if (size < sizeof(e)) return -1;
    if (img_read(ino, 0, &e, sizeof(e)) != 0) return -1;
    if (e.e_ident[0] != 0x7F || e.e_ident[1] != 'E' || e.e_ident[2] != 'L' || e.e_ident[3] != 'F')
        return -1;
    if (e.e_ident[4] != 2) return -1;             /* ELFCLASS64 */
    if (e.e_machine != EM_X86_64) return -1;
    if (e.e_type != 2) return -1;                 /* ET_EXEC */
    if (e.e_phoff + (uint64_t)e.e_phnum * e.e_phentsize > size) return -1;

    uint64_t pd = paging_create_user_dir();
    if (!pd) return -1;

    uint64_t img_end = 0;
    for (int i = 0; i < e.e_phnum; i++) {
        struct elf64_phdr ph;
        if (img_read(ino, e.e_phoff + (uint64_t)i * e.e_phentsize, &ph, sizeof(ph)) != 0)
            goto fail;
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if (ph.p_offset + ph.p_filesz > size) goto fail;
        if (ph.p_vaddr + ph.p_memsz > img_end)
            img_end = ph.p_vaddr + ph.p_memsz;

        uint64_t start = ph.p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end = (ph.p_vaddr + ph.p_memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t file_end = (ph.p_vaddr + ph.p_filesz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t foff = ph.p_offset - (ph.p_vaddr - start);   /* byte of `start` */
        int prot = (ph.p_flags & PF_R ? VMA_R : 0) |
                   (ph.p_flags & PF_W ? VMA_W : 0) |
                   (ph.p_flags & PF_X ? VMA_X : 0);

        if (file_end > start &&
            vma_add(mm, start, file_end, prot, VMA_FILE, ino, foff) != 0)
            goto fail;
        if (seg_end > file_end &&                                  /* bss tail */
            vma_add(mm, file_end, seg_end, prot, VMA_ANON, NULL, 0) != 0)
            goto fail;
    }

    /* user stack: an anon vma, top page eager for the NULL argv write */
    if (vma_add(mm, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE, USER_STACK_TOP,
                VMA_R | VMA_W, VMA_ANON, NULL, 0) != 0)
        goto fail;
    uint64_t top = pmm_alloc_zero();
    if (!top) goto fail;
    if (paging_map_user(pd, USER_STACK_TOP - PAGE_SIZE, top, 1) != 0) {
        pmm_free(top);
        goto fail;
    }
    /* write the NULL argv terminator through the identity map: the kernel
     * runs on its own CR3, so USER_STACK_TOP-8 is not dereferenceable here */
    *(volatile uint64_t *)(top + PAGE_SIZE - 8) = 0;

    *pd_out = pd;
    *entry_out = e.e_entry;
    *uesp_out = USER_STACK_TOP - 16;                  /* 16-byte aligned */
    *brk_out = (img_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return 0;

fail:
    paging_destroy_user_dir(pd);
    return -1;
}
