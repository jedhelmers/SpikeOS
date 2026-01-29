#include <kernel/shell.h>
#include <stdio.h>

#include <kernel/tty.h>

#define LINE_BUF_SIZE 128

static char line_buf[LINE_BUF_SIZE];
static int line_len = 0;

void shell_clear(void) {
    terminal_clear();
    shell_init_prefix();
}

void shell_readline(void) {
    line_len = 0;

    while (1) {
        key_event_t c = keyboard_get_event();

        if (c.type == KEY_NONE) {
            // Wait for IRQ
            __asm__ volatile ("hlt");

            continue;
        }

        if (c.type == KEY_ENTER) {
            putchar('\n');
            line_buf[line_len] = 0;

            return;
        }

        if (c.type == KEY_BACKSPACE) {
            if (line_len > 0) {
                line_len--;

                printf("\b \b");

            }

            // shell_clear();
            continue;
        }

        if (line_len < LINE_BUF_SIZE - 1) {
            line_buf[line_len++] = c.ch;

            putchar(c.ch);
        }
    }
}

void shell_execute(void) {
    // printf("cmd: %s\n", line_buf);
}

void shell_init_prefix(void) {
    printf("jedhelmers> ");
}

void shell_run(void) {
    printf("\nbooting shell...\n\n");

    while (1) {
        shell_init_prefix();
        shell_readline();
        shell_execute();
    }
}