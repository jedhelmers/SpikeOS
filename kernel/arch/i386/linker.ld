ENTRY(_start)

SECTIONS {
    /* Physical load address – GRUB places the image here */
    . = 0x00200000;

    /* Multiboot header must appear within the first 8 KiB of the file */
    .multiboot : ALIGN(4K) {
        *(.multiboot)
    }

    /* Pre-paging bootstrap code/data – VMA == LMA (low physical addresses) */
    .boottext : ALIGN(4K) {
        *(.boot.text)
        *(.boot.rodata)
    }

    .bootdata : ALIGN(4K) {
        *(.boot.data)
        *(.boot.bss)
    }

    /*
        Record the physical address where the higher-half sections begin.
        Used in AT() so the ELF LMA stays in low memory while the VMA lives
        at 0xC0000000+.
    */
    . = ALIGN(4K);
    _higher_half_lma = .;

    /*
        Switch location counter to higher-half VMA.
        Adding 0xC0000000 means VMA = phys + 0xC0000000, so the boot page
        directory (PDE[768]: 0xC0000000–0xC03FFFFF → 0x00000000–0x003FFFFF)
        maps each virtual address to its correct physical frame.
    */
    . += 0xC0000000;

    _kernel_start = .;

    /*
        AT(_higher_half_lma) sets the LMA for .text.  Subsequent sections
        inherit the VMA-to-LMA delta automatically, giving each section an
        LMA of  VMA - 0xC0000000  (i.e. its physical load address).
    */
    .text BLOCK(4K) : AT(_higher_half_lma) ALIGN(4K) {
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K) {
        *(COMMON)
        *(.bss)
    }

    endkernel = .;
}
