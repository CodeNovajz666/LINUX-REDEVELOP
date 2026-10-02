// Novix four-level paging (x86_64 long mode) -- the kernel's first Rust
// module (v0.3.0 milestone C).
//
// Compiled as a no_std staticlib for x86_64-unknown-none and linked with
// the C objects; the C side sees only the extern "C" surface declared in
// include/paging.h. Cross-language contract (ROADMAP: "novix.h <-> novix.rs
// 类型手工对齐"): only POD values cross the boundary, and the constants
// below mirror include/novix.h by hand.
//
// Design, one level deeper than the i386 paging.c it replaces:
//   * kernel low map: boot.S builds PML4[0] -> PDPT[0] -> PD of 512 x 2 MiB
//     supervisor pages covering [0, 1 GiB). paging_init() adopts that table
//     as the kernel CR3 (identity mapping: physical == virtual for every
//     kernel address, so table pointers are usable as physical addresses).
//   * user directories share the kernel low map at PD granularity: user
//     PML4[0] points to a per-process PDPT whose entry 0 is a copy of the
//     kernel PDPT[0] (same supervisor PD). User code starts at
//     USER_CODE_BASE = 1 GiB, the first byte above the kernel map, inside
//     PDPT slot 1 of that same per-process table; the user stack lives
//     near the top of the low canonical half (PML4[255]).
//   * user pages: 4 KiB, present|rw|user, frames from the C pmm.

#![no_std]

use core::fmt::Write;
use core::sync::atomic::{AtomicU64, Ordering};

/* ---- PTE flag bits ---- */

const P_PRESENT: u64 = 1 << 0;
const P_RW: u64 = 1 << 1;
const P_USER: u64 = 1 << 2;
const P_ACCESSED: u64 = 1 << 5; // cpu sets on any access (clock hand clears it)
const P_DIRTY: u64 = 1 << 6; // cpu sets on write (writeback hint)
const P_PS: u64 = 1 << 7; // page-size bit: 2 MiB leaf at PD level
const P_COW: u64 = 1 << 9; // software bit: write-fault on this page = copy-on-write (v0.4.0)
const P_SWP: u64 = 1 << 10; // software bit: non-present PTE whose frame is in a swap slot (v0.6.0)

const ENTRIES: usize = 512;
const PAGE_SIZE: u64 = 4096;
const ADDR_MASK: u64 = 0x000F_FFFF_FFFF_F000;

/* mirror of include/novix.h (kept in sync by hand) */
const KERNEL_MAP_TOP: u64 = 0x4000_0000; // 1 GiB
const USER_CODE_BASE: u64 = 0x4000_0000;
const USER_STACK_TOP: u64 = 0x7FFF_FFFF_E000;

extern "C" {
    static boot_pml4: [u64; ENTRIES]; // built by boot.S before kmain
    fn pmm_alloc() -> u64; // kernel/pmm.c
    fn pmm_alloc_zero() -> u64;
    fn pmm_free(pa: u64); // refcount-aware since v0.4.0
    fn pmm_ref_inc(pa: u64);
    fn pmm_ref_get(pa: u64) -> u32;
    fn rust_panic_msg(msg: *const u8) -> !; // kernel/kpanic.c
}

static KERNEL_PML4: AtomicU64 = AtomicU64::new(0);

/* ---- index helpers ---- */

fn idx_pml4(va: u64) -> usize { ((va >> 39) & 511) as usize }
fn idx_pdpt(va: u64) -> usize { ((va >> 30) & 511) as usize }
fn idx_pd(va: u64) -> usize { ((va >> 21) & 511) as usize }
fn idx_pt(va: u64) -> usize { ((va >> 12) & 511) as usize }

fn table_of(entry: u64) -> *mut u64 { (entry & ADDR_MASK) as *mut u64 }

/* ---- C ABI ---- */

