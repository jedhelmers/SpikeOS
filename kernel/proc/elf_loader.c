#include <kernel/elf.h>
#include <kernel/paging.h>
#include <kernel/process.h>
#include <kernel/initrd.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <string.h>
#include <stdio.h>

#define USER_STACK_VADDR 0xBFFFF000u
#define USER_STACK_TOP   0xC0000000u

/*
 * Static staging buffer for copying data between physical pages.
 * Must be static (not on the 4KB kernel stack).
 */
static uint8_t elf_staging[PAGE_SIZE] __attribute__((aligned(4096)));

/*
 * Read 'count' bytes from initrd at physical address 'src_phys'
 * into the kernel buffer 'dst'. Handles page-boundary crossings
 * via repeated temp_map/temp_unmap calls.
 */
static void initrd_read(uint32_t src_phys, void *dst, uint32_t count) {
    uint8_t *out = (uint8_t *)dst;

    while (count > 0) {
        uint32_t page_base = src_phys & ~0xFFFu;
        uint32_t page_off  = src_phys & 0xFFFu;
        uint32_t chunk = PAGE_SIZE - page_off;
        if (chunk > count) chunk = count;

        uint8_t *mapped = (uint8_t *)temp_map(page_base);
        memcpy(out, mapped + page_off, chunk);
        temp_unmap();

        out      += chunk;
        src_phys += chunk;
        count    -= chunk;
    }
}

struct process *elf_load_and_exec(uint32_t file_phys, uint32_t file_size) {
    /* ---- 1. Read and validate ELF header ---- */
    Elf32_Ehdr ehdr;

    if (file_size < sizeof(Elf32_Ehdr)) {
        printf("[elf] file too small for ELF header\n");
        return NULL;
    }

    initrd_read(file_phys, &ehdr, sizeof(ehdr));

    if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3) {
        printf("[elf] bad magic\n");
        return NULL;
    }

    if (ehdr.e_ident[EI_CLASS] != ELFCLASS32) {
        printf("[elf] not ELF32\n");
        return NULL;
    }

    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        printf("[elf] not little-endian\n");
        return NULL;
    }

    if (ehdr.e_type != ET_EXEC) {
        printf("[elf] not ET_EXEC (type=%d)\n", ehdr.e_type);
        return NULL;
    }

    if (ehdr.e_machine != EM_386) {
        printf("[elf] not i386 (machine=%d)\n", ehdr.e_machine);
        return NULL;
    }

    if (ehdr.e_phnum == 0) {
        printf("[elf] no program headers\n");
        return NULL;
    }

    /* ---- 2. Read program headers into a kmalloc'd buffer ---- */
    uint32_t ph_total = ehdr.e_phnum * ehdr.e_phentsize;
    Elf32_Phdr *phdrs = (Elf32_Phdr *)kmalloc(ph_total);
    if (!phdrs) {
        printf("[elf] kmalloc failed for phdrs\n");
        return NULL;
    }

    initrd_read(file_phys + ehdr.e_phoff, phdrs, ph_total);

    /* ---- 3. Create a new page directory ---- */
    uint32_t pd_phys = pgdir_create();
    if (pd_phys == 0) {
        printf("[elf] pgdir_create failed\n");
        kfree(phdrs);
        return NULL;
    }

    /* ---- 4. Map each PT_LOAD segment ---- */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr *ph = &phdrs[i];

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        /* Validate: segment must be in user space */
        if (ph->p_vaddr >= 0xC0000000u) {
            printf("[elf] segment vaddr 0x%x in kernel space\n", ph->p_vaddr);
            goto fail;
        }

        uint32_t start_page = ph->p_vaddr & ~0xFFFu;
        uint32_t end_addr   = ph->p_vaddr + ph->p_memsz;
        uint32_t end_page   = (end_addr + PAGE_SIZE - 1) & ~0xFFFu;

        for (uint32_t page_vaddr = start_page; page_vaddr < end_page;
             page_vaddr += PAGE_SIZE) {
            /* Allocate a physical frame for this page */
            uint32_t frame = alloc_frame();
            if (frame == 0) {
                printf("[elf] alloc_frame failed\n");
                goto fail;
            }

            /* Map it in the user's page directory */
            if (pgdir_map_user_page(pd_phys, page_vaddr, frame,
                                    PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
                printf("[elf] pgdir_map_user_page failed at 0x%x\n", page_vaddr);
                free_frame(frame);
                goto fail;
            }

            /* Zero the staging buffer (handles BSS regions) */
            memset(elf_staging, 0, PAGE_SIZE);

            /*
             * Calculate which bytes of file data land on this page.
             * seg_file_start..seg_file_end = virtual range with file data
             */
            uint32_t seg_file_start = ph->p_vaddr;
            uint32_t seg_file_end   = ph->p_vaddr + ph->p_filesz;
            uint32_t page_end       = page_vaddr + PAGE_SIZE;

            uint32_t copy_start = (page_vaddr > seg_file_start)
                                  ? page_vaddr : seg_file_start;
            uint32_t copy_end   = (page_end < seg_file_end)
                                  ? page_end : seg_file_end;

            if (copy_start < copy_end) {
                uint32_t offset_in_page = copy_start - page_vaddr;
                uint32_t offset_in_file = copy_start - ph->p_vaddr;
                uint32_t bytes_to_copy  = copy_end - copy_start;

                /* Read file data from initrd into staging buffer */
                initrd_read(file_phys + ph->p_offset + offset_in_file,
                            elf_staging + offset_in_page, bytes_to_copy);
            }

            /* Write staging buffer into the destination frame */
            uint8_t *dest = (uint8_t *)temp_map(frame);
            memcpy(dest, elf_staging, PAGE_SIZE);
            temp_unmap();
        }
    }

    /* ---- 5. Allocate and map user stack ---- */
    {
        uint32_t stack_frame = alloc_frame();
        if (stack_frame == 0) {
            printf("[elf] alloc_frame failed for stack\n");
            goto fail;
        }

        if (pgdir_map_user_page(pd_phys, USER_STACK_VADDR, stack_frame,
                                PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
            printf("[elf] pgdir_map_user_page failed for stack\n");
            free_frame(stack_frame);
            goto fail;
        }

        /* Zero the stack page */
        uint8_t *sp = (uint8_t *)temp_map(stack_frame);
        memset(sp, 0, PAGE_SIZE);
        temp_unmap();
    }

    /* ---- 6. Create the user process ---- */
    kfree(phdrs);

    struct process *p = proc_create_user_process(pd_phys, ehdr.e_entry,
                                                  USER_STACK_TOP);
    if (!p) {
        printf("[elf] proc_create_user_process failed\n");
        pgdir_destroy(pd_phys);
        return NULL;
    }

    return p;

fail:
    kfree(phdrs);
    pgdir_destroy(pd_phys);
    return NULL;
}

