#ifndef _ISR_H
#define _ISR_H

#include <stdio.h>
#include <stdint.h>

void isr_handler(uint32_t int_no, uint32_t err_code);

#endif