.global isr0

.extern isr_handler

isr0:
    cli

    /* Save general register */
    pusha

    /* Save segment registers */
    push %ds
    push %es
    push %fs
    push %gs

    /* Load kernel data segment (0x10) */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* Switch to kernel data segment. My GDT uses 0x10 for data */
    pushl $0            /* err_code = 0 */
    pushl $0            /* int_no = 0 */

    call isr_handler
    add $8, %esp        /* pop int_no + err_code */

    /* Restore segs + regs */
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa

    sti
    iret