#[no_mangle]
pub extern "C" fn paging_init(ram_top: u64) {
    // boot.S already built this table, loaded CR3 and enabled paging;
    // adopt it as THE kernel directory. The boot map is a fixed 1 GiB --
    // refuse anything larger instead of silently mapping beyond it.
    if ram_top > KERNEL_MAP_TOP {
        panic_rs("ram above the 1 GiB boot map");
    }
    KERNEL_PML4.store(unsafe { &boot_pml4 as *const _ as u64 }, Ordering::Relaxed);

    // CR0.WP = 1: ring0 must respect the write-protect bit too, otherwise
    // a kernel memcpy into a COW-readonly user page would corrupt the
    // shared frame silently instead of faulting (v0.4.0 COW prerequisite).
    unsafe {
        let cr0: u64;
        core::arch::asm!("mov {0}, cr0", lateout(reg) cr0, options(nomem, preserves_flags));
        core::arch::asm!("mov cr0, {0}", in(reg) cr0 | (1 << 16), options(nomem, preserves_flags));
    }
}

#[no_mangle]
pub extern "C" fn paging_kernel_dir() -> u64 {
    KERNEL_PML4.load(Ordering::Relaxed)
}

#[no_mangle]
pub extern "C" fn paging_cur_cr3() -> u64 {
    let cr3: u64;
    unsafe { core::arch::asm!("mov {0}, cr3", out(reg) cr3, options(nomem, preserves_flags)) };
    cr3 & ADDR_MASK
}

#[no_mangle]
pub extern "C" fn paging_load_cr3(pa: u64) {
    unsafe { core::arch::asm!("mov cr3, {0}", in(reg) pa, options(nomem, preserves_flags)) };
}

/// Fresh user PML4 sharing the kernel low map:
/// PML4[0] -> new PDPT, PDPT[0] = kernel PDPT[0] (supervisor, 2 MiB pages).
/// v0.7.0: PDPT[3] = kernel PDPT[3] -- the [3GiB, 4GiB) APIC MMIO map, so
/// the LAPIC stays reachable (apic_eoi from any interrupt path) on every
/// CR3 in the system; its leafs are supervisor-only, ring 3 cannot touch it.
/// Every level of a user-reachable walk needs the USER bit; the shared
/// kernel leaf pages stay supervisor-only, so ring3 still faults on them.
#[no_mangle]
pub extern "C" fn paging_create_user_dir() -> u64 {
    unsafe {
        let pml4_pa = pmm_alloc_zero();
        if pml4_pa == 0 { return 0; }
        let pdpt_pa = pmm_alloc_zero();
        if pdpt_pa == 0 {
            pmm_free(pml4_pa);
            return 0;
        }
        let kernel_pdpt = table_of(boot_pml4[0]);
        let pdpt = pdpt_pa as *mut u64;
        *pdpt.add(0) = kernel_pdpt.read_volatile() & ADDR_MASK | P_PRESENT | P_RW | P_USER;
        *pdpt.add(3) = kernel_pdpt.add(3).read_volatile() & ADDR_MASK | P_PRESENT | P_RW | P_USER;
        let pml4 = pml4_pa as *mut u64;
        *pml4.add(0) = pdpt_pa | P_PRESENT | P_RW | P_USER;
        pml4_pa
    }
}

