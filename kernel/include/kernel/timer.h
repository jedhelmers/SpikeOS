#ifndef _TIMER_H
#define _TIMER_H

#include <kernel/isr.h>
#include <kernel/io.h>
#include <stdint.h>

void timer_init(uint32_t hz);
uint32_t timer_ticks(void);

#endif