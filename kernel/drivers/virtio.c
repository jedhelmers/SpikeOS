/*
 * VirtIO transport layer for SpikeOS.
 *
 * Implements virtqueue allocation, descriptor management, and
 * the available/used ring protocol. Used by device-specific drivers
 * (virtio_gpu.c, etc.).
 */

#include <kernel/virtio.h>
#include <kernel/paging.h>
#include <kernel/heap.h>
#include <string.h>

/*
 * Virtqueue memory layout (contiguous physical allocation):
 *
 *   [descriptor table]  size * 16 bytes
 *   [available ring]    6 + size * 2 bytes  (flags, idx, ring[size])
 *   [padding to 4K]
 *   [used ring]         6 + size * 8 bytes  (flags, idx, ring[size])
 *
 * All three structures must be in guest-physical memory accessible by
 * the device via DMA.
 */

int virtq_init(virtq_t *vq, uint16_t size) {
    memset(vq, 0, sizeof(*vq));
    vq->size = size;

    /* Calculate sizes for each region */
    uint32_t desc_size  = (uint32_t)size * sizeof(virtq_desc_t);
    uint32_t avail_size = sizeof(uint16_t) * (3 + size);  /* flags + idx + ring[size] */

    /* Available ring must follow descriptors, then pad to PAGE_SIZE for used ring */
    uint32_t avail_end  = desc_size + avail_size;
    uint32_t used_start = (avail_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t used_size  = sizeof(uint16_t) * 3 + sizeof(virtq_used_elem_t) * size;
    uint32_t total      = used_start + used_size;

    /* Allocate contiguous physical pages */
    uint32_t num_pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t phys = alloc_frames_contiguous(num_pages, 1);
    if (phys == FRAME_ALLOC_FAIL) return -1;

    /* Map into kernel VA so we can access it */
    uint32_t virt;
    if (map_mmio_region(phys, total, &virt) != 0) {
        free_frames_contiguous(phys, num_pages);
        return -1;
    }

    /* Zero everything */
    memset((void *)virt, 0, total);

    /* Set up pointers */
    vq->desc  = (virtq_desc_t *)virt;
    vq->avail = (virtq_avail_t *)(virt + desc_size);
    vq->used  = (virtq_used_t *)(virt + used_start);

    vq->desc_phys  = phys;
    vq->avail_phys = phys + desc_size;
    vq->used_phys  = phys + used_start;

    /* Initialize free descriptor list: chain all descriptors together */
    vq->free_head = 0;
    vq->num_free  = size;
    for (uint16_t i = 0; i < size - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[size - 1].next = 0xFFFF;

    vq->last_used = 0;

    return 0;
}

void virtq_destroy(virtq_t *vq) {
    if (!vq->desc) return;
    uint32_t desc_size  = (uint32_t)vq->size * sizeof(virtq_desc_t);
    uint32_t avail_size = sizeof(uint16_t) * (3 + vq->size);
    uint32_t avail_end  = desc_size + avail_size;
    uint32_t used_start = (avail_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint32_t used_size  = sizeof(uint16_t) * 3 + sizeof(virtq_used_elem_t) * vq->size;
    uint32_t total      = used_start + used_size;
    uint32_t num_pages  = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    free_frames_contiguous(vq->desc_phys, num_pages);
    memset(vq, 0, sizeof(*vq));
}

uint16_t virtq_alloc_desc(virtq_t *vq) {
    if (vq->num_free == 0) return 0xFFFF;
    uint16_t idx = vq->free_head;
    vq->free_head = vq->desc[idx].next;
    vq->num_free--;
    vq->desc[idx].next = 0xFFFF;
    vq->desc[idx].flags = 0;
    return idx;
}

void virtq_free_desc(virtq_t *vq, uint16_t idx) {
    vq->desc[idx].addr = 0;
    vq->desc[idx].len = 0;
    vq->desc[idx].flags = 0;
    vq->desc[idx].next = vq->free_head;
    vq->free_head = idx;
    vq->num_free++;
}

void virtq_submit(virtq_t *vq, uint16_t head) {
    uint16_t avail_idx = vq->avail->idx;
    vq->avail->ring[avail_idx % vq->size] = head;
    /* Memory barrier: ensure ring entry is visible before updating idx */
    __asm__ volatile("" ::: "memory");
    vq->avail->idx = avail_idx + 1;
}

int virtq_has_used(virtq_t *vq) {
    /* Memory barrier before reading used->idx */
    __asm__ volatile("" ::: "memory");
    return vq->last_used != vq->used->idx;
}

uint16_t virtq_pop_used(virtq_t *vq, uint32_t *len_out) {
    if (!virtq_has_used(vq)) return 0xFFFF;
    uint16_t idx = vq->last_used % vq->size;
    uint32_t id  = vq->used->ring[idx].id;
    if (len_out) *len_out = vq->used->ring[idx].len;
    vq->last_used++;
    return (uint16_t)id;
}
