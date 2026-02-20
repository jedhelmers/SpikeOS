#include <kernel/initrd.h>
#include <kernel/paging.h>
#include <kernel/heap.h>
#include <string.h>
#include <stdio.h>

static uint32_t initrd_phys_start = 0;
static uint32_t initrd_phys_end   = 0;
static uint32_t initrd_num_files  = 0;
static struct initrd_file_entry *initrd_files = NULL;  /* heap-allocated copy */

int initrd_init(uint32_t phys_start, uint32_t phys_end) {
    initrd_phys_start = phys_start;
    initrd_phys_end   = phys_end;

    /* Reserve physical frames so alloc_frame won't reuse them */
    reserve_region(phys_start, phys_end);

    /* Read the header from the first page of the initrd */
    uint32_t page_base = phys_start & ~0xFFFu;
    uint32_t page_off  = phys_start & 0xFFFu;

    uint8_t *mapped = (uint8_t *)temp_map(page_base);

    struct initrd_header *hdr = (struct initrd_header *)(mapped + page_off);

    if (hdr->magic != INITRD_MAGIC) {
        temp_unmap();
        printf("[initrd] bad magic: 0x%x\n", hdr->magic);
        return -1;
    }

    uint32_t nfiles = hdr->num_files;
    initrd_num_files = nfiles;

    if (nfiles == 0) {
        temp_unmap();
        printf("[initrd] 0 files\n");
        return 0;
    }

    /* The file entry table starts right after the header */
    uint32_t entries_size = nfiles * sizeof(struct initrd_file_entry);

    /* Allocate kernel-heap copy of the file entries */
    initrd_files = (struct initrd_file_entry *)kmalloc(entries_size);
    if (!initrd_files) {
        temp_unmap();
        printf("[initrd] kmalloc failed\n");
        return -1;
    }

    /*
     * The entries start at phys_start + sizeof(initrd_header).
     * They may span a page boundary, so copy carefully.
     */
    uint32_t entries_phys = phys_start + sizeof(struct initrd_header);
    temp_unmap();  /* done with first page for now */

    /* Copy entries byte-by-byte across page boundaries */
    uint8_t *dst = (uint8_t *)initrd_files;
    uint32_t remaining = entries_size;
    uint32_t src_phys = entries_phys;

    while (remaining > 0) {
        page_base = src_phys & ~0xFFFu;
        page_off  = src_phys & 0xFFFu;
        uint32_t chunk = 0x1000 - page_off;
        if (chunk > remaining) chunk = remaining;

        mapped = (uint8_t *)temp_map(page_base);
        memcpy(dst, mapped + page_off, chunk);
        temp_unmap();

        dst      += chunk;
        src_phys += chunk;
        remaining -= chunk;
    }

    printf("[initrd] %d file(s) loaded\n", nfiles);
    return 0;
}

int initrd_find(const char *name, uint32_t *out_phys, uint32_t *out_size) {
    if (!initrd_files) return -1;

    for (uint32_t i = 0; i < initrd_num_files; i++) {
        if (strcmp(initrd_files[i].name, name) == 0) {
            *out_phys = initrd_phys_start + initrd_files[i].offset;
            *out_size = initrd_files[i].size;
            return 0;
        }
    }

    return -1;
}

void initrd_list(void) {
    if (!initrd_files || initrd_num_files == 0) {
        printf("(no files)\n");
        return;
    }

    for (uint32_t i = 0; i < initrd_num_files; i++) {
        printf("  %s  (%d bytes)\n", initrd_files[i].name, initrd_files[i].size);
    }
}
