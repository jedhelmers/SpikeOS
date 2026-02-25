#include <kernel/keyboard.h>
#include <kernel/wait.h>
#include <kernel/event.h>

#define KBD_BUF_SIZE 128

// Ring buffer
static key_event_t kbd_buf[KBD_BUF_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;

static wait_queue_t keyboard_wq = WAIT_QUEUE_INIT;

static int ctrl_held  = 0;
static int shift_held = 0;
static int extended   = 0;  /* set when 0xE0 prefix received */

static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,   '*', 0,  ' '
};

static const char scancode_to_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',
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

int keyboard_shift_held(void) {
    return shift_held;
}

static void keyboard_irq(trapframe* r) {
    (void)r;

    uint8_t sc = inb(0x60);

    /* Extended scancode prefix — next byte is the real scancode */
    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    /* Handle key releases (bit 7 set) */
    if (sc & 0x80) {
        uint8_t release = sc & 0x7F;
        if (release == 0x2A || release == 0x36)  /* left/right shift */
            shift_held = 0;
        else if (release == 0x1D)                 /* ctrl */
            ctrl_held = 0;
        extended = 0;
        return;
    }

    /* Ctrl press (left = 0x1D, right = extended+0x1D) */
    if (sc == 0x1D) {
        ctrl_held = 1;
        extended  = 0;
        return;
    }

    /* Shift press (left = 0x2A, right = 0x36) */
    if (sc == 0x2A || sc == 0x36) {
        shift_held = 1;
        extended   = 0;
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
            case 0x47: e.type = KEY_HOME;      break;
            case 0x4F: e.type = KEY_END;       break;
            case 0x52: e.type = KEY_INSERT;    break;
            case 0x53: e.type = KEY_DELETE;    break;
        }
        extended = 0;
    } else if (ctrl_held) {
        /* Ctrl+letter combinations */
        switch (sc) {
            case 0x2E: e.type = KEY_CTRL_C; break;     /* Ctrl+C */
            case 0x1F: e.type = KEY_CTRL_S; break;     /* Ctrl+S */
            case 0x2D: e.type = KEY_CTRL_X; break;     /* Ctrl+X */
            case 0x25: e.type = KEY_CTRL_K; break;     /* Ctrl+K */
            case 0x2F: e.type = KEY_CTRL_V; break;     /* Ctrl+V */
            case 0x1E: e.type = KEY_CTRL_A; break;     /* Ctrl+A */
            case 0x10: e.type = KEY_CTRL_Q; break;     /* Ctrl+Q */
            case 0x0D: e.type = KEY_CTRL_PLUS; break;  /* Ctrl+= */
            case 0x0C: e.type = KEY_CTRL_MINUS; break; /* Ctrl+- */
            case 0x2C:                                  /* Ctrl+Z / Ctrl+Shift+Z */
                e.type = shift_held ? KEY_CTRL_SHIFT_Z : KEY_CTRL_Z;
                break;
        }
    } else if (sc == 0x0E) {
        /* Backspace */
        e.type = KEY_BACKSPACE;
    } else if (sc == 0x1C) {
        e.type = KEY_ENTER;
    } else if (sc == 0x0F) {
        e.type = KEY_TAB;
    } else if (sc < 128) {
        char c = shift_held ? scancode_to_ascii_shift[sc]
                            : scancode_to_ascii[sc];
        if (c) {
            e.type = KEY_CHAR;
            e.ch   = c;
        }
    }

    if (e.type != KEY_NONE) {
        kbd_push(e);
        wake_up_one(&keyboard_wq);
        event_push_key(EVENT_KEY_PRESS, e.type, e.ch);
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