#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/*
    31                      12 11            0
    +------------------------+----------------+
    |  Physical Address      |   Flags       |
    +------------------------+----------------+
*/

/*
    Page constants
*/
#define PAGE_SIZE 0x1000
#define PAGE_ENTRIES 1024

/*
    Page entry flags
*/
#define PAGE_PRESENT         0x1
#define PAGE_WRITABLE        0x2
#define PAGE_USER            0x4
#define PAGE_WRITE_THROUGH   0x8 
#define PAGE_CACHE_DISABLE   0x10 
#define PAGE_ACCESSED        0x20
#define PAGE_DIRTY           0x40
#define PAGE_4MB             0x80
#define PAGE_GLOBAL          0x100

/*
    Page directory and first page table
    (defined in paging.c)
*/
extern uint32_t page_directory[PAGE_ENTRIES];
extern uint32_t first_page_table[PAGE_ENTRIES];

/*
    Initializes paging structures
    (creates identity mapping for first 4MB)
*/
void paging_init(void);

/*
    Enables paging (implemented in paging_enable.S)
*/
void paging_enable(uint32_t page_directory_address);

#endif