/// Map one 4 KiB user page. Refuses anything outside
/// [USER_CODE_BASE, USER_STACK_TOP) -- the kernel low map is not writable
/// through this entry point. Missing PML4/PDPT/PD tables are allocated on
/// demand, so the user stack (PML4[255]) needs no pre-wiring.
#[no_mangle]
pub extern "C" fn paging_map_user(pml4_pa: u64, va: u64, pa: u64, writable: i32) -> i32 {
    if va & (PAGE_SIZE - 1) != 0 || pa & (PAGE_SIZE - 1) != 0 { return -1; }
    if va < USER_CODE_BASE || va >= USER_STACK_TOP { return -1; }

    unsafe {
        let pml4 = pml4_pa as *mut u64;

        // PML4 -> PDPT (allocated on demand: user code lands in the
        // per-process PDPT under PML4[0], the stack gets its own at PML4[255])
        let e4 = pml4.add(idx_pml4(va)).read_volatile();
        let pdpt_pa = if e4 & P_PRESENT == 0 {
            let t = pmm_alloc_zero();
            if t == 0 { return -1; }
            pml4.add(idx_pml4(va)).write_volatile(t | P_PRESENT | P_RW | P_USER);
            t
        } else if e4 & P_PS != 0 {
            return -1; // huge leaf: never created by us
        } else {
            e4 & ADDR_MASK
        };
        let pdpt = pdpt_pa as *mut u64;

        // PDPT -> PD
        let e3 = pdpt.add(idx_pdpt(va)).read_volatile();
        let pd_pa = if e3 & P_PRESENT == 0 {
            let t = pmm_alloc_zero();
            if t == 0 { return -1; }
            pdpt.add(idx_pdpt(va)).write_volatile(t | P_PRESENT | P_RW | P_USER);
            t
        } else if e3 & P_PS != 0 {
            return -1; // 1 GiB leaf: never created by us
        } else {
            e3 & ADDR_MASK
        };
        let pd = pd_pa as *mut u64;

        // PD -> PT (user pages are always 4 KiB)
        let e2 = pd.add(idx_pd(va)).read_volatile();
        let pt_pa = if e2 & P_PRESENT == 0 {
            let t = pmm_alloc_zero();
            if t == 0 { return -1; }
            pd.add(idx_pd(va)).write_volatile(t | P_PRESENT | P_RW | P_USER);
            t
        } else if e2 & P_PS != 0 {
            return -1; // 2 MiB leaf where a user page was requested
        } else {
            e2 & ADDR_MASK
        };
        let pt = pt_pa as *mut u64;

        let mut flags = P_PRESENT | P_USER;
        if writable != 0 { flags |= P_RW; }
        pt.add(idx_pt(va)).write_volatile(pa & ADDR_MASK | flags);

        core::arch::asm!("invlpg [{0}]", in(reg) va, options(preserves_flags));
    }
    0
}

/// Fork's dup_mm (v0.4.0): build a child directory that shares every user
/// frame with the parent. Both sides' leaf PTEs lose P_RW and gain P_COW;
/// the frame's PMM refcount goes up by one. Table pages are fresh (one
/// private copy per process); PDPT[0] of PML4[0] still shares the kernel's
/// supervisor PD, exactly like paging_create_user_dir.
///
/// Runs inside the fork syscall with interrupts off (int 0x80 is an
/// interrupt gate), so the parent/child PTE pair can never be observed
/// half-updated. When SMP arrives (0.7.0) this needs real locking.
#[no_mangle]
pub extern "C" fn paging_dup_user_dir(parent_pa: u64) -> u64 {
    unsafe {
        let parent = parent_pa as *const u64;

        let pml4_pa = pmm_alloc_zero();
        if pml4_pa == 0 { return 0; }
        let pdpt_pa = pmm_alloc_zero();
        if pdpt_pa == 0 {
            pmm_free(pml4_pa);
            return 0;
        }
        let child = pml4_pa as *mut u64;
        let cdpt = pdpt_pa as *mut u64;
        let kernel_pdpt = table_of(boot_pml4[0]);
        *cdpt.add(0) = kernel_pdpt.read_volatile() & ADDR_MASK | P_PRESENT | P_RW | P_USER;
        *cdpt.add(3) = kernel_pdpt.add(3).read_volatile() & ADDR_MASK | P_PRESENT | P_RW | P_USER;
        *child.add(0) = pdpt_pa | P_PRESENT | P_RW | P_USER;

        for i in 0..ENTRIES {
            let e4 = parent.add(i).read_volatile();
            if e4 & P_PRESENT == 0 { continue; }
            if e4 & P_PS != 0 { continue; }            // never created by us
            let ppdpt = table_of(e4);

            // PML4[0] is the per-process table whose PDPT[0] shares the
            // kernel's supervisor PD (wired above); its user mappings --
            // code at 1 GiB sits in PDPT[1] -- clone from index 1. PDPT[3]
            // is the shared APIC MMIO map (v0.7.0): wired above, never
            // cloned. Every other PML4 slot (stack at [255], ...) clones in
            // full.
            let (cpdpt, jstart) = if i == 0 {
                (cdpt, 1)
            } else {
                let t = pmm_alloc_zero();
                if t == 0 { paging_destroy_user_dir(pml4_pa); return 0; }
                *child.add(i) = t | P_PRESENT | P_RW | P_USER;
                (t as *mut u64, 0)
            };

            for j in jstart..ENTRIES {
                if i == 0 && j == 3 { continue; }      // shared APIC MMIO map
                let e3 = ppdpt.add(j).read_volatile();
                if e3 & P_PRESENT == 0 || e3 & P_PS != 0 { continue; }
                let ppd = table_of(e3);

                let cpd_pa = pmm_alloc_zero();
                if cpd_pa == 0 { paging_destroy_user_dir(pml4_pa); return 0; }
                *cpdpt.add(j) = cpd_pa | P_PRESENT | P_RW | P_USER;
                let cpd = cpd_pa as *mut u64;

                for k in 0..ENTRIES {
                    let e2 = ppd.add(k).read_volatile();
                    if e2 & P_PRESENT == 0 || e2 & P_PS != 0 { continue; }
                    let ppt = table_of(e2);

                    let cpt_pa = pmm_alloc_zero();
                    if cpt_pa == 0 { paging_destroy_user_dir(pml4_pa); return 0; }
                    *cpd.add(k) = cpt_pa | P_PRESENT | P_RW | P_USER;
                    let cpt = cpt_pa as *mut u64;

                    for m in 0..ENTRIES {
                        let e1 = ppt.add(m).read_volatile();
                        if e1 & P_PRESENT == 0 { continue; }
                        let frame = e1 & ADDR_MASK;
                        let va = ((i as u64) << 39) | ((j as u64) << 30)
                               | ((k as u64) << 21) | ((m as u64) << 12);

                        // parent: read-only + COW marker
                        ppt.add(m).write_volatile(e1 & !P_RW | P_COW);
                        core::arch::asm!("invlpg [{}]", in(reg) va, options(preserves_flags));

                        // child: same frame, same flags minus RW, plus COW
                        pmm_ref_inc(frame);
                        cpt.add(m).write_volatile(frame | (e1 & !ADDR_MASK & !P_RW) | P_COW);
                    }
                }
            }
        }
        pml4_pa
    }
}

