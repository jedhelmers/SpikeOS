/* Multiboot header constants */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

/*
    Multiboot header – must be in the first 8 KiB of the file.
    Placed in its own section so the linker script can force it first.
*/
.section .multiboot,"a",@progbits
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/*
    Bootstrap data: boot-time page directory, page table, and stack.
    These live at low physical addresses (VMA == LMA in .bootdata).
    We reference them by their link address, which IS their physical address.
*/
.section .boot.bss, "aw", @nobits

.align 4096
boot_page_dir:
    .skip 4096          /* 1024 × 4-byte PDEs */

.align 4096
boot_page_table0:
    .skip 4096          /* 1024 × 4-byte PTEs – covers first 4 MiB */

.align 16
boot_stack_bottom:
    .skip 16384         /* 16 KiB kernel stack */
boot_stack_top:

.global multiboot_info_ptr
.align 4
multiboot_info_ptr:
    .skip 4             /* saved EBX from GRUB (Multiboot info pointer) */

/*
    _start: entry point called by GRUB.
    Runs with paging OFF at a low physical address.

    Responsibilities:
      1. Set up a temporary page directory that maps the first 4 MiB both
         identity (PDE[0]) and at 0xC0000000 (PDE[768]), so the kernel can
         keep running through the CR0.PG flip and the jump to the higher half.
      2. Enable paging.
      3. Long-jump into the higher-half virtual address of _higher_half_start.
*/
.section .boot.text, "ax"
.global _start
.type _start, @function
_start:
    /* Use the physical (boot) stack while paging is off */
    movl $boot_stack_top, %esp

    /* Save the Multiboot info pointer (GRUB passes it in EBX) */
    movl %ebx, multiboot_info_ptr

    /* ------------------------------------------------------------------
       Fill boot_page_table0: map frame i → virtual page i (first 4 MiB)
       Entry format: (frame_addr) | PRESENT(1) | WRITABLE(2)
    ------------------------------------------------------------------ */
    movl $0, %ecx
.Lfill_pt:
    movl %ecx, %eax
    shll $12, %eax          /* physical address = frame * 4096 */
    orl  $3,  %eax          /* PRESENT | WRITABLE */
    movl %eax, boot_page_table0(, %ecx, 4)
    addl $1,  %ecx
    cmpl $1024, %ecx
    jl   .Lfill_pt

    /* PDE[0]   → identity map  0x00000000–0x003FFFFF */
    movl $boot_page_table0, %eax
    orl  $3, %eax
    movl %eax, boot_page_dir

    /* PDE[768] → higher-half  0xC0000000–0xC03FFFFF */
    movl $boot_page_table0, %eax
    orl  $3, %eax
    movl %eax, boot_page_dir + (768 * 4)

    /* Load CR3 with physical address of boot page directory */
    movl $boot_page_dir, %eax
    movl %eax, %cr3

    /* Set CR0.PG – paging is now ON */
    movl %cr0, %eax
    orl  $0x80000000, %eax
    movl %eax, %cr0

    /*
        We are still executing at a low physical/virtual address (identity map).
        Do an absolute indirect jump to the higher-half virtual address of
        _higher_half_start so that EIP moves into 0xC0000000+ space.
    */
    lea  _higher_half_start, %eax
    jmp  *%eax

.size _start, . - _start

/*
    _higher_half_start: entered immediately after the paging jump.
    Now running at VMA 0xC0000000+.
*/
.section .text
.global _higher_half_start
.type _higher_half_start, @function
_higher_half_start:
    /* Call global constructors (needed for C++) */
    call _init

    /* Hand off to the C kernel */
    call kernel_main

    /* kernel_main must never return; halt if it does */
    cli
1:  hlt
    jmp 1b

.size _higher_half_start, . - _higher_half_start
