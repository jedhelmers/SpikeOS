#include <kernel/paging.h>
#include <kernel/process.h>
#include <kernel/isr.h>
#include <kernel/hal.h>
#include <kernel/signal.h>
#include <stdint.h>

uint32_t page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t first_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t second_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t third_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static uint32_t frame_bitmap[MAX_FRAMES / 32];

void frame_init() {
    for (uint32_t i = 0; i < MAX_FRAMES / 32; i++) {
        frame_bitmap[i] = 0;
    }
}

void reserve_region(uint32_t start, uint32_t end) {
    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t frame = addr / PAGE_SIZE;

        set_frame(frame);
    }
}

void paging_init() {
    extern uint32_t endkernel;

    frame_init();

    // endkernel is a higher-half VMA symbol; subtract KERNEL_VMA_OFFSET to
    // get the physical address of the kernel's end.
    uint32_t endkernel_phys = (uint32_t)&endkernel - KERNEL_VMA_OFFSET;

    // Reserve known used physical regions
    reserve_region(0x00000000, 0x00100000);                    // BIOS/low stuff
    reserve_region(0x00100000, endkernel_phys + 0x200000);     // kernel + 2 MiB margin

    // Zero page directory and our bootstrap tables
    memset(page_directory, 0, sizeof(page_directory));
    memset(first_page_table, 0, sizeof(first_page_table));
    // second_page_table can be used later for more mappings

    // Identity-map first 4 MiB physical → virtual 0x00000000+
    // (safe margin over your kernel size)
    for (uint32_t addr = 0; addr < 0x00400000; addr += PAGE_SIZE) {
        uint32_t pte_idx = addr >> 12;
        first_page_table[pte_idx] = addr | PAGE_PRESENT | PAGE_WRITABLE;
    }

    // PDEs must store PHYSICAL addresses. first_page_table has a higher-half
    // VMA, so subtract KERNEL_VMA_OFFSET to get its physical address.
    uint32_t fpt_phys = (uint32_t)first_page_table - KERNEL_VMA_OFFSET;

    // Set PD entries: same PT for low identity AND higher half
    page_directory[0]                = fpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    page_directory[KERNEL_PDE_INDEX] = fpt_phys | PAGE_PRESENT | PAGE_WRITABLE;

    // Pre-allocate PDE[769] for the kernel heap region (0xC0400000–0xC07FFFFF).
    // Using a statically-allocated page table avoids alloc_frame() before heap is ready.
    memset(second_page_table, 0, sizeof(second_page_table));
    uint32_t spt_phys = (uint32_t)second_page_table - KERNEL_VMA_OFFSET;
    page_directory[769] = spt_phys | PAGE_PRESENT | PAGE_WRITABLE;

    // Pre-allocate PDE[770] for the framebuffer region (0xC0800000–0xC0BFFFFF).
    memset(third_page_table, 0, sizeof(third_page_table));
    uint32_t tpt_phys = (uint32_t)third_page_table - KERNEL_VMA_OFFSET;
    page_directory[770] = tpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
}
uint32_t virt_to_phys(uint32_t virt) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;
    uint32_t offset   = virt & 0xFFF;

    uint32_t pde = page_directory[pd_index];

    if (!(pde & PAGE_PRESENT)) {
        printf("PDE not present!\n");
        return 0;
    }

    /* Use temp_map to access the page table by its physical address. */
    uint32_t pt_phys = pde & 0xFFFFF000;
    uint32_t *pt = (uint32_t *)temp_map(pt_phys);
    uint32_t pte = pt[pt_index];
    temp_unmap();

    if (!(pte & PAGE_PRESENT)) {
        printf("PTE not present!\n");
        return 0;
    }

    uint32_t phys_base = pte & 0xFFFFF000;

    return phys_base + offset;
}

void set_frame(uint32_t frame) {
    frame_bitmap[frame / 32] |= (1 << (frame % 32));
}

void clear_frame(uint32_t frame) {
    frame_bitmap[frame / 32] &= ~(1 << (frame % 32));
}

int test_frame(uint32_t frame) {
    return frame_bitmap[frame / 32] & (1 << (frame % 32));
}