/*
 * Load an ELF binary from the VFS and create a user process.
 * VFS file data (node->data) is already a contiguous heap buffer,
 * so we can parse ELF headers directly via pointer arithmetic.
 */
struct process *elf_load_from_vfs(const char *path) {
    int32_t ino = vfs_resolve(path, NULL, NULL);
    if (ino < 0) return NULL;

    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (!node || node->type != VFS_TYPE_FILE || node->size == 0)
        return NULL;

    uint8_t *file_data = (uint8_t *)node->data;
    uint32_t file_size = node->size;

    /* ---- 1. Validate ELF header ---- */
    if (file_size < sizeof(Elf32_Ehdr)) return NULL;

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)file_data;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return NULL;

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32 ||
        ehdr->e_ident[EI_DATA]  != ELFDATA2LSB ||
        ehdr->e_type    != ET_EXEC ||
        ehdr->e_machine != EM_386 ||
        ehdr->e_phnum   == 0)
        return NULL;

    /* ---- 2. Locate program headers ---- */
    uint32_t ph_off  = ehdr->e_phoff;
    uint16_t ph_num  = ehdr->e_phnum;
    uint16_t ph_entsz = ehdr->e_phentsize;

    if (ph_off + ph_num * ph_entsz > file_size)
        return NULL;

    Elf32_Phdr *phdrs = (Elf32_Phdr *)(file_data + ph_off);

    /* ---- 3. Create a new page directory ---- */
    uint32_t pd_phys = pgdir_create();
    if (pd_phys == 0) return NULL;

    /* ---- 4. Map each PT_LOAD segment ---- */
    for (uint16_t i = 0; i < ph_num; i++) {
        Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;

        if (ph->p_vaddr >= 0xC0000000u) goto fail;

        uint32_t start_page = ph->p_vaddr & ~0xFFFu;
        uint32_t end_addr   = ph->p_vaddr + ph->p_memsz;
        uint32_t end_page   = (end_addr + PAGE_SIZE - 1) & ~0xFFFu;

        for (uint32_t pv = start_page; pv < end_page; pv += PAGE_SIZE) {
            uint32_t frame = alloc_frame();
            if (frame == 0) goto fail;

            if (pgdir_map_user_page(pd_phys, pv, frame,
                                    PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
                free_frame(frame);
                goto fail;
            }

            memset(elf_staging, 0, PAGE_SIZE);

            uint32_t seg_file_start = ph->p_vaddr;
            uint32_t seg_file_end   = ph->p_vaddr + ph->p_filesz;
            uint32_t page_end       = pv + PAGE_SIZE;

            uint32_t copy_start = (pv > seg_file_start) ? pv : seg_file_start;
            uint32_t copy_end   = (page_end < seg_file_end) ? page_end : seg_file_end;

            if (copy_start < copy_end) {
                uint32_t off_in_page = copy_start - pv;
                uint32_t off_in_file = copy_start - ph->p_vaddr;

                memcpy(elf_staging + off_in_page,
                       file_data + ph->p_offset + off_in_file,
                       copy_end - copy_start);
            }

            uint8_t *dest = (uint8_t *)temp_map(frame);
            memcpy(dest, elf_staging, PAGE_SIZE);
            temp_unmap();
        }
    }

    /* ---- 5. Allocate and map user stack ---- */
    {
        uint32_t stack_frame = alloc_frame();
        if (stack_frame == 0) goto fail;

        if (pgdir_map_user_page(pd_phys, USER_STACK_VADDR, stack_frame,
                                PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER) != 0) {
            free_frame(stack_frame);
            goto fail;
        }

        uint8_t *sp = (uint8_t *)temp_map(stack_frame);
        memset(sp, 0, PAGE_SIZE);
        temp_unmap();
    }

    /* ---- 6. Create the user process ---- */
    {
        struct process *p = proc_create_user_process(pd_phys, ehdr->e_entry,
                                                      USER_STACK_TOP);
        if (!p) {
            pgdir_destroy(pd_phys);
            return NULL;
        }
        return p;
    }

fail:
    pgdir_destroy(pd_phys);
    return NULL;
}

struct process *elf_spawn(const char *name) {
    /* Try VFS first (files are imported from initrd at boot) */
    struct process *p = elf_load_from_vfs(name);
    if (p) return p;

    /* Fall back to initrd for files not yet in VFS */
    uint32_t file_phys, file_size;
    if (initrd_find(name, &file_phys, &file_size) != 0) {
        printf("[elf] '%s' not found\n", name);
        return NULL;
    }

    return elf_load_and_exec(file_phys, file_size);
}
