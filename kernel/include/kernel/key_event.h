#ifndef _KEY_EVENT_H
#define _KEY_EVENT_H

#include <stdint.h>

typedef enum {
    KEY_NONE = 0,

    KEY_CHAR,        // printable character

    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_ENTER,

    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,

    KEY_SHIFT_DOWN,
    KEY_SHIFT_UP,

    KEY_CTRL_C,

    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
} key_type_t;

typedef struct {
    key_type_t type;
    char ch;
} key_event_t;

#endif