uint32_t alloc_frame() {
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (!test_frame(i)) {
            set_frame(i);

            return i * FRAME_SIZE;
        }
    }

    return FRAME_ALLOC_FAIL;
}

void free_frame(uint32_t phys) {
    uint32_t frame = phys / FRAME_SIZE;
    clear_frame(frame);
}

uint32_t alloc_frames_contiguous(uint32_t count, uint32_t align_frames) {
    if (count == 0) return FRAME_ALLOC_FAIL;
    if (align_frames == 0) align_frames = 1;

    uint32_t flags = hal_irq_save();

    /* Scan for a run of 'count' free frames starting at an aligned boundary */
    for (uint32_t start = 0; start <= MAX_FRAMES - count; ) {
        /* Align start to the requested boundary */
        uint32_t rem = start % align_frames;
        if (rem != 0) start += align_frames - rem;
        if (start + count > MAX_FRAMES) break;

        /* Check if all 'count' frames from 'start' are free */
        uint32_t run = 0;
        for (run = 0; run < count; run++) {
            if (test_frame(start + run)) break;
        }

        if (run == count) {
            /* Found a contiguous run — mark all frames as used */
            for (uint32_t i = 0; i < count; i++)
                set_frame(start + i);
            hal_irq_restore(flags);
            return start * FRAME_SIZE;
        }

        /* Skip past the occupied frame */
        start += run + 1;
    }

    hal_irq_restore(flags);
    return FRAME_ALLOC_FAIL;
}

void free_frames_contiguous(uint32_t phys, uint32_t count) {
    uint32_t frame = phys / FRAME_SIZE;
    uint32_t flags = hal_irq_save();
    for (uint32_t i = 0; i < count; i++)
        clear_frame(frame + i);
    hal_irq_restore(flags);
}

/*
 * Temp mapping: map any physical frame at TEMP_MAP_VADDR (0xC03FF000).
 * Uses PTE[1023] of first_page_table. Since first_page_table is in
 * kernel BSS (physical < 4MB, identity-mapped), we can write to it directly.
 *
 * Interrupts are disabled for the duration of a temp mapping to prevent
 * re-entrancy (only one temp slot exists). temp_unmap restores the
 * previous interrupt state.
 */
#define TEMP_MAP_PTE_INDEX 1023

static uint32_t temp_map_irq_flags;

void *temp_map(uint32_t phys_frame) {
    temp_map_irq_flags = hal_irq_save();
    first_page_table[TEMP_MAP_PTE_INDEX] = phys_frame | PAGE_PRESENT | PAGE_WRITABLE;
    hal_tlb_invalidate(TEMP_MAP_VADDR);
    return (void *)TEMP_MAP_VADDR;
}

void temp_unmap(void) {
    first_page_table[TEMP_MAP_PTE_INDEX] = 0;
    hal_tlb_invalidate(TEMP_MAP_VADDR);
    hal_irq_restore(temp_map_irq_flags);
}

/*
 * Allocate a new page directory that clones the kernel's PDEs.
 * Returns the physical address, or 0 on failure.
 */
uint32_t pgdir_create(void) {
    uint32_t pd_phys = alloc_frame();
    if (pd_phys == FRAME_ALLOC_FAIL) return 0;

    uint32_t *pd = (uint32_t *)temp_map(pd_phys);
    memcpy(pd, page_directory, PAGE_SIZE);
    temp_unmap();

    return pd_phys;
}

/*
 * Destroy a per-process page directory.
 * Frees user page tables (PDEs 1-767) and their mapped frames,
 * then frees the PD frame itself. Skips shared kernel page tables.
 */
