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
    Temp mapping window — maps any physical frame at a fixed kernel VA.
    Uses PTE[1023] of first_page_table.
    Not reentrant: only one temp mapping active at a time.
*/
#define TEMP_MAP_VADDR 0xC03FF000u

void *temp_map(uint32_t phys_frame);
void  temp_unmap(void);

/*
    Sentinel value for alloc_frame() failure (out of memory).
    Distinct from physical address 0x0 (which is a valid reserved frame).
*/
#define FRAME_ALLOC_FAIL ((uint32_t)-1)

/*
    Page Frame Allocation
*/
void frame_init();

void reserve_region(uint32_t start, uint32_t end);

void set_frame(uint32_t frame);

void clear_frame(uint32_t frame);

int test_frame(uint32_t frame);

uint32_t alloc_frame(void);

void free_frame(uint32_t phys);

/*
    Allocate 'count' physically contiguous frames aligned to 'align_frames'
    frame boundary (e.g., align_frames=1 for page-aligned, 4 for 16KB-aligned).
    Returns the physical address of the first frame, or FRAME_ALLOC_FAIL on failure.
*/
uint32_t alloc_frames_contiguous(uint32_t count, uint32_t align_frames);

/*
    Free 'count' contiguous frames starting at physical address 'phys'.
*/
void free_frames_contiguous(uint32_t phys, uint32_t count);

int map_page(uint32_t virt, uint32_t phys, uint32_t flags);

/*
    Map a device MMIO region into kernel virtual address space.
    Automatically selects a free PDE slot (starting from PDE[773]),
    maps 'size' bytes from 'phys_base' with PAGE_CACHE_DISABLE.
    On success, writes the kernel virtual address to *virt_out and returns 0.
    On failure (no free PDE or map_page failed), returns -1.
    Note: PDE[772] is reserved for the compositor buffer (COMPOSITOR_VIRT_BASE).
*/
#define MMIO_PDE_START 773  /* first PDE available for dynamic MMIO mapping */
int map_mmio_region(uint32_t phys_base, uint32_t size, uint32_t *virt_out);

/*
    Per-process page directory management
*/
uint32_t pgdir_create(void);
void     pgdir_destroy(uint32_t pd_phys);
int      pgdir_map_user_page(uint32_t pd_phys, uint32_t virt, uint32_t phys, uint32_t flags);

/*
    Page fault handler (ISR 14)
    Called from isr_common_handler. Returns 0 if handled, -1 to kill/halt.
*/
struct trapframe;
void page_fault_handler(struct trapframe *tf);

#endif