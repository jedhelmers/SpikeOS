#ifndef HEAP_H
#define HEAP_H

/*
 * Kernel Heap Allocator
 *
 * First-fit free-list allocator with block splitting and coalescing.
 * The heap occupies virtual addresses starting at HEAP_START (0xC0400000,
 * PDE[769]) and grows on demand by mapping physical frames via
 * alloc_frame() + map_page().
 *
 * Memory layout per allocation:
 *   [heap_block_t header (16 bytes)][user data (aligned to 16 bytes)]
 *
 * The returned pointer from kmalloc points to the user data region,
 * immediately after the header. kfree recovers the header by subtracting
 * sizeof(heap_block_t) from the pointer.
 *
 * Interrupt safety: kmalloc/kfree disable interrupts around critical
 * sections to prevent corruption from concurrent scheduler ticks.
 */

#include <stdint.h>
#include <stddef.h>

/* Virtual address where the kernel heap starts (PDE[769]). */
#define HEAP_START      0xC0400000u

/* Maximum heap size in pages (4 MiB). */
#define HEAP_MAX_PAGES  1024u
#define HEAP_MAX_SIZE   (HEAP_MAX_PAGES * 0x1000u)

/* Pages allocated per heap_grow() call. */
#define HEAP_GROW_PAGES 4u

/* All returned pointers are 16-byte aligned. */
#define HEAP_ALIGN      16u

/*
 * Block header — 16 bytes, embedded immediately before each allocation.
 * Both free and in-use blocks carry this header. Free blocks are linked
 * into a doubly-linked free list via next/prev.
 */
typedef struct heap_block {
    uint32_t           size;    /* bytes of DATA following this header */
    uint32_t           flags;   /* HEAP_FLAG_FREE if block is free     */
    struct heap_block *next;    /* free-list next (NULL if in use)     */
    struct heap_block *prev;    /* free-list prev (NULL if in use)     */
} heap_block_t;

#define HEAP_FLAG_FREE  0x1u

/*
 * Initialize the kernel heap. Maps HEAP_GROW_PAGES pages at HEAP_START
 * and creates an initial free block. Must be called after paging_enable()
 * and before any kmalloc calls.
 */
void  heap_init(void);

/*
 * Allocate 'size' bytes from the kernel heap.
 * Returns a 16-byte aligned pointer, or NULL on failure.
 * The heap grows automatically if no free block can satisfy the request.
 */
void *kmalloc(size_t size);

/*
 * Free a previously allocated pointer. Coalesces adjacent free blocks
 * (both forward and backward) to reduce fragmentation.
 * Passing NULL is a no-op. Double-free is detected and logged.
 */
void  kfree(void *ptr);

/*
 * Allocate zero-initialized memory for an array of nmemb elements,
 * each of 'size' bytes. Returns NULL on overflow or allocation failure.
 */
void *kcalloc(size_t nmemb, size_t size);

/*
 * Resize a previously allocated block to new_size bytes.
 * - If ptr is NULL, equivalent to kmalloc(new_size).
 * - If new_size is 0, equivalent to kfree(ptr); returns NULL.
 * - Shrinking may split the block. Growing tries in-place coalesce
 *   with the next free block before falling back to alloc-copy-free.
 */
void *krealloc(void *ptr, size_t new_size);

/*
 * Print a dump of all heap blocks (address, size, free/used status)
 * to the terminal via printf. Useful for debugging and the 'meminfo'
 * shell command.
 */
void  heap_dump(void);

#endif
