#include <kernel/pic.h>
#include <kernel/io.h>

/* PIC PORTS */
#define PIC1_COMMAND    0X20
#define PIC1_DATA       0X21
#define PIC2_COMMAND    0XA0
#define PIC2_DATA       0XA1

#define PIC_EOI         0X20

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_remap(uint8_t offset1, uint8_t offset2) {
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, 0x11); io_wait();
    outb(PIC2_COMMAND, 0x11); io_wait();

    outb(PIC1_DATA, offset1); io_wait();
    outb(PIC2_DATA, offset2); io_wait();

    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t bit = (irq < 8) ? irq : (irq - 8);
    uint8_t val = inb(port) | (1u << bit);
    outb(port, val);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t bit = (irq < 8) ? irq : (irq - 8);
    uint8_t val = inb(port) & ~(1u << bit);
    outb(port, val);

    /* Slave PIC IRQs (8-15) require IRQ2 (cascade) unmasked on the
       master PIC, otherwise the slave's interrupts never reach the CPU. */
    if (irq >= 8) {
        uint8_t master = inb(PIC1_DATA);
        if (master & (1u << 2))
            outb(PIC1_DATA, master & ~(1u << 2));
    }
}

