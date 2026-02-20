/*
 * mkinitrd — host tool to create a SpikeOS initrd image.
 *
 * Usage: mkinitrd output.img file1 [file2 ...]
 *
 * Format:
 *   initrd_header (8 bytes): magic + num_files
 *   initrd_file_entry[num_files] (68 bytes each): name + offset + size
 *   file data concatenated at the specified offsets
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define INITRD_MAGIC 0x52444E49  /* "INDR" */

struct initrd_header {
    uint32_t magic;
    uint32_t num_files;
};

struct initrd_file_entry {
    char     name[60];
    uint32_t offset;
    uint32_t size;
};

/* Strip directory prefix from a path: "foo/bar/baz.elf" -> "baz.elf" */
static const char *basename_of(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s output.img file1 [file2 ...]\n", argv[0]);
        return 1;
    }

    const char *outfile = argv[1];
    int nfiles = argc - 2;

    /* Read all input files */
    uint8_t **file_data = calloc(nfiles, sizeof(uint8_t *));
    uint32_t *file_sizes = calloc(nfiles, sizeof(uint32_t));
    if (!file_data || !file_sizes) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    for (int i = 0; i < nfiles; i++) {
        FILE *f = fopen(argv[i + 2], "rb");
        if (!f) {
            perror(argv[i + 2]);
            return 1;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);

        file_data[i] = malloc(sz);
        if (!file_data[i]) {
            fprintf(stderr, "malloc failed for %s\n", argv[i + 2]);
            return 1;
        }

        if (fread(file_data[i], 1, sz, f) != (size_t)sz) {
            fprintf(stderr, "read error on %s\n", argv[i + 2]);
            return 1;
        }

        file_sizes[i] = (uint32_t)sz;
        fclose(f);
    }

    /* Calculate offsets: data starts after header + file entries */
    uint32_t data_start = sizeof(struct initrd_header) +
                          nfiles * sizeof(struct initrd_file_entry);

    /* Build file entries */
    struct initrd_file_entry *entries = calloc(nfiles, sizeof(struct initrd_file_entry));
    if (!entries) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    uint32_t cur_offset = data_start;
    for (int i = 0; i < nfiles; i++) {
        const char *name = basename_of(argv[i + 2]);
        strncpy(entries[i].name, name, sizeof(entries[i].name) - 1);
        entries[i].name[sizeof(entries[i].name) - 1] = '\0';
        entries[i].offset = cur_offset;
        entries[i].size = file_sizes[i];
        cur_offset += file_sizes[i];
    }

    /* Write output */
    FILE *out = fopen(outfile, "wb");
    if (!out) {
        perror(outfile);
        return 1;
    }

    struct initrd_header hdr = {
        .magic = INITRD_MAGIC,
        .num_files = (uint32_t)nfiles,
    };

    fwrite(&hdr, sizeof(hdr), 1, out);
    fwrite(entries, sizeof(struct initrd_file_entry), nfiles, out);

    for (int i = 0; i < nfiles; i++) {
        fwrite(file_data[i], 1, file_sizes[i], out);
    }

    fclose(out);

    printf("mkinitrd: created %s (%d file(s), %u bytes)\n",
           outfile, nfiles, cur_offset);

    /* Cleanup */
    for (int i = 0; i < nfiles; i++) free(file_data[i]);
    free(file_data);
    free(file_sizes);
    free(entries);

    return 0;
}
