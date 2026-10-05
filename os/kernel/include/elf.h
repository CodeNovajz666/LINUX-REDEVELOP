#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

struct inode;
struct mm;

/* initrd image packed by tools/mkramfs.py (generated build/ramfs_img.c);
 * unpacked into the live ramfs tree at boot by kmain. */
extern const unsigned char ramfs_img[];
extern const unsigned int ramfs_img_size;

/* register an ELF64 from its ramfs inode into a fresh address space
 * (v0.6.0: PT_LOADs become FILE/ANON VMAs, nothing is mapped; frames
 * materialise on fault). returns 0 and fills pd (physical CR3), entry
 * (rip), uesp (user stack), brk_out (page-aligned end of the image --
 * where sbrk starts).
 *
 * v0.9.0: argv[] is pushed onto the (eagerly mapped) stack top page, so
 * the process starts with the classic initial stack:
 *     rsp -> [argc][argv[0]]..[argv[argc-1]][NULL]  (strings above)
 * and crt0 hands (argc, argv) to main. argc <= TASK_MAX_ARGV, and the
 * strings must fit the page -- over-budget loads fail before any state
 * is destroyed. */
int elf_load(struct inode *ino, struct mm *mm, const char *const argv[], int argc,
             uint64_t *pd, uint64_t *entry, uint64_t *uesp, uint64_t *brk_out);

#endif /* ELF_H */
