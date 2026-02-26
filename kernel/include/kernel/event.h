#ifndef _EVENT_H
#define _EVENT_H

#include <stdint.h>
#include <kernel/key_event.h>

typedef enum {
    EVENT_NONE = 0,
    EVENT_KEY_PRESS,
    EVENT_KEY_RELEASE,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_BUTTON,
    EVENT_MOUSE_SCROLL,
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t     timestamp;   /* timer_ticks() value */
    union {
        struct { key_type_t key; char ch; } keyboard;
        struct { int32_t x, y, dx, dy; } mouse_move;
        struct { int32_t x, y; uint8_t button; uint8_t pressed; } mouse_button;
        struct { int32_t x, y; int32_t dz; } mouse_scroll;
    };
} event_t;

/* Initialize the unified event queue (call once at boot) */
void event_init(void);

/* Non-blocking poll: returns event with type EVENT_NONE if queue is empty */
event_t event_poll(void);

/* Blocking wait: sleeps until an event is available */
event_t event_wait(void);

/* Push functions called by IRQ handlers (keyboard.c and mouse.c) */
void event_push_key(event_type_t type, key_type_t key, char ch);
void event_push_mouse_move(int32_t x, int32_t y, int32_t dx, int32_t dy);
void event_push_mouse_button(int32_t x, int32_t y,
                              uint8_t button, uint8_t pressed);
void event_push_mouse_scroll(int32_t x, int32_t y, int32_t dz);

#endif