/// Unmap one user page (sbrk shrink, v0.5.0). The frame goes back through
/// pmm_free's refcount-aware path, so a COW-shared frame just drops one
/// reference. Missing intermediate levels or a non-present leaf are a no-op
/// (idempotent); huge leaves are never created by us, so they read as absent.
#[no_mangle]
pub extern "C" fn paging_unmap_user(pml4_pa: u64, va: u64) -> i32 {
    if va & (PAGE_SIZE - 1) != 0 { return -1; }
    if va < USER_CODE_BASE || va >= USER_STACK_TOP { return -1; }

    unsafe {
        let pml4 = pml4_pa as *const u64;
        let e4 = pml4.add(idx_pml4(va)).read_volatile();
        if e4 & P_PRESENT == 0 || e4 & P_PS != 0 { return 0; }
        let e3 = table_of(e4).add(idx_pdpt(va)).read_volatile();
        if e3 & P_PRESENT == 0 || e3 & P_PS != 0 { return 0; }
        let e2 = table_of(e3).add(idx_pd(va)).read_volatile();
        if e2 & P_PRESENT == 0 || e2 & P_PS != 0 { return 0; }
        let pte_p = table_of(e2).add(idx_pt(va));
        let e1 = pte_p.read_volatile();
        if e1 & P_PRESENT == 0 { return 0; }           // already gone

        pmm_free(e1 & ADDR_MASK);                      // refcount-aware drop
        pte_p.write_volatile(0);
        core::arch::asm!("invlpg [{0}]", in(reg) va, options(preserves_flags));
    }
    0
}

