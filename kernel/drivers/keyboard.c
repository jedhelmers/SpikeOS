#include <kernel/keyboard.h>
#include <kernel/wait.h>

#define KBD_BUF_SIZE 128

// Ring buffer
static key_event_t kbd_buf[KBD_BUF_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

static wait_queue_t keyboard_wq = WAIT_QUEUE_INIT;

static int ctrl_held = 0;
static int extended  = 0;  /* set when 0xE0 prefix received */

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

key_event_t keyboard_get_event_blocking(void) {
    key_event_t e;
    while (1) {
        e = keyboard_get_event();
        if (e.type != KEY_NONE)
            return e;
        sleep_on(&keyboard_wq);
    }
}

static void keyboard_irq(trapframe* r) {
    (void)r;

    uint8_t sc = inb(0x60);

    /* Extended scancode prefix — next byte is the real scancode */
    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    /* Ctrl release: 0x9D (left) or extended+0x1D (right) */
    if (sc == 0x9D) {
        ctrl_held = 0;
        extended  = 0;
        return;
    }

    /* All other key releases — clear extended flag and ignore */
    if (sc & 0x80) {
        extended = 0;
        return;
    }

    /* Ctrl press (left = 0x1D, right = extended+0x1D) */
    if (sc == 0x1D) {
        ctrl_held = 1;
        extended  = 0;
        return;
    }

    key_event_t e;
    e.type = KEY_NONE;
    e.ch   = 0;

    if (extended) {
        /* Extended keys (E0-prefixed) */
        switch (sc) {
            case 0x48: e.type = KEY_UP;        break;
            case 0x50: e.type = KEY_DOWN;      break;
            case 0x4B: e.type = KEY_LEFT;      break;
            case 0x4D: e.type = KEY_RIGHT;     break;
            case 0x49: e.type = KEY_PAGE_UP;   break;
            case 0x51: e.type = KEY_PAGE_DOWN; break;
        }
        extended = 0;
    } else if (ctrl_held && sc == 0x2E) {
        /* Ctrl+C (scancode 0x2E = 'c') */
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
            e.ch   = c;
        }
    }

    if (e.type != KEY_NONE) {
        kbd_push(e);
        wake_up_one(&keyboard_wq);
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