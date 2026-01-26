#ifndef _ISR_H
#define _ISR_H

#include <stdint.h>

typedef struct regs {
    uint32_t ds;
    uint32_t edi, esi, edp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

void isr_common_handler(regs_t* r);

#endif