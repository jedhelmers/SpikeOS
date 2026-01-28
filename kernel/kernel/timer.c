#include <kernel/timer.h>


static volatile uint32_t g_ticks = 0;

static void timer_irq(regs_t* r) {
    (void)r;
    g_ticks++;
}

uint32_t timer_ticks(void) {
    return g_ticks;

}

void timer_init(uint32_t hz) {
    irq_install_handler(0, timer_irq);

    // PIT: channel 0, lobyte/hibyte, mode 3 (square wave), binary
    uint32_t divisor = 1193182u / 100;

    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}