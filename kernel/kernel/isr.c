#include <kernel/isr.h>
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

void isr_common_handler(regs_t* r) {
    if (r->int_no < 32) {
        printf("\n[EXCEPTION %u] %s\n", r->int_no, exception_names[r->int_no]);
    } else {
        printf("\n[EXCEPTION %u]\n", r->int_no);
    }

    printf("EIP=%x CS=%x EFLAGS=%x INT_NO=%x\n", r->eip, r->cs, r->eflags, r->int_no);

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
