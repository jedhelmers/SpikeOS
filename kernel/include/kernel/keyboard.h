#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <kernel/io.h>
#include <kernel/isr.h>
#include <stdio.h>

void keyboard_init(void);
char keyboard_getchar(void);

#endif