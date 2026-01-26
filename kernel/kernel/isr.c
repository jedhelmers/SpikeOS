#include <kernel/isr.h>

void isr_handler(uint32_t int_no, uint32_t err_code) {
    (void)err_code;

    printf("\n[EXCEPTION] int=%x (divide-by-zero?)\n", int_no);

    /* Halt so you can see it and not keep running */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}