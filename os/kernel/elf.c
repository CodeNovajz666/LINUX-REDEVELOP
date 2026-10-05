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

int elf_load(struct inode *ino, struct mm *mm, const char *const argv[], int argc,
             uint64_t *pd_out, uint64_t *entry_out, uint64_t *uesp_out,
             uint64_t *brk_out) {
    if (!ino || !ino->f_op || !ino->f_op->read) return -1;
    uint64_t size = ino->size;

    /* v0.9.0: validate the argv budget before touching any state */
    if (argc < 0 || argc > TASK_MAX_ARGV) return -1;
    for (int i = 0; i < argc; i++)
        if (!argv[i]) return -1;

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

    /* user stack: an anon vma, top page eager -- argv is written into it */
    if (vma_add(mm, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE, USER_STACK_TOP,
                VMA_R | VMA_W, VMA_ANON, NULL, 0) != 0)
        goto fail;
    uint64_t top = pmm_alloc_zero();
    if (!top) goto fail;
    if (paging_map_user(pd, USER_STACK_TOP - PAGE_SIZE, top, 1) != 0) {
        pmm_free(top);
        goto fail;
    }

    /* v0.9.0: build the initial stack inside the top page, written through
     * the identity map (the kernel runs on its own CR3, so the user VA is
     * not dereferenceable here). Layout, low to high:
     *   [argc] [argv[0] .. argv[argc-1]] [NULL] [strings...]
     * argc lands 16-byte aligned so crt0's `call main` meets the SysV ABI. */
    {
        uint8_t *page = (uint8_t *)(uintptr_t)top;
        size_t slen[TASK_MAX_ARGV];
        size_t sbytes = 0;
        for (int i = 0; i < argc; i++) {
            slen[i] = strlen(argv[i]) + 1;
            if (slen[i] > 64) goto fail;           /* per-arg cap; the frame
                                                    * is mapped, destroy_user_dir
                                                    * returns it -- no free here */
            sbytes += slen[i];
        }
        /* strings end 16 bytes under the top; array + argc sit below them */
        uint32_t s_end = PAGE_SIZE - 16;
        if (sbytes > s_end - 8 * (argc + 1) - 16 - 64)
            goto fail;                              /* same: frame still mapped */
        uint32_t s_beg = (uint32_t)(s_end - sbytes);

        uint32_t arg_off[TASK_MAX_ARGV];
        uint32_t o = s_beg;
        for (int i = 0; i < argc; i++) {
            memcpy(page + o, argv[i], slen[i]);
            arg_off[i] = o;
            o += (uint32_t)slen[i];
        }

        uint32_t arr_end = s_beg & ~7u;                /* 8-byte aligned */
        uint32_t arr_beg = arr_end - 8u * (uint32_t)(argc + 1);
        uint32_t argc_off = (arr_beg - 8u) & ~15u;     /* rsp 16-byte aligned */
        /* the SysV pair [rsp]=argc / [rsp+8]=argv[0] must be contiguous:
         * when the 16-rounding opened an 8-byte gap, argv[] moved with the
         * array -- crt0 would read unwritten stack for argv[0] (v0.9.0 bug,
         * hit only by argument-length combinations osh first produced) */
        arr_beg = argc_off + 8u;

        uint64_t *arr = (uint64_t *)(page + arr_beg);
        for (int i = 0; i < argc; i++)
            arr[i] = USER_STACK_TOP - PAGE_SIZE + arg_off[i];
        arr[argc] = 0;                                 /* argv[argc] = NULL */
        *(uint64_t *)(page + argc_off) = (uint64_t)argc;

        *uesp_out = USER_STACK_TOP - PAGE_SIZE + argc_off;
    }
    *pd_out = pd;
    *entry_out = e.e_entry;
    *brk_out = (img_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return 0;

fail:
    paging_destroy_user_dir(pd);
    return -1;
}
