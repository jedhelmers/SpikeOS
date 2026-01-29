#ifndef _SHELL_H
#define _SHELL_H

#include <kernel/keyboard.h>
#include <kernel/key_event.h>
#include "string.h"

void shell_run(void);
void shell_readline(void);
void shell_execute(void);
void shell_init_prefix(void);
void shell_clear(void);

#endif