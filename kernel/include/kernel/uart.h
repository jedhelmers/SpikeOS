#pragma once
#include <stdint.h>

#define COM1 0x3F8

void uart_init(void);
uint8_t uart_read(void);
void uart_write(uint8_t b);
