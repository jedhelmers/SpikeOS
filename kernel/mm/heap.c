/*
 * Kernel Heap Allocator
 *
 * First-fit free-list allocator for the SpikeOS kernel.
 * See kernel/include/kernel/heap.h for the public API and design overview.
 *
 * Internal structure:
 *   - heap_start: pointer to the first block header at HEAP_START
 *   - heap_end:   one-past-the-end of mapped heap virtual memory
 *   - free_list:  doubly-linked list of free blocks (head insertion)
 *
 * Blocks are physically contiguous in virtual memory. Each block is a
 * heap_block_t header followed by 'size' bytes of data. Walking all blocks
 * is done by stepping: next_block = (char*)(block+1) + block->size.
 */

#include <kernel/heap.h>
#include <kernel/paging.h>
#include <kernel/hal.h>
#include <string.h>
#include <stdio.h>

/* First block header in the heap (set after initial grow). */
static heap_block_t *heap_start = NULL;

/* One-past-the-end of currently mapped heap virtual memory. */
static uint32_t heap_end = HEAP_START;

/* Head of the doubly-linked free list. */
static heap_block_t *free_list = NULL;

/* Round sz up to the next multiple of HEAP_ALIGN (16). */
static inline uint32_t align_up(uint32_t sz) {
    return (sz + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);
}

/* Insert a block at the head of the free list and mark it free. */
static void free_list_insert(heap_block_t *blk) {
    blk->flags = HEAP_FLAG_FREE;
    blk->next  = free_list;
    blk->prev  = NULL;
    if (free_list) free_list->prev = blk;
    free_list = blk;
}

/* Remove a block from the free list and clear its free flag. */
static void free_list_remove(heap_block_t *blk) {
    if (blk->prev) blk->prev->next = blk->next;
    else           free_list       = blk->next;
    if (blk->next) blk->next->prev = blk->prev;
    blk->next  = NULL;
    blk->prev  = NULL;
    blk->flags = 0;
}

/*
 * Grow the heap by mapping 'pages' new physical frames at heap_end.
 * Creates a single free block spanning the newly mapped region.
 * If the last existing block is free, merges with it instead of
 * creating a separate block (avoids fragmentation at the boundary).
 *
 * Returns 0 on success, -1 on failure.
 */
static int heap_grow(uint32_t pages) {
    uint32_t new_bytes = pages * PAGE_SIZE;

    if (heap_end + new_bytes - HEAP_START > HEAP_MAX_SIZE) {
        printf("[heap] ERROR: would exceed HEAP_MAX_SIZE\n");
        return -1;
    }

    uint32_t grow_virt = heap_end;

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = alloc_frame();
        if (phys == FRAME_ALLOC_FAIL) {
            /* Rollback: free frames from pages we already mapped */
            for (uint32_t j = 0; j < i; j++) {
                uint32_t mapped_phys = virt_to_phys(grow_virt + j * PAGE_SIZE);
                if (mapped_phys) free_frame(mapped_phys);
            }
            printf("[heap] ERROR: alloc_frame() failed\n");
            return -1;
        }
        if (map_page(grow_virt + i * PAGE_SIZE, phys, PAGE_PRESENT | PAGE_WRITABLE) != 0) {
            free_frame(phys);
            for (uint32_t j = 0; j < i; j++) {
                uint32_t mapped_phys = virt_to_phys(grow_virt + j * PAGE_SIZE);
                if (mapped_phys) free_frame(mapped_phys);
            }
            printf("[heap] ERROR: map_page() failed\n");
            return -1;
        }
    }

    heap_end += new_bytes;

    /* Carve the newly mapped region into a single free block. */
    uint32_t data_bytes = new_bytes - sizeof(heap_block_t);
    heap_block_t *new_blk = (heap_block_t *)grow_virt;
    new_blk->size  = data_bytes;
    new_blk->flags = HEAP_FLAG_FREE;
    new_blk->next  = NULL;
    new_blk->prev  = NULL;

    /*
     * Try to merge with the last physical block if it is free.
     * Walk from heap_start to find the block whose data region
     * ends exactly at grow_virt.
     */
    if (heap_start != NULL) {
        heap_block_t *cursor = heap_start;
        heap_block_t *last = NULL;

        while ((uint32_t)cursor < grow_virt) {
            last = cursor;
            cursor = (heap_block_t *)((char *)(cursor + 1) + cursor->size);
        }

        if (last != NULL && (last->flags & HEAP_FLAG_FREE)) {
            /* Extend last block to absorb new_blk's header + data. */
            last->size += sizeof(heap_block_t) + new_blk->size;
            return 0;
        }
    }

    free_list_insert(new_blk);
    return 0;
}

