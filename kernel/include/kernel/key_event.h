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
    KEY_CTRL_S,
    KEY_CTRL_X,
    KEY_CTRL_K,

    KEY_PAGE_UP,
    KEY_PAGE_DOWN,

    KEY_HOME,
    KEY_END,
    KEY_INSERT,

    KEY_TAB,
    KEY_CTRL_V,
    KEY_CTRL_A,
    KEY_CTRL_Q,
    KEY_CTRL_PLUS,
    KEY_CTRL_MINUS,
    KEY_CTRL_Z,
    KEY_CTRL_SHIFT_Z,
} key_type_t;

typedef struct {
    key_type_t type;
    char ch;
} key_event_t;

#endif