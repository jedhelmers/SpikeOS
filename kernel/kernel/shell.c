#include <kernel/shell.h>
#include <kernel/process.h>
#include <kernel/tetris.h>
#include <kernel/heap.h>
#include <kernel/initrd.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/spikefs.h>
#include <kernel/timer.h>
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

/* Skip whitespace and return pointer to first arg after cmd_len chars.
   Returns NULL if no argument found. */
static const char *shell_arg(const char *line, int cmd_len) {
    const char *p = line + cmd_len;
    while (*p == ' ') p++;
    return (*p) ? p : (const char *)0;
}

/* Split args string into two space-separated arguments.
   Writes first arg into arg1_buf, sets *arg2 to second arg (or NULL). */
static char split_buf[LINE_BUF_SIZE];
static void shell_split_args(const char *args, const char **arg1, const char **arg2) {
    const char *space = args;
    while (*space && *space != ' ') space++;

    int len = space - args;
    if (len >= LINE_BUF_SIZE) len = LINE_BUF_SIZE - 1;
    memcpy(split_buf, args, len);
    split_buf[len] = '\0';
    *arg1 = split_buf;

    if (*space) {
        space++;
        while (*space == ' ') space++;
        *arg2 = (*space) ? space : (const char *)0;
    } else {
        *arg2 = (const char *)0;
    }
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

    /* ---- help ---- */
    if (strcmp(line_buf, "help") == 0) {
        printf("Commands:\n");
        printf("  help           - show this help\n");
        printf("  pwd            - print working directory\n");
        printf("  ls [path]      - list directory contents\n");
        printf("  cd <path>      - change directory\n");
        printf("  mkdir <name>   - create directory\n");
        printf("  touch <name>   - create empty file\n");
        printf("  rm <name>      - remove file or empty directory\n");
        printf("  cat <name>     - display file contents\n");
        printf("  write <n> <t>  - write text to file\n");
        printf("  mv <src> <dst> - move/rename\n");
        printf("  cp <src> <dst> - copy file\n");
        printf("  sync           - save filesystem to disk\n");
        printf("  format         - reformat disk (erases all data!)\n");
        printf("  exec <name>    - run ELF binary from initrd\n");
        printf("  run            - start thread_inc\n");
        printf("  run tetris     - play Tetris (WASD=move, Space=drop, Q=quit)\n");
        printf("  ps             - list processes\n");
        printf("  kill <pid>     - kill process by PID\n");
        printf("  meminfo        - show heap info\n");
        printf("  clear          - clear screen\n");
    }

    /* ---- pwd ---- */
    else if (strcmp(line_buf, "pwd") == 0) {
        printf("%s\n", vfs_get_cwd_path());
    }

    /* ---- cd ---- */
    else if (strcmp(line_buf, "cd") == 0) {
        vfs_chdir("/");
    }
    else if (strncmp(line_buf, "cd ", 3) == 0) {
        const char *path = shell_arg(line_buf, 2);
        if (!path) {
            vfs_chdir("/");
        } else if (vfs_chdir(path) != 0) {
            printf("cd: no such directory: '%s'\n", path);
        }
    }

    /* ---- ls ---- */
    else if (strcmp(line_buf, "ls") == 0) {
        vfs_list(vfs_get_cwd());
    }
    else if (strncmp(line_buf, "ls ", 3) == 0) {
        const char *path = shell_arg(line_buf, 2);
        if (!path) {
            vfs_list(vfs_get_cwd());
        } else {
            int32_t ino = vfs_resolve(path, (uint32_t *)0, (char *)0);
            if (ino < 0) {
                printf("ls: not found: '%s'\n", path);
            } else {
                vfs_list((uint32_t)ino);
            }
        }
    }

    /* ---- mkdir ---- */
    else if (strncmp(line_buf, "mkdir ", 6) == 0) {
        const char *name = shell_arg(line_buf, 5);
        if (!name) {
            printf("Usage: mkdir <name>\n");
        } else if (vfs_mkdir(name) < 0) {
            printf("mkdir: failed to create '%s'\n", name);
        }
    }

    /* ---- touch ---- */
    else if (strncmp(line_buf, "touch ", 6) == 0) {
        const char *name = shell_arg(line_buf, 5);
        if (!name) {
            printf("Usage: touch <name>\n");
        } else if (vfs_create_file(name) < 0) {
            printf("touch: failed to create '%s'\n", name);
        }
    }

    /* ---- rm ---- */
    else if (strncmp(line_buf, "rm ", 3) == 0) {
        const char *name = shell_arg(line_buf, 2);
        if (!name) {
            printf("Usage: rm <name>\n");
        } else if (vfs_remove(name) != 0) {
            printf("rm: failed to remove '%s'\n", name);
        }
    }

    /* ---- cat ---- */
    else if (strncmp(line_buf, "cat ", 4) == 0) {
        const char *name = shell_arg(line_buf, 3);
        if (!name) {
            printf("Usage: cat <name>\n");
        } else {
            int32_t ino = vfs_resolve(name, (uint32_t *)0, (char *)0);
            if (ino < 0) {
                printf("cat: not found: '%s'\n", name);
            } else {
                vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
                if (!node || node->type != VFS_TYPE_FILE) {
                    printf("cat: not a file: '%s'\n", name);
                } else if (node->size > 0) {
                    uint8_t *data = (uint8_t *)node->data;
                    for (uint32_t i = 0; i < node->size; i++)
                        putchar(data[i]);
                    if (data[node->size - 1] != '\n')
                        putchar('\n');
                }
            }
        }
    }

    /* ---- write <name> <text> ---- */
    else if (strncmp(line_buf, "write ", 6) == 0) {
        const char *args = shell_arg(line_buf, 5);
        if (!args) {
            printf("Usage: write <name> <text>\n");
        } else {
            /* Extract filename (first token) */
            const char *name_end = args;
            while (*name_end && *name_end != ' ') name_end++;
            char fname[VFS_MAX_NAME + 1];
            int nlen = name_end - args;
            if (nlen > VFS_MAX_NAME) nlen = VFS_MAX_NAME;
            memcpy(fname, args, nlen);
            fname[nlen] = '\0';

            const char *text = name_end;
            while (*text == ' ') text++;
            if (*text == '\0') {
                printf("Usage: write <name> <text>\n");
            } else {
                int32_t ino = vfs_resolve(fname, (uint32_t *)0, (char *)0);
                if (ino < 0) {
                    /* Auto-create the file */
                    ino = vfs_create_file(fname);
                    if (ino < 0) {
                        printf("write: failed to create '%s'\n", fname);
                    }
                }
                if (ino >= 0) {
                    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
                    if (!node || node->type != VFS_TYPE_FILE) {
                        printf("write: '%s' is not a file\n", fname);
                    } else {
                        uint32_t len = strlen(text);
                        /* Overwrite from beginning */
                        vfs_write((uint32_t)ino, text, 0, len);
                        /* Add trailing newline */
                        char nl = '\n';
                        vfs_write((uint32_t)ino, &nl, len, 1);
                        /* Truncate if file was longer before */
                        node->size = len + 1;
                    }
                }
            }
        }
    }

    /* ---- mv <src> <dst> ---- */
    else if (strncmp(line_buf, "mv ", 3) == 0) {
        const char *args = shell_arg(line_buf, 2);
        if (!args) {
            printf("Usage: mv <src> <dst>\n");
        } else {
            const char *src, *dst;
            shell_split_args(args, &src, &dst);
            if (!dst) {
                printf("Usage: mv <src> <dst>\n");
            } else if (vfs_rename(src, dst) != 0) {
                printf("mv: failed\n");
            }
        }
    }

    /* ---- cp <src> <dst> ---- */
    else if (strncmp(line_buf, "cp ", 3) == 0) {
        const char *args = shell_arg(line_buf, 2);
        if (!args) {
            printf("Usage: cp <src> <dst>\n");
        } else {
            const char *src, *dst;
            shell_split_args(args, &src, &dst);
            if (!dst) {
                printf("Usage: cp <src> <dst>\n");
            } else if (vfs_copy(src, dst) < 0) {
                printf("cp: failed\n");
            }
        }
    }

    /* ---- sync ---- */
    else if (strcmp(line_buf, "sync") == 0) {
        if (spikefs_sync() != 0)
            printf("sync: failed to write to disk\n");
    }

    /* ---- format ---- */
    else if (strcmp(line_buf, "format") == 0) {
        printf("Formatting disk... all data will be lost!\n");
        if (spikefs_format() != 0) {
            printf("format: failed\n");
        } else if (spikefs_sync() != 0) {
            printf("format: sync failed\n");
        }
    }

    /* ---- run tetris ---- */
    else if (strcmp(line_buf, "run tetris") == 0) {
        tetris_run();
    }
    /* ---- run ---- */
    else if (strcmp(line_buf, "run") == 0) {
        struct process *p = proc_create_kernel_thread(thread_inc);
        if (p) {
            foreground_pid = p->pid;
            printf("Started thread_inc [PID %d] - Ctrl+C to stop\n", p->pid);
        } else {
            printf("Error: process table full\n");
        }
    }

    /* ---- ps ---- */
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

    /* ---- kill ---- */
    else if (strncmp(line_buf, "kill ", 5) == 0) {
        uint32_t pid = parse_uint(line_buf + 5);
        if (pid == foreground_pid) foreground_pid = 0;
        proc_kill(pid);
    }

    /* ---- meminfo ---- */
    else if (strcmp(line_buf, "meminfo") == 0) {
        heap_dump();
    }

    /* ---- exec ---- */
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

    /* ---- clear ---- */
    else if (strcmp(line_buf, "clear") == 0) {
        shell_clear();
    }

    else {
        printf("Unknown command: '%s' (type 'help')\n", line_buf);
    }
}

static uint32_t last_sync_tick = 0;

#define SYNC_INTERVAL_TICKS 500  /* 5 seconds at 100Hz */

void shell_init_prefix(void) {
    /* Auto write-back: sync dirty VFS to disk every 5 seconds */
    uint32_t now = timer_ticks();
    if (vfs_is_dirty() && (now - last_sync_tick) >= SYNC_INTERVAL_TICKS) {
        spikefs_sync();
        last_sync_tick = now;
    }
    printf("jedhelmers:%s> ", vfs_get_cwd_path());
}

void shell_run(void) {
    printf("\nbooting shell...\n\n");

    while (1) {
        shell_init_prefix();
        shell_readline();
        shell_execute();
    }
}