void pgdir_destroy(uint32_t pd_phys) {
    if (pd_phys == 0) return;

    uint32_t fpt_phys = (uint32_t)first_page_table - KERNEL_VMA_OFFSET;
    uint32_t spt_phys = (uint32_t)second_page_table - KERNEL_VMA_OFFSET;
    uint32_t tpt_phys = (uint32_t)third_page_table - KERNEL_VMA_OFFSET;

    for (int i = 1; i < 768; i++) {
        /* Temp-map PD, read one PDE, temp-unmap */
        uint32_t *pd = (uint32_t *)temp_map(pd_phys);
        uint32_t pde = pd[i];
        temp_unmap();

        if (!(pde & PAGE_PRESENT)) continue;

        uint32_t pt_phys = pde & 0xFFFFF000;

        /* Skip shared kernel page tables */
        if (pt_phys == fpt_phys || pt_phys == spt_phys || pt_phys == tpt_phys) continue;

        /* Free all frames referenced by this page table */
        for (int j = 0; j < PAGE_ENTRIES; j++) {
            uint32_t *pt = (uint32_t *)temp_map(pt_phys);
            uint32_t pte = pt[j];
            temp_unmap();

            if (pte & PAGE_PRESENT) {
                free_frame(pte & 0xFFFFF000);
            }
        }

        /* Free the page table frame itself */
        free_frame(pt_phys);
    }

    /* Also check for cloned kernel page tables in PDEs 768+ */
    for (int i = 768; i < PAGE_ENTRIES; i++) {
        uint32_t *pd = (uint32_t *)temp_map(pd_phys);
        uint32_t pde = pd[i];
        temp_unmap();

        if (!(pde & PAGE_PRESENT)) continue;

        uint32_t pt_phys = pde & 0xFFFFF000;

        /* Only free if it's NOT a shared kernel page table */
        if (pt_phys != fpt_phys && pt_phys != spt_phys && pt_phys != tpt_phys) {
            /* This is a cloned kernel PT — free the clone but NOT the frames
               (they're kernel frames, still in use by the kernel's PD) */
            free_frame(pt_phys);
        }
    }

    free_frame(pd_phys);
}

/*
 * Map a single page in a per-process page directory.
 * If the PDE points to a shared kernel page table and PAGE_USER is
 * requested, clones the page table to avoid modifying the kernel's tables.
 * Returns 0 on success, -1 on failure.
 */
int pgdir_map_user_page(uint32_t pd_phys, uint32_t virt, uint32_t phys,
                        uint32_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    uint32_t fpt_phys = (uint32_t)first_page_table - KERNEL_VMA_OFFSET;
    uint32_t spt_phys = (uint32_t)second_page_table - KERNEL_VMA_OFFSET;
    uint32_t tpt_phys = (uint32_t)third_page_table - KERNEL_VMA_OFFSET;

    /* Read the PDE */
    uint32_t *pd = (uint32_t *)temp_map(pd_phys);
    uint32_t pde = pd[pd_index];

    if (!(pde & PAGE_PRESENT)) {
        /* Allocate a new page table */
        uint32_t pt_phys = alloc_frame();
        if (pt_phys == FRAME_ALLOC_FAIL) { temp_unmap(); return -1; }

        pd[pd_index] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);
        temp_unmap();

        /* Zero the new page table and write the PTE */
        uint32_t *pt = (uint32_t *)temp_map(pt_phys);
        memset(pt, 0, PAGE_SIZE);
        pt[pt_index] = phys | flags;
        temp_unmap();
        return 0;
    }

    uint32_t pt_phys = pde & 0xFFFFF000;

    /* If PDE points to a shared kernel PT and we need PAGE_USER, clone it */
    if ((flags & PAGE_USER) &&
        (pt_phys == fpt_phys || pt_phys == spt_phys || pt_phys == tpt_phys)) {
        uint32_t new_pt_phys = alloc_frame();
        if (new_pt_phys == FRAME_ALLOC_FAIL) { temp_unmap(); return -1; }

        /* Update the PDE to point to the clone */
        pd[pd_index] = new_pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        temp_unmap();

        /* Copy from the original kernel PT (accessible via identity map) */
        uint32_t *orig_pt = (pt_phys == fpt_phys) ? first_page_table
                          : (pt_phys == spt_phys)  ? second_page_table
                                                   : third_page_table;

        uint32_t *new_pt = (uint32_t *)temp_map(new_pt_phys);
        memcpy(new_pt, orig_pt, PAGE_SIZE);
        new_pt[pt_index] = phys | flags;
        temp_unmap();
        return 0;
    }

    /* PDE present and not a shared kernel PT — just write the PTE */
    temp_unmap(); /* unmap PD */

    uint32_t *pt = (uint32_t *)temp_map(pt_phys);
    pt[pt_index] = phys | flags;
    temp_unmap();
    return 0;
}

