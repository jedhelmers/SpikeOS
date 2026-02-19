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
    frame_bitmap[frame / 32] &= (1 << (frame % 32));
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

void map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FF;

    if (!page_directory[pd_index] & PAGE_PRESENT) {
        uint32_t new_table = alloc_frame();
        page_directory[pd_index] = new_table | PAGE_PRESENT | PAGE_WRITABLE;

        memset((void*)new_table, 0, PAGE_SIZE);
    }

    uint32_t* table = (uint32_t*)(page_directory[pd_index] & 0xFFFFF000);
    table[pt_index] = phys | flags;

    asm("invlpg (%0)" :: "r"(virt) : "memory");
}
