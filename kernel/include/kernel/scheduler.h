#ifndef _SCHEDULER_H
#define _SCHEDULER_H

struct trapframe;   // forward declaration

void scheduler_init(void);

void scheduler_tick(struct trapframe *tf);

#endif