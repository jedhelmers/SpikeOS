/* Declare constants for the multiboot header. */
.set ALIGN,     1<<0                /* align loaded modules on page boundaries */
.set MEMINFO,   1<<1                /* provide memory map */
.set FLAGS,     ALIGN | MEMINFO     /* this is the multiboot flag field */
.set MAGIC,     0x1BADB002          /* magic number lets bootloader find the header */
.set CHECKSUM,  -(MAGIC + FLAGS)    /* checksum of above, to prove we are multiboot */

/*
    Declare a multiboot header that marks the program as a kernel. These are magic values
    that are documented in the multiboot standard. The bootloader will search for these
    signatures in the first 8KiB of the kernel file, aligned at a 32-bit boundary. The
    signature is in its own section so the header can be forced to be within the first
    8KiB of the kernel file.
*/
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/*
    The multibook standard does not define the value of the stack pointer register (esp)
    and it is up to the kernel to provide a stack. This allocates room for a small stack by
    creating a symbol at the top. The stack grows downwards on x86. The stack is in its
    own section so it can be marked nobits, which means the kernel file is smaller because
    it does not contain an uninitialized stack. The stack on x86 must be 16-byte aligned
    according to the System V ABI standard and de-factor extensions. The compiler will
    assume the stack is properly aligned and failure to align the stack will result in
    undefined behavior.
*/
.section .bss
.align 16
stack_bottom:
    .skip 16384 # 16KiB
stack_top:

/*
    The linker script specifies _start as the entry point to the kernel and the bootloader
    will jump to this position once the kernel has been loaded. It doesn't make sense to
    return from this function as the bootloader is gone alread.
*/

.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp

    # Call the global constructors.
    call _init

    # Transfer control to the main kernel
    call kernel_main

    # Hand if kernel_main unexpectedly returns.
    cli
1:  hlt
    jmp 1b

/*
    Set the size of the _start symbol to the current location '.' minus its start.
    This is useful for debugging or when you implement call tracing.
*/
.size _start, . - _start