int map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PAGE_PRESENT)) {
        uint32_t new_table = alloc_frame();
        if (new_table == FRAME_ALLOC_FAIL) return -1;
        page_directory[pd_index] = new_table | PAGE_PRESENT | PAGE_WRITABLE;

        /* Use temp_map to zero the new page table — the physical address
           may be above 4MB and not identity-mapped. */
        uint32_t *pt = (uint32_t *)temp_map(new_table);
        memset(pt, 0, PAGE_SIZE);
        temp_unmap();
    }

    /* Use temp_map to access the page table by its physical address. */
    uint32_t pt_phys = page_directory[pd_index] & 0xFFFFF000;
    uint32_t *table = (uint32_t *)temp_map(pt_phys);
    table[pt_index] = phys | flags;
    temp_unmap();

    hal_tlb_invalidate(virt);
    return 0;
}

/*
 * Track which PDE slots have been claimed for MMIO.
 * mmio_next_pde starts at MMIO_PDE_START and advances as regions are mapped.
 */
static int mmio_next_pde = MMIO_PDE_START;

int map_mmio_region(uint32_t phys_base, uint32_t size, uint32_t *virt_out) {
    if (size == 0 || !virt_out) return -1;

    /* Page-align physical base (preserve sub-page offset for caller) */
    uint32_t phys_aligned = phys_base & ~0xFFFu;
    uint32_t offset_in_page = phys_base & 0xFFFu;
    uint32_t total_bytes = size + offset_in_page;

    /* Calculate how many pages (and PDEs) we need */
    uint32_t num_pages = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t num_pdes  = (num_pages + PAGE_ENTRIES - 1) / PAGE_ENTRIES;

    /* Check we have enough PDE slots */
    if (mmio_next_pde + (int)num_pdes > PAGE_ENTRIES) return -1;

    uint32_t virt_base = (uint32_t)mmio_next_pde << 22;

    /* Map all pages */
    uint32_t phys = phys_aligned;
    uint32_t virt = virt_base;
    for (uint32_t i = 0; i < num_pages; i++) {
        if (map_page(virt, phys,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE) != 0) {
            return -1;
        }
        virt += PAGE_SIZE;
        phys += PAGE_SIZE;
    }

    /* Advance the PDE cursor past the claimed slots */
    mmio_next_pde += num_pdes;

    *virt_out = virt_base + offset_in_page;
    return 0;
}

/*
 * Page fault handler (ISR 14).
 *
 * Error code bits (pushed by CPU):
 *   bit 0: 0 = not-present fault, 1 = protection violation
 *   bit 1: 0 = read, 1 = write
 *   bit 2: 0 = kernel mode, 1 = user mode
 *
 * CR2 holds the faulting linear address.
 */
void page_fault_handler(trapframe *tf) {
    uint32_t fault_addr = hal_get_fault_addr();

    int present  = tf->err_code & 0x1;
    int write    = tf->err_code & 0x2;
    int user     = tf->err_code & 0x4;

    if (user) {
        /* User-mode page fault: send SIGSEGV */
        printf("\n[PAGE FAULT] PID %d: %s %s at 0x%x (EIP=0x%x)\n",
               current_process->pid,
               write ? "write" : "read",
               present ? "protection violation" : "not-present page",
               fault_addr, tf->eip);

        proc_signal(current_process->pid, SIGSEGV);
        signal_check_pending();

        /* If still running (shouldn't be), halt */
        hal_irq_enable();
        for (;;) hal_halt();
    }

    /* Kernel-mode page fault — unrecoverable */
    printf("\n[KERNEL PAGE FAULT] %s %s at 0x%x\n",
           write ? "write" : "read",
           present ? "protection violation" : "not-present page",
           fault_addr);
    printf("EIP=0x%x CS=0x%x EFLAGS=0x%x\n", tf->eip, tf->cs, tf->eflags);
    printf("EAX=0x%x EBX=0x%x ECX=0x%x EDX=0x%x\n",
           tf->eax, tf->ebx, tf->ecx, tf->edx);
    printf("ESP=0x%x EBP=0x%x ESI=0x%x EDI=0x%x\n",
           tf->esp_dummy, tf->ebp, tf->esi, tf->edi);

    hal_halt_forever();
}
