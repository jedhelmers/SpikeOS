#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>

void kernel_main(void) {
    terminal_initialize();
    printf("\n\nHello,\n\tkernels!\n");

    /*
        GDT setup.
        We're using a flat-model GDT so we can later
        implement paging. Securoty will largely be handled
        by the pages.
    */
    gdt_init();

    /*
        TESTING GDT
    */
    printf("\nTesting GDT\n");
    uint16_t cs, ds, ss;
    asm volatile ("mov %%cs, %0" : "=r"(cs));
    asm volatile ("mov %%ds, %0" : "=r"(ds));
    asm volatile ("mov %%ss, %0" : "=r"(ss));

    printf("CS=%x DS=%x SS=%x\n", cs, ds, ss);

    idt_init();

    printf("\nINIT IDT\n");

    /*
        TESTING INTERRUPTS
    */
    // volatile int x = 1;
    // volatile int y = 0;
    // volatile int z = x / y; // EIP=002008F5 CS=00000008 EFLAGS=00010006
    // (void)z;

    // asm volatile ("ud2"); // EIP=002008DD CS=00000008 EFLAGS=00010046

    // asm volatile("int $3"); // EIP=002008DE CS=00000008 EFLAGS=00000046
}