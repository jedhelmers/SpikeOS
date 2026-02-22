#ifndef _INITRD_H
#define _INITRD_H

#include <stdint.h>

#define INITRD_MAGIC 0x52444E49  /* "INDR" in little-endian */

struct initrd_header {
    uint32_t magic;
    uint32_t num_files;
};

struct initrd_file_entry {
    char     name[60];
    uint32_t offset;    /* byte offset from start of archive */
    uint32_t size;      /* file size in bytes */
};

/*
 * Initialize the initrd subsystem from a GRUB module.
 * phys_start / phys_end: physical address range of the module blob.
 * Returns 0 on success, -1 on failure.
 */
int initrd_init(uint32_t phys_start, uint32_t phys_end);

/*
 * Find a file in the initrd by name.
 * If found, *out_phys and *out_size are set.
 * Returns 0 on success, -1 if not found.
 */
int initrd_find(const char *name, uint32_t *out_phys, uint32_t *out_size);

/*
 * List all files in the initrd (prints to terminal).
 */
void initrd_list(void);

/*
 * Return the number of files in the initrd.
 */
uint32_t initrd_count(void);

/*
 * Get initrd file entry by index.
 * Sets *name_out, *phys_out, *size_out on success.
 * Returns 0 on success, -1 if index out of range.
 */
int initrd_get_entry(uint32_t index, const char **name_out,
                     uint32_t *phys_out, uint32_t *size_out);

#endif