void heap_init(void) {
    heap_start = NULL;
    heap_end   = HEAP_START;
    free_list  = NULL;

    if (heap_grow(HEAP_GROW_PAGES) != 0) {
        printf("[heap] FATAL: initial heap_grow failed\n");
        hal_halt_forever();
    }

    heap_start = (heap_block_t *)HEAP_START;
    printf("[heap] initialized at 0x%x, %u KiB\n",
           HEAP_START, (HEAP_GROW_PAGES * PAGE_SIZE) / 1024);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    uint32_t req = align_up((uint32_t)size);
    if (req < HEAP_ALIGN) req = HEAP_ALIGN;

    uint32_t irqflags = hal_irq_save();

    /* First-fit search: walk the free list for a large enough block. */
    heap_block_t *blk = free_list;
    while (blk != NULL) {
        if (blk->size >= req) goto found;
        blk = blk->next;
    }

    /* No fit — grow the heap and retry. */
    {
        uint32_t pages_needed = (req + sizeof(heap_block_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages_needed < HEAP_GROW_PAGES) pages_needed = HEAP_GROW_PAGES;

        if (heap_grow(pages_needed) != 0) {
            printf("[heap] kmalloc(%u): out of memory\n", (unsigned)size);
            hal_irq_restore(irqflags);
            return NULL;
        }
    }

    /* Retry after grow. */
    blk = free_list;
    while (blk != NULL) {
        if (blk->size >= req) goto found;
        blk = blk->next;
    }

    printf("[heap] kmalloc(%u): internal error after grow\n", (unsigned)size);
    hal_irq_restore(irqflags);
    return NULL;

found:
    /*
     * Split the block if the remainder is large enough to hold
     * a new header plus at least HEAP_ALIGN bytes of data.
     */
    {
        uint32_t leftover = blk->size - req;
        if (leftover >= sizeof(heap_block_t) + HEAP_ALIGN) {
            heap_block_t *split = (heap_block_t *)((char *)(blk + 1) + req);
            split->size  = leftover - sizeof(heap_block_t);
            split->flags = 0;
            split->next  = NULL;
            split->prev  = NULL;
            free_list_insert(split);
            blk->size = req;
        }
    }

    free_list_remove(blk);
    hal_irq_restore(irqflags);
    return (void *)(blk + 1);
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    heap_block_t *blk = (heap_block_t *)ptr - 1;

    /* Bounds check. */
    if ((uint32_t)blk < HEAP_START || (uint32_t)blk >= heap_end) {
        printf("[heap] kfree: pointer 0x%x outside heap!\n", (uint32_t)ptr);
        return;
    }

    /* Double-free detection. */
    if (blk->flags & HEAP_FLAG_FREE) {
        printf("[heap] kfree: double-free at 0x%x!\n", (uint32_t)ptr);
        return;
    }

    uint32_t irqflags = hal_irq_save();

    free_list_insert(blk);

    /*
     * Forward coalesce: if the next physical block is free, absorb it.
     * "Next physical" = the block starting right after blk's data region.
     */
    heap_block_t *next_phys = (heap_block_t *)((char *)(blk + 1) + blk->size);
    if ((uint32_t)next_phys < heap_end && (next_phys->flags & HEAP_FLAG_FREE)) {
        free_list_remove(next_phys);
        blk->size += sizeof(heap_block_t) + next_phys->size;
    }

    /*
     * Backward coalesce: walk from heap_start to find the block whose
     * data region ends exactly at blk. If that block is free, merge.
     * This is O(n) in block count but acceptable at hobby-OS scale.
     */
    if ((uint32_t)blk > HEAP_START) {
        heap_block_t *cursor = heap_start;
        heap_block_t *pred = NULL;

        while ((uint32_t)cursor < (uint32_t)blk) {
            pred = cursor;
            cursor = (heap_block_t *)((char *)(cursor + 1) + cursor->size);
        }

        if (pred != NULL && (pred->flags & HEAP_FLAG_FREE)) {
            free_list_remove(blk);
            pred->size += sizeof(heap_block_t) + blk->size;
        }
    }

    hal_irq_restore(irqflags);
}

void *kcalloc(size_t nmemb, size_t size) {
    /* Overflow check. */
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL)     return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    heap_block_t *blk = (heap_block_t *)ptr - 1;
    uint32_t old_size = blk->size;
    uint32_t req = align_up((uint32_t)new_size);

    /* Shrinking: split off the tail as a free block. */
    if (req <= old_size) {
        uint32_t leftover = old_size - req;
        if (leftover >= sizeof(heap_block_t) + HEAP_ALIGN) {
            heap_block_t *split = (heap_block_t *)((char *)(blk + 1) + req);
            split->size  = leftover - sizeof(heap_block_t);
            split->flags = 0;
            split->next  = NULL;
            split->prev  = NULL;
            blk->size = req;
            kfree((void *)(split + 1));
        }
        return ptr;
    }

    /* Growing: try in-place coalesce with next free block. */
    {
        uint32_t irqflags = hal_irq_save();
        heap_block_t *next_phys = (heap_block_t *)((char *)(blk + 1) + blk->size);
        if ((uint32_t)next_phys < heap_end && (next_phys->flags & HEAP_FLAG_FREE)) {
            uint32_t combined = blk->size + sizeof(heap_block_t) + next_phys->size;
            if (combined >= req) {
                free_list_remove(next_phys);
                blk->size = combined;
                hal_irq_restore(irqflags);
                return ptr;
            }
        }
        hal_irq_restore(irqflags);
    }

    /* Fallback: allocate new block, copy data, free old block. */
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    kfree(ptr);
    return new_ptr;
}

void heap_dump(void) {
    printf("[heap] start=0x%x end=0x%x (%u KiB mapped)\n",
           HEAP_START, heap_end, (heap_end - HEAP_START) / 1024);
    heap_block_t *blk = heap_start;
    int i = 0;
    while (blk && (uint32_t)blk < heap_end) {
        printf("  [%d] 0x%x  size=%u  %s\n",
               i++,
               (uint32_t)(blk + 1),
               blk->size,
               (blk->flags & HEAP_FLAG_FREE) ? "FREE" : "USED");
        blk = (heap_block_t *)((char *)(blk + 1) + blk->size);
    }
}