/// Raw leaf-PTE access for the v0.6.0 memory manager (demand paging, swap).
///
/// pte_read returns the leaf entry verbatim -- 0 when any walk level is
/// missing, or the *non-present* swap marker entry (P_SWP | slot << 12) when
/// the page was evicted. The C side owns every interpretation.
#[no_mangle]
pub extern "C" fn paging_pte_read(pml4_pa: u64, va: u64) -> u64 {
    if va & (PAGE_SIZE - 1) != 0 { return 0; }
    unsafe {
        let pml4 = pml4_pa as *const u64;
        let e4 = pml4.add(idx_pml4(va)).read_volatile();
        if e4 & P_PRESENT == 0 || e4 & P_PS != 0 { return 0; }
        let e3 = table_of(e4).add(idx_pdpt(va)).read_volatile();
        if e3 & P_PRESENT == 0 || e3 & P_PS != 0 { return 0; }
        let e2 = table_of(e3).add(idx_pd(va)).read_volatile();
        if e2 & P_PRESENT == 0 || e2 & P_PS != 0 { return 0; }
        table_of(e2).add(idx_pt(va)).read_volatile()
    }
}

/// Write a raw leaf PTE, allocating any missing intermediate tables on the
/// way down (identical walk to paging_map_user). Used to install demand
/// mappings, swap-marker PTEs and to clear the accessed bit in place.
#[no_mangle]
pub extern "C" fn paging_pte_write(pml4_pa: u64, va: u64, pte: u64) -> i32 {
    if va & (PAGE_SIZE - 1) != 0 { return -1; }
    if va < USER_CODE_BASE || va >= USER_STACK_TOP { return -1; }
    unsafe {
        let pml4 = pml4_pa as *mut u64;
        let e4 = pml4.add(idx_pml4(va)).read_volatile();
        if e4 & P_PS != 0 { return -1; }            // huge leaf: never created by us
        let pdpt_pa = if e4 & P_PRESENT == 0 {
            let t = pmm_alloc_zero();
            if t == 0 { return -1; }
            pml4.add(idx_pml4(va)).write_volatile(t | P_PRESENT | P_RW | P_USER);
            t
        } else { e4 & ADDR_MASK };
        let pdpt = pdpt_pa as *mut u64;

        let e3 = pdpt.add(idx_pdpt(va)).read_volatile();
        if e3 & P_PS != 0 { return -1; }
        let pd_pa = if e3 & P_PRESENT == 0 {
            let t = pmm_alloc_zero();
            if t == 0 { return -1; }
            pdpt.add(idx_pdpt(va)).write_volatile(t | P_PRESENT | P_RW | P_USER);
            t
        } else { e3 & ADDR_MASK };
        let pd = pd_pa as *mut u64;

        let e2 = pd.add(idx_pd(va)).read_volatile();
        if e2 & P_PS != 0 { return -1; }
        let pt_pa = if e2 & P_PRESENT == 0 {
            let t = pmm_alloc_zero();
            if t == 0 { return -1; }
            pd.add(idx_pd(va)).write_volatile(t | P_PRESENT | P_RW | P_USER);
            t
        } else { e2 & ADDR_MASK };

        table_of(pt_pa).add(idx_pt(va)).write_volatile(pte);
        core::arch::asm!("invlpg [{0}]", in(reg) va, options(preserves_flags));
    }
    0
}

/// COW write-fault resolution, called from isr.c on #PF where the error
/// code says "write to a present page". Exclusive frame (refcount 0):
/// just re-grant write. Shared: allocate a copy, move the PTE, drop the
/// old ref. Everything below 1 GiB is identity-mapped, so the 4 KiB copy
/// runs without any CR3 gymnastics. Returns 0 when the fault is resolved,
/// -1 when the address is not a COW page (the caller kills the process).
#[no_mangle]
pub extern "C" fn paging_cow_fault(pml4_pa: u64, va: u64) -> i32 {
    if va < USER_CODE_BASE || va >= USER_STACK_TOP { return -1; }
    unsafe {
        let pml4 = pml4_pa as *const u64;
        let e4 = pml4.add(idx_pml4(va)).read_volatile();
        if e4 & P_PRESENT == 0 { return -1; }
        let e3 = table_of(e4).add(idx_pdpt(va)).read_volatile();
        if e3 & P_PRESENT == 0 || e3 & P_PS != 0 { return -1; }
        let e2 = table_of(e3).add(idx_pd(va)).read_volatile();
        if e2 & P_PRESENT == 0 || e2 & P_PS != 0 { return -1; }
        let pte_p = table_of(e2).add(idx_pt(va));
        let e1 = pte_p.read_volatile();
        if e1 & P_PRESENT == 0 { return -1; }
        if e1 & P_COW == 0 { return -1; }             // not ours to fix

        let frame = e1 & ADDR_MASK;
        if pmm_ref_get(frame) == 0 {
            // last owner: keep the frame, re-grant write
            pte_p.write_volatile(e1 | P_RW);
        } else {
            let copy = pmm_alloc();
            if copy == 0 { return -1; }
            core::ptr::copy_nonoverlapping(frame as *const u8, copy as *mut u8, 4096);
            pmm_free(frame);                          // refcount-aware drop
            pte_p.write_volatile(copy | (e1 & !ADDR_MASK & !P_COW) | P_RW);
        }
        core::arch::asm!("invlpg [{}]", in(reg) va, options(preserves_flags));
    }
    0
}

