#ifndef _ELF_H
#define _ELF_H

#include <stdint.h>

/* ELF identification indices */
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5
#define EI_NIDENT  16

#define ELFMAG0    0x7f
#define ELFMAG1    'E'
#define ELFMAG2    'L'
#define ELFMAG3    'F'

#define ELFCLASS32  1
#define ELFDATA2LSB 1

/* e_type */
#define ET_EXEC 2

/* e_machine */
#define EM_386 3

/* Program header types */
#define PT_NULL 0
#define PT_LOAD 1

/* Program header flags */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

/*
 * Load an ELF binary from the initrd and create a user process.
 * file_phys: physical address of the ELF file in the initrd
 * file_size: size of the ELF file in bytes
 * Returns pointer to the created process, or NULL on failure.
 */
struct process *elf_load_and_exec(uint32_t file_phys, uint32_t file_size);

/*
 * Load an ELF binary by name (looks up in initrd).
 * name: filename (e.g. "hello.elf")
 * Returns pointer to the created process, or NULL on failure.
 */
struct process *elf_spawn(const char *name);

#endif
