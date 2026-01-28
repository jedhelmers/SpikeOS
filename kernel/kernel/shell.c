#include <kernel/shell.h>

#include <stdio.h>

void shell_readline(void) {
    char c = 0;

    while (c != '\n') {
        c = keyboard_getchar();

        if (c) {
            putchar(c);
        }
    }
}

void shell_execute(void) {}

void shell_run(void) {
    printf("SpikeOS shell\n");

    while (1) {
        printf("> ");
        shell_readline();
        shell_execute();
    }
}