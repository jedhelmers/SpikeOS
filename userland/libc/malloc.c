/*
 * Userland heap allocator for SpikeOS.
 *
 * First-fit free-list using a linear block chain (implicit free list).
 * Grows the heap via sbrk(). Modeled after the kernel heap allocator
 * in kernel/mm/heap.c, simplified for single-threaded userland use.
 */

#include "stdlib.h"
#include "unistd.h"
#include "string.h"

/* Alignment for all allocations (must be power of 2) */
#define ALIGN 8

/* Minimum sbrk increment (4KB = 1 page) */
#define SBRK_MIN 4096

/* Block header: tracks size, free status, and next physical block */
typedef struct block {
    unsigned int  size;     /* bytes of user data after header */
    unsigned int  free;     /* 1 = free, 0 = in use */
    struct block *next;     /* next physical block in chain */
} block_t;

/* Size of header, rounded up to alignment boundary */
#define HEADER_SIZE ((sizeof(block_t) + (ALIGN - 1)) & ~(ALIGN - 1))

/* Head of the linear block chain */
static block_t *heap_head = NULL;

/* Round up to alignment */
static unsigned int align_up(unsigned int n) {
    return (n + ALIGN - 1) & ~(ALIGN - 1);
}

/*
 * Request more memory from the kernel via sbrk().
 * Creates a new block at the end of the chain.
 */
static block_t *grow_heap(unsigned int size) {
    unsigned int total = HEADER_SIZE + size;
    if (total < SBRK_MIN) total = SBRK_MIN;

    void *ptr = sbrk((int)total);
    if (ptr == (void *)-1)
        return NULL;

    block_t *blk = (block_t *)ptr;
    blk->size = total - HEADER_SIZE;
    blk->free = 1;
    blk->next = NULL;

    /* Append to end of chain */
    if (!heap_head) {
        heap_head = blk;
    } else {
        block_t *cur = heap_head;
        while (cur->next) cur = cur->next;
        cur->next = blk;
    }

    return blk;
}

/*
 * Find a free block large enough for 'size' bytes (first-fit).
 */
static block_t *find_free(unsigned int size) {
    block_t *cur = heap_head;
    while (cur) {
        if (cur->free && cur->size >= size)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

/*
 * Split a block if the remainder can hold at least HEADER_SIZE + ALIGN bytes.
 */
static void split_block(block_t *blk, unsigned int size) {
    unsigned int leftover = blk->size - size;
    if (leftover >= HEADER_SIZE + ALIGN) {
        block_t *new_blk = (block_t *)((char *)blk + HEADER_SIZE + size);
        new_blk->size = leftover - HEADER_SIZE;
        new_blk->free = 1;
        new_blk->next = blk->next;
        blk->size = size;
        blk->next = new_blk;
    }
}

/*
 * Coalesce adjacent free blocks (forward merge).
 */
static void coalesce(void) {
    block_t *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void *malloc(size_t size) {
    if (size == 0) return NULL;

    unsigned int req = align_up((unsigned int)size);

    block_t *blk = find_free(req);
    if (!blk) {
        blk = grow_heap(req);
        if (!blk) return NULL;
    }

    split_block(blk, req);
    blk->free = 0;
    return (void *)((char *)blk + HEADER_SIZE);
}

void free(void *ptr) {
    if (!ptr) return;

    block_t *blk = (block_t *)((char *)ptr - HEADER_SIZE);

    /* Double-free guard */
    if (blk->free) return;

    blk->free = 1;
    coalesce();
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb)
        return NULL;

    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) { free(ptr); return NULL; }

    block_t *blk = (block_t *)((char *)ptr - HEADER_SIZE);
    unsigned int req = align_up((unsigned int)new_size);

    /* Already big enough? */
    if (blk->size >= req) {
        split_block(blk, req);
        return ptr;
    }

    /* Try to absorb next free block for in-place growth */
    if (blk->next && blk->next->free) {
        unsigned int combined = blk->size + HEADER_SIZE + blk->next->size;
        if (combined >= req) {
            blk->size = combined;
            blk->next = blk->next->next;
            split_block(blk, req);
            return ptr;
        }
    }

    /* Fallback: allocate new, copy, free old */
    void *new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, blk->size < new_size ? blk->size : new_size);
    free(ptr);
    return new_ptr;
}
