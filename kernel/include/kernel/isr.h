/*
    Interrupt Service Routine (ISR)
*/
#ifndef _ISR_H
#define _ISR_H

#include <stdint.h>

typedef struct trapframe {
    uint32_t ds, es, fs, gs;
    // uint32_t gs, fs, es, ds;
    uint32_t edi; // Destination pointer for string ops
    uint32_t esi; // Source pointer for string ops
    uint32_t ebp; // Points to the base of the current stack
    uint32_t esp_dummy; // Points to the top of the stack - controls push/pop, call/ret
    uint32_t ebx; // Base pointer for arrays/structs
    uint32_t edx; // I/O port address
    uint32_t ecx; // Usually for counters
    uint32_t eax; // Accumulator
    uint32_t int_no;
    uint32_t err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} trapframe;

void isr_common_handler(trapframe* r);

typedef void (*irq_handler_t)(trapframe*);
void irq_install_handler(uint8_t irq, irq_handler_t h);
void irq_uninstall_handler(uint8_t irq);

#endif
