#include <kernel/event.h>
#include <kernel/wait.h>
#include <kernel/timer.h>
#include <kernel/hal.h>

#define EVENT_BUF_SIZE 256

static event_t  event_buf[EVENT_BUF_SIZE];
static volatile uint32_t event_head = 0;
static volatile uint32_t event_tail = 0;

static wait_queue_t event_wq = WAIT_QUEUE_INIT;

void event_init(void) {
    event_head = 0;
    event_tail = 0;
}

static void event_push(event_t *e) {
    uint32_t next = (event_head + 1) % EVENT_BUF_SIZE;
    if (next == event_tail) {
        /* Buffer full — drop oldest event */
        event_tail = (event_tail + 1) % EVENT_BUF_SIZE;
    }
    event_buf[event_head] = *e;
    event_head = next;
    wake_up_one(&event_wq);
}

event_t event_poll(void) {
    event_t e;
    e.type = EVENT_NONE;

    if (event_head == event_tail)
        return e;

    e = event_buf[event_tail];
    event_tail = (event_tail + 1) % EVENT_BUF_SIZE;
    return e;
}

event_t event_wait(void) {
    event_t e;
    while (1) {
        e = event_poll();
        if (e.type != EVENT_NONE)
            return e;
        sleep_on(&event_wq);
    }
}

void event_push_key(event_type_t type, key_type_t key, char ch) {
    event_t e;
    e.type = type;
    e.timestamp = timer_ticks();
    e.keyboard.key = key;
    e.keyboard.ch = ch;
    event_push(&e);
}

void event_push_mouse_move(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    event_t e;
    e.type = EVENT_MOUSE_MOVE;
    e.timestamp = timer_ticks();
    e.mouse_move.x = x;
    e.mouse_move.y = y;
    e.mouse_move.dx = dx;
    e.mouse_move.dy = dy;
    event_push(&e);
}

void event_push_mouse_button(int32_t x, int32_t y,
                              uint8_t button, uint8_t pressed) {
    event_t e;
    e.type = EVENT_MOUSE_BUTTON;
    e.timestamp = timer_ticks();
    e.mouse_button.x = x;
    e.mouse_button.y = y;
    e.mouse_button.button = button;
    e.mouse_button.pressed = pressed;
    event_push(&e);
}
