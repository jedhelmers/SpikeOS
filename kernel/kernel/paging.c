#include <kernel/paging.h>
#include <stdint.h>

uint32_t page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t first_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t second_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

void paging_init() {
    /*
        Map first 4MB
        We need this to map the kernel's starting physical address.
        Map it or die.

        Maps from:
            0x00000000 - 0x003FFFFF
        to:
            0x00000000 - 0x003FFFFF
    */

    for (int i = 0; i < PAGE_ENTRIES; i++) {
        first_page_table[i] = (i * PAGE_SIZE)| PAGE_PRESENT | PAGE_WRITABLE;
    }

    for (int i = 0; i < PAGE_ENTRIES; i++) {
        second_page_table[i] = (i * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITABLE;
    }

    for (int i = 1; i < PAGE_ENTRIES; i++) {
        page_directory[i] = 0;
    }

    page_directory[0] = ((uint32_t)first_page_table) | PAGE_PRESENT | PAGE_WRITABLE;
    page_directory[1] = ((uint32_t)second_page_table) | PAGE_PRESENT | PAGE_WRITABLE;
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