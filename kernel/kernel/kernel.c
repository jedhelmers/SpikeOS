#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/gdt.h>

void kernel_main(void) {
    terminal_initialize();
    printf("\n\nHello,\n\tkernels!\n");

    gdt_init();

    printf("\nTesting GDT\n");
    uint16_t cs, ds, ss;
    asm volatile ("mov %%cs, %0" : "=r"(cs));
    asm volatile ("mov %%ds, %0" : "=r"(ds));
    asm volatile ("mov %%ss, %0" : "=r"(ss));

    printf("CS=%x DS=%x SS=%x\n", cs, ds, ss);

}