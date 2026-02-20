#include <kernel/paging.h>
#include <stdint.h>

uint32_t page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t first_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t second_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

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
    // Using the statically-allocated second_page_table avoids the map_page()
    // new-table-allocation path, which has issues with physical vs virtual addresses.
    memset(second_page_table, 0, sizeof(second_page_table));
    uint32_t spt_phys = (uint32_t)second_page_table - KERNEL_VMA_OFFSET;
    page_directory[769] = spt_phys | PAGE_PRESENT | PAGE_WRITABLE;

    // Optional: map page dir itself recursively (helps later for dynamic mapping)
    // page_directory[1023] = (uint32_t)page_directory | PAGE_PRESENT | PAGE_WRITABLE;
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

    uint32_t* pt = (uint32_t*)(pde & 0xFFFFF000);

    uint32_t pte = pt[pt_index];

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

    return 0;
}

void free_frame(uint32_t phys) {
    uint32_t frame = phys / FRAME_SIZE;
    clear_frame(frame);
}

/*
 * Temp mapping: map any physical frame at TEMP_MAP_VADDR (0xC03FF000).
 * Uses PTE[1023] of first_page_table. Since first_page_table is in
 * kernel BSS (physical < 4MB, identity-mapped), we can write to it directly.
 */
#define TEMP_MAP_PTE_INDEX 1023

void *temp_map(uint32_t phys_frame) {
    first_page_table[TEMP_MAP_PTE_INDEX] = phys_frame | PAGE_PRESENT | PAGE_WRITABLE;
    asm volatile("invlpg (%0)" :: "r"(TEMP_MAP_VADDR) : "memory");
    return (void *)TEMP_MAP_VADDR;
}

void temp_unmap(void) {
    first_page_table[TEMP_MAP_PTE_INDEX] = 0;
    asm volatile("invlpg (%0)" :: "r"(TEMP_MAP_VADDR) : "memory");
}

/*
 * Allocate a new page directory that clones the kernel's PDEs.
 * Returns the physical address, or 0 on failure.
 */
uint32_t pgdir_create(void) {
    uint32_t pd_phys = alloc_frame();
    if (pd_phys == 0) return 0;

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

    for (int i = 1; i < 768; i++) {
        /* Temp-map PD, read one PDE, temp-unmap */
        uint32_t *pd = (uint32_t *)temp_map(pd_phys);
        uint32_t pde = pd[i];
        temp_unmap();

        if (!(pde & PAGE_PRESENT)) continue;

        uint32_t pt_phys = pde & 0xFFFFF000;

        /* Skip shared kernel page tables */
        if (pt_phys == fpt_phys || pt_phys == spt_phys) continue;

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
        if (pt_phys != fpt_phys && pt_phys != spt_phys) {
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

    /* Read the PDE */
    uint32_t *pd = (uint32_t *)temp_map(pd_phys);
    uint32_t pde = pd[pd_index];

    if (!(pde & PAGE_PRESENT)) {
        /* Allocate a new page table */
        uint32_t pt_phys = alloc_frame();
        if (pt_phys == 0) { temp_unmap(); return -1; }

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
        (pt_phys == fpt_phys || pt_phys == spt_phys)) {
        uint32_t new_pt_phys = alloc_frame();
        if (new_pt_phys == 0) { temp_unmap(); return -1; }

        /* Update the PDE to point to the clone */
        pd[pd_index] = new_pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        temp_unmap();

        /* Copy from the original kernel PT (accessible via identity map) */
        uint32_t *orig_pt = (pt_phys == fpt_phys) ? first_page_table
                                                   : second_page_table;

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

void map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PAGE_PRESENT)) {
        uint32_t new_table = alloc_frame();
        page_directory[pd_index] = new_table | PAGE_PRESENT | PAGE_WRITABLE;

        memset((void*)new_table, 0, PAGE_SIZE);
    }

    uint32_t* table = (uint32_t*)(page_directory[pd_index] & 0xFFFFF000);
    table[pt_index] = phys | flags;

    asm("invlpg (%0)" :: "r"(virt) : "memory");
}
