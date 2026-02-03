#include <kernel/uart.h>
#include <kernel/io.h>

static inline int uart_received(void) {
    return inb(COM1 + 5) & 1;
}

static inline int uart_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void uart_init(void) {
    outb(COM1 + 1, 0x00);    // Temporarliy disable interrupts
    outb(COM1 + 3, 0x80);    // Enable Divisor Latch Access Bit (DLAB) - this makes available two registers
    outb(COM1 + 0, 0x03);    // Divisor low (38400 baud) - first register
    outb(COM1 + 1, 0x00);    // Divisor high - second register
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set

    outb(COM1 + 1, 0x01);    // Reenable RX interrupt
}

uint8_t uart_read(void) {
    while (!uart_received());

    return inb(COM1);
}

void uart_write(uint8_t b) {
    while (!uart_transmit_empty());

    outb(COM1, b);
}
