#include <kernel/shell.h>
#include <kernel/process.h>
#include <kernel/tetris.h>
#include <kernel/heap.h>
#include <kernel/initrd.h>
#include <kernel/elf.h>
#include <stdio.h>
#include <string.h>

#include <kernel/tty.h>

#define LINE_BUF_SIZE 128

static char line_buf[LINE_BUF_SIZE];
static int line_len = 0;
static uint32_t foreground_pid = 0;

extern void thread_inc(void);

/* Parse a decimal string to uint32 */
static uint32_t parse_uint(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

void shell_clear(void) {
    terminal_clear();
    shell_init_prefix();
}

void shell_readline(void) {
    line_len = 0;

    while (1) {
        key_event_t c = keyboard_get_event();

        if (c.type == KEY_NONE) {
            __asm__ volatile ("hlt");
            continue;
        }

        if (c.type == KEY_CTRL_C) {
            printf("^C\n");
            if (foreground_pid != 0) {
                proc_kill(foreground_pid);
                foreground_pid = 0;
            }
            line_len = 0;
            return;
        }

        if (c.type == KEY_ENTER) {
            putchar('\n');
            line_buf[line_len] = '\0';
            return;
        }

        if (c.type == KEY_BACKSPACE) {
            if (line_len > 0) {
                line_len--;
                printf("\b \b");
            }
            continue;
        }

        if (line_len < LINE_BUF_SIZE - 1) {
            line_buf[line_len++] = c.ch;
            putchar(c.ch);
        }
    }
}

void shell_execute(void) {
    if (line_len == 0) return;

    if (strcmp(line_buf, "help") == 0) {
        printf("Commands:\n");
        printf("  help         - show this help\n");
        printf("  ls           - list files in initrd\n");
        printf("  exec <name>  - run ELF binary from initrd\n");
        printf("  run          - start thread_inc\n");
        printf("  run tetris   - play Tetris (WASD=move, Space=drop, Q=quit)\n");
        printf("  ps           - list processes\n");
        printf("  kill <pid>   - kill process by PID\n");
        printf("  meminfo      - show heap info\n");
        printf("  clear        - clear screen\n");
    }
    else if (strcmp(line_buf, "run tetris") == 0) {
        tetris_run();
    }
    else if (strcmp(line_buf, "run") == 0) {
        struct process *p = proc_create_kernel_thread(thread_inc);
        if (p) {
            foreground_pid = p->pid;
            printf("Started thread_inc [PID %d] - Ctrl+C to stop\n", p->pid);
        } else {
            printf("Error: process table full\n");
        }
    }
    else if (strcmp(line_buf, "ps") == 0) {
        static const char *state_names[] = {
            "NEW", "READY", "RUNNING", "BLOCKED", "ZOMBIE"
        };
        printf("PID  STATE\n");
        for (int i = 0; i < MAX_PROCS; i++) {
            if (proc_table[i].state != PROC_ZOMBIE) {
                printf("%d    %s\n", proc_table[i].pid,
                       state_names[proc_table[i].state]);
            }
        }
    }
    else if (strncmp(line_buf, "kill ", 5) == 0) {
        uint32_t pid = parse_uint(line_buf + 5);
        if (pid == foreground_pid) foreground_pid = 0;
        proc_kill(pid);
    }
    else if (strcmp(line_buf, "meminfo") == 0) {
        heap_dump();
    }
    else if (strcmp(line_buf, "ls") == 0) {
        initrd_list();
    }
    else if (strncmp(line_buf, "exec ", 5) == 0) {
        const char *name = line_buf + 5;
        uint32_t file_phys, file_size;
        if (initrd_find(name, &file_phys, &file_size) != 0) {
            printf("File not found: '%s'\n", name);
        } else {
            struct process *p = elf_load_and_exec(file_phys, file_size);
            if (p) {
                printf("Started '%s' [PID %d]\n", name, p->pid);
            } else {
                printf("Failed to load '%s'\n", name);
            }
        }
    }
    else if (strcmp(line_buf, "clear") == 0) {
        shell_clear();
    }
    else {
        printf("Unknown command: '%s' (type 'help')\n", line_buf);
    }
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
