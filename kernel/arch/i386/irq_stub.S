.extern isr_common

/* Hardware IRQs (PIC, remapped to 32-47) */
.macro IRQ n
.global irq\n
irq\n:
    cli
    .set irqvec, 32 + \n    /* int_no */
    pushl $0                /* fake err_code */
    pushl $irqvec
    jmp isr_common
.endm

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15