#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/*
    31                      12 11            0
    +------------------------+----------------+
    |  Physical Address      |   Flags       |
    +------------------------+----------------+
*/

/*
    Higher Half
*/
#define HIGHER_HALF_BASE   0xC0000000u
/* VMA = phys + KERNEL_VMA_OFFSET; phys = VMA - KERNEL_VMA_OFFSET */
#define KERNEL_VMA_OFFSET  0xC0000000u
#define KERNEL_PDE_INDEX   (HIGHER_HALF_BASE >> 22)  /* 768 = 0x300 */

/*
    Page constants
*/
#define PAGE_SIZE 0x1000
#define PAGE_ENTRIES 1024

/*
    Frame constants
*/
#define MAX_FRAMES 16384
#define FRAME_SIZE 4096

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
extern uint32_t second_page_table[PAGE_ENTRIES];

/*
    Initializes paging structures
    (creates identity mapping for first 4MB)
*/
void paging_init(void);

/*
    Enables paging (implemented in paging_enable.S)
*/
void paging_enable(uint32_t page_directory_address);

/*
    Virtual to Physical - for debugging
*/
uint32_t virt_to_phys(uint32_t virt);

/*
    Page Frame Allocation
*/
void frame_init();

void reserve_region(uint32_t start, uint32_t end);

void set_frame(uint32_t frame);

void clear_frame(uint32_t frame);

int test_frame(uint32_t frame);

uint32_t alloc_frame();

void map_page(uint32_t virt, uint32_t phys, uint32_t flags);

#endif