/// Free every table and page owned by a user directory. The kernel PD
/// shared through PDPT[0] stays alive (it belongs to the kernel PML4).
#[no_mangle]
pub extern "C" fn paging_destroy_user_dir(pml4_pa: u64) {
    unsafe {
        let pml4 = pml4_pa as *mut u64;
        for i in 0..ENTRIES {
            let e4 = pml4.add(i).read_volatile();
            if e4 & P_PRESENT == 0 { continue; }
            let pdpt = table_of(e4);
            // PDPT[0] of the PML4[0] chain shares the kernel's PD, and
            // PDPT[3] shares the APIC MMIO PD (v0.7.0): keep both, free
            // everything else (that also covers user code at 1 GiB+).
            let first = if i == 0 { 1 } else { 0 };
            for j in first..ENTRIES {
                if i == 0 && j == 3 { continue; }      // shared APIC MMIO map
                let e3 = pdpt.add(j).read_volatile();
                if e3 & P_PRESENT == 0 { continue; }
                if e3 & P_PS != 0 { continue; } // 1 GiB leaf: not ours
                free_pd(e3);
            }
            pmm_free(e4 & ADDR_MASK); // the PDPT itself
        }
        pmm_free(pml4_pa);
    }
}

/// Free a PD, every PT under it and every mapped 4 KiB frame.
unsafe fn free_pd(pd_entry: u64) {
    let pd = table_of(pd_entry);
    for k in 0..ENTRIES {
        let e2 = pd.add(k).read_volatile();
        if e2 & P_PRESENT == 0 { continue; }
        if e2 & P_PS != 0 { continue; } // 2 MiB leaf: not ours
        let pt = table_of(e2);
        for m in 0..ENTRIES {
            let e1 = pt.add(m).read_volatile();
            if e1 & P_PRESENT != 0 {
                pmm_free(e1 & ADDR_MASK);
            }
        }
        pmm_free(e2 & ADDR_MASK); // the PT
    }
    pmm_free(pd_entry & ADDR_MASK); // the PD
}

/* ---- panics land in klog via kpanic.c ---- */

fn panic_rs(msg: &str) -> ! {
    let mut buf = [0u8; 192];
    let mut w = SliceWriter { buf: &mut buf, pos: 0 };
    let _ = w.write_str("paging.rs: ");
    let _ = w.write_str(msg);
    unsafe { rust_panic_msg(buf.as_ptr()) }
}

struct SliceWriter<'a> { buf: &'a mut [u8], pos: usize }

impl Write for SliceWriter<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let b = s.as_bytes();
        let n = core::cmp::min(b.len(), self.buf.len() - 1 - self.pos);
        self.buf[self.pos..self.pos + n].copy_from_slice(&b[..n]);
        self.pos += n;
        Ok(())
    }
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    let mut buf = [0u8; 192];
    let mut w = SliceWriter { buf: &mut buf, pos: 0 };
    let _ = w.write_str("paging.rs panic: ");
    let _ = w.write_fmt(format_args!("{}", info.message()));
    if let Some(loc) = info.location() {
        let _ = w.write_fmt(format_args!(" ({}:{})", loc.file(), loc.line()));
    }
    unsafe { rust_panic_msg(buf.as_ptr()) }
}
