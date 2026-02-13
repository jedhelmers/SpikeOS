#include <kernel/paging.h>
#include <stdint.h>

uint32_t page_directory[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
uint32_t first_page_table[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

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
        first_page_table[i] = (i * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITABLE;
    }

    page_directory[0] = ((uint32_t)first_page_table) | PAGE_PRESENT | PAGE_WRITABLE;

    for (int i = 1; i < PAGE_ENTRIES; i++) {
        page_directory[i] = 0;
    }
}