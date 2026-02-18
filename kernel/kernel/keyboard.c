#include <kernel/keyboard.h>

#define KBD_BUF_SIZE 128

// Ring buffer
static key_event_t kbd_buf[KBD_BUF_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

static int ctrl_held = 0;

static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,   '*', 0,  ' '
};

static inline void kbd_push(key_event_t e) {
    uint8_t next = (kbd_head + 1) % KBD_BUF_SIZE;

    if (next != kbd_tail) {
        // drop if full
        kbd_buf[kbd_head] = e;
        kbd_head = next;
    }
}

key_event_t keyboard_get_event(void) {
    key_event_t o;
    o.type = KEY_NONE;

    if (kbd_head == kbd_tail) {
        return o; // empty
    }

    o = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;

    return o;
}

static void keyboard_irq(trapframe* r) {
    (void)r;

    uint8_t sc = inb(0x60);

    /* Ctrl release (0x9D = 0x1D | 0x80) — handle before generic release filter */
    if (sc == 0x9D) {
        ctrl_held = 0;
        return;
    }

    /* Ignore other key releases */
    if (sc & 0x80) {
        return;
    }

    /* Ctrl press */
    if (sc == 0x1D) {
        ctrl_held = 1;
        return;
    }

    key_event_t e;
    e.type = KEY_NONE;
    e.ch = 0;

    /* Ctrl+C (scancode 0x2E = 'c') */
    if (ctrl_held && sc == 0x2E) {
        e.type = KEY_CTRL_C;
    } else if (sc == 0x0E) {
        /* Backspace */
        e.type = KEY_BACKSPACE;
    } else if (sc == 0x1C) {
        e.type = KEY_ENTER;
    } else if (sc < 128) {
        char c = scancode_to_ascii[sc];

        if (c) {
            e.type = KEY_CHAR;
            e.ch = c;
        }
    }

    if (e.type != KEY_NONE) {
        kbd_push(e);
    }
}

void keyboard_init(void) {
    /* Flush buffer */
    while (inb(0x64) & 1) {
        inb(0x60);
    }

    /* Enable keyboard IRQs on the controller */
    outb(0x64, 0xAE);

    irq_install_handler(1, keyboard_irq);
}