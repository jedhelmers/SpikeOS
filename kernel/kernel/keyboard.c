#include <kernel/keyboard.h>

static void keyboard_irq(regs_t* r) {
    (void)r;
    uint8_t sc = inb(0x60);

    printf("[KBD %x]\n", sc);
}

void keyboard_init(void) {
    irq_install_handler(1, keyboard_irq);
}