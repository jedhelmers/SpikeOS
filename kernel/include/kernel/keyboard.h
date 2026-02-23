#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <kernel/io.h>
#include <kernel/isr.h>
#include <kernel/key_event.h>
#include <stdio.h>

void keyboard_init(void);
key_event_t keyboard_get_event(void);
key_event_t keyboard_get_event_blocking(void);

#endif