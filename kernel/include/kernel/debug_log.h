#ifndef _DEBUG_LOG_H
#define _DEBUG_LOG_H

#include <stdint.h>

void debug_log_pgfault(uint32_t eip, uint32_t cr3);
void debug_log_pgfault_live(uint32_t eip, uint32_t cr3, uint32_t pde, uint32_t pte);
void debug_log_sched_switch(uint32_t next_pid, uint32_t next_mm);
void debug_log_user_create(uint32_t pd, uint32_t user_entry_phys, uint32_t pte_0x1000);

#endif
