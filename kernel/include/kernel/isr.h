#ifndef _ISR_H
#define _ISR_H

#include <stdint.h>

typedef struct regs {
    uint32_t ds, es, fs, gs;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t err_code, int_no;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

void isr_common_handler(regs_t* r);

typedef void (*irq_handler_t)(regs_t*);
void irq_install_handler(uint8_t irq, irq_handler_t h);
void irq_uninstall_handler(uint8_t irq);

#endif
