#include <kernel/isr.h>
#include <kernel/pic.h>
#include <kernel/scheduler.h>
#include <stdio.h>

static const char* exception_names[32] = {
    "Divide By Zero",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound Range",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection",
    "Page Fault",
    "Reserved",
    "x87 FP Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD FP Exception",
    "Virtualization",
    "Control Protection",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

// static void time_handler(trapframe *tf) {
//     scheduler_tick(tf);
// }

static irq_handler_t irq_handlers[16] = {0};

void irq_install_handler(uint8_t irq, irq_handler_t h) {
    if (irq < 16) irq_handlers[irq] = h;
}
void irq_uninstall_handler(uint8_t irq) {
    if (irq < 16) irq_handlers[irq] = 0;
}

uint32_t isr_common_handler(trapframe* r) {
    // if (r->int_no == 33) {
    //     printf("IRQ1\n");
    // }

    // CPU exceptions
    if (r->int_no < 32) {
        printf("\n[EXCEPTION %u] %s\n", r->int_no, exception_names[r->int_no]);
        printf("EIP=%x CS=%x EFLAGS=%x INT_NO=%x\n", r->eip, r->cs, r->eflags, r->int_no);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // PIC IRQs (after remap)
    if (r->int_no >= 32 && r->int_no <= 47) {
        uint8_t irq = (uint8_t)(r->int_no - 32);

        if (irq_handlers[irq]) {
            irq_handlers[irq](r);
        }

        pic_send_eoi(irq);

        // if this was IRQ0, ask schedule whether to switch stacks
        if (irq == 0) {
            return scheduler_tick(r); // return new ESP (or 0)
        }
        return 0;
    }

    return 0;
}
