#include <kernel/keyboard.h>

static char key_buffer[128];
static int key_index = 0;

static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',
    0,   '*', 0,  ' '
};


char keyboard_getchar(void) {
    if (!key_index) return 0;

    char c = key_buffer[0];

    for (int i = 0; i < key_index; i++) {
        key_buffer[i - 1] = key_buffer[i];
    }

    key_index--;

    return c;
}

static void keyboard_irq(regs_t* r) {
    (void)r;
    uint8_t sc = inb(0x60);

    if (!(sc & 0x80)) {
        char c = scancode_to_ascii[sc];
        if (c && key_index < 127) {
            key_buffer[key_index++] = c;
        }
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