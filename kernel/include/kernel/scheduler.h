#ifndef _SCHEDULER_H
#define _SCHEDULER_H

#include <stdint.h>

struct trapframe;   // forward declaration

void scheduler_init(void);

uint32_t scheduler_tick(struct trapframe *tf);

#endif