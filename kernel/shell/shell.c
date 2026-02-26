#include <kernel/shell.h>
#include <kernel/process.h>
#include <kernel/tetris.h>
#include <kernel/editor.h>
#include <kernel/finder.h>
#include <kernel/heap.h>
#include <kernel/initrd.h>
#include <kernel/elf.h>
#include <kernel/vfs.h>
#include <kernel/spikefs.h>
#include <kernel/timer.h>
#include <kernel/fd.h>
#include <kernel/pipe.h>
#include <kernel/hal.h>
#include <kernel/signal.h>
#include <kernel/mutex.h>
#include <kernel/condvar.h>
#include <kernel/rwlock.h>
#include <kernel/event.h>
#include <kernel/mouse.h>
#include <kernel/window.h>
#include <kernel/fb_console.h>
#include <stdio.h>
#include <string.h>

#include <kernel/tty.h>
#include <kernel/pci.h>
#include <kernel/e1000.h>
#include <kernel/net.h>

#define LINE_BUF_SIZE 128

static char line_buf[LINE_BUF_SIZE];
static int line_len = 0;
#define MAX_FG_PIDS 4
static uint32_t fg_pids[MAX_FG_PIDS];
static int fg_count = 0;

extern void thread_inc(void);

/* ================================================================== */
/*  Output capture (redirect terminal_write to buffer)                */
/* ================================================================== */

static char    *redir_buf  = NULL;
static uint32_t redir_size = 0;
static uint32_t redir_cap  = 0;
#define REDIR_MAX_CAP  (64 * 1024)

static void redir_sink(const char *data, size_t size) {
    uint32_t need = redir_size + (uint32_t)size;
    if (need > REDIR_MAX_CAP) return;
    if (need > redir_cap) {
        uint32_t nc = redir_cap * 2;
        if (nc < need) nc = need;
        if (nc < 1024) nc = 1024;
        char *nb = krealloc(redir_buf, nc);
        if (!nb) return;
        redir_buf = nb;
        redir_cap = nc;
    }
    memcpy(redir_buf + redir_size, data, size);
    redir_size = need;
}

static void capture_start(void) {
    redir_buf = kmalloc(1024);
    redir_size = 0;
    redir_cap = redir_buf ? 1024 : 0;
    terminal_set_redirect(redir_sink);
}

static void capture_stop(char **out, uint32_t *len) {
    terminal_set_redirect((terminal_redirect_fn)0);
    *out = redir_buf;
    *len = redir_size;
    redir_buf = NULL;
    redir_size = 0;
    redir_cap = 0;
}

/* ================================================================== */
/*  Piped stdin buffer                                                 */
/* ================================================================== */

static const char *stdin_buf = NULL;
static uint32_t    stdin_pos = 0;
static uint32_t    stdin_len = 0;

static void shell_stdin_set(const char *buf, uint32_t len) {
    stdin_buf = buf;
    stdin_pos = 0;
    stdin_len = len;
}

static void shell_stdin_clear(void) {
    stdin_buf = NULL;
    stdin_pos = 0;
    stdin_len = 0;
}

/* ================================================================== */
/*  Shell variables                                                    */
/* ================================================================== */

#define MAX_SHELL_VARS 32
#define VAR_NAME_MAX   31
#define VAR_VALUE_MAX  127

typedef struct {
    char name[VAR_NAME_MAX + 1];
    char value[VAR_VALUE_MAX + 1];
    int  exported;
} shell_var_t;

static shell_var_t shell_vars[MAX_SHELL_VARS];
static int num_vars = 0;

static const char *shell_getvar(const char *name) {
    for (int i = 0; i < num_vars; i++)
        if (strcmp(shell_vars[i].name, name) == 0)
            return shell_vars[i].value;
    return (const char *)0;
}

static void shell_setvar(const char *name, const char *value) {
    for (int i = 0; i < num_vars; i++) {
        if (strcmp(shell_vars[i].name, name) == 0) {
            strncpy(shell_vars[i].value, value, VAR_VALUE_MAX);
            shell_vars[i].value[VAR_VALUE_MAX] = '\0';
            return;
        }
    }
    if (num_vars >= MAX_SHELL_VARS) return;
    strncpy(shell_vars[num_vars].name, name, VAR_NAME_MAX);
    shell_vars[num_vars].name[VAR_NAME_MAX] = '\0';
    strncpy(shell_vars[num_vars].value, value, VAR_VALUE_MAX);
    shell_vars[num_vars].value[VAR_VALUE_MAX] = '\0';
    shell_vars[num_vars].exported = 0;
    num_vars++;
}

static int is_varname_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_varname_char(char c) {
    return is_varname_start(c) || (c >= '0' && c <= '9');
}

static void shell_expand_vars(const char *input, char *output, uint32_t cap) {
    uint32_t out = 0;
    while (*input && out < cap - 1) {
        if (*input == '$' && is_varname_start(input[1])) {
            input++;
            char vname[VAR_NAME_MAX + 1];
            int vn = 0;
            while (*input && is_varname_char(*input) && vn < VAR_NAME_MAX)
                vname[vn++] = *input++;
            vname[vn] = '\0';

            const char *val = (const char *)0;
            if (strcmp(vname, "PWD") == 0)
                val = vfs_get_cwd_path();
            else if (strcmp(vname, "USER") == 0)
                val = "jedhelmers";
            else if (strcmp(vname, "HOME") == 0)
                val = "/";
            else
                val = shell_getvar(vname);

            if (val) {
                uint32_t vlen = strlen(val);
                if (out + vlen < cap - 1) {
                    memcpy(output + out, val, vlen);
                    out += vlen;
                }
            }
        } else {
            output[out++] = *input++;
        }
    }
    output[out] = '\0';
}

/* Check for variable assignment: NAME=VALUE (no spaces before =) */
static int try_var_assignment(const char *line) {
    if (!is_varname_start(line[0])) return 0;
    const char *eq = strchr(line, '=');
    if (!eq || eq == line) return 0;
    /* Make sure no spaces before = (otherwise it's a command) */
    for (const char *p = line; p < eq; p++)
        if (!is_varname_char(*p)) return 0;

    char name[VAR_NAME_MAX + 1];
    int nlen = eq - line;
    if (nlen > VAR_NAME_MAX) nlen = VAR_NAME_MAX;
    memcpy(name, line, nlen);
    name[nlen] = '\0';
    shell_setvar(name, eq + 1);
    return 1;
}

/* ================================================================== */
/*  Pipeline parsing                                                   */
/* ================================================================== */

#define MAX_PIPE_STAGES 4
#define REDIR_NONE   0
#define REDIR_WRITE  1
#define REDIR_APPEND 2

typedef struct {
    char *cmd;
    int   redir_type;
    char *redir_file;
} shell_segment_t;

static shell_segment_t segments[MAX_PIPE_STAGES];
static int num_segments;

static void shell_parse_line(char *line) {
    num_segments = 0;
    segments[0].cmd = line;
    segments[0].redir_type = REDIR_NONE;
    segments[0].redir_file = (char *)0;
    num_segments = 1;

    char *p = line;
    while (*p) {
        if (*p == '|') {
            *p = '\0';
            p++;
            while (*p == ' ') p++;
            if (num_segments < MAX_PIPE_STAGES) {
                segments[num_segments].cmd = p;
                segments[num_segments].redir_type = REDIR_NONE;
                segments[num_segments].redir_file = (char *)0;
                num_segments++;
            }
        } else {
            p++;
        }
    }

    /* Check last segment for > or >> */
    shell_segment_t *last = &segments[num_segments - 1];
    char *gt = strchr(last->cmd, '>');
    if (gt) {
        if (*(gt + 1) == '>') {
            last->redir_type = REDIR_APPEND;
            *gt = '\0';
            gt += 2;
        } else {
            last->redir_type = REDIR_WRITE;
            *gt = '\0';
            gt += 1;
        }
        while (*gt == ' ') gt++;
        last->redir_file = gt;
        /* Trim trailing spaces from filename */
        int flen = strlen(gt);
        while (flen > 0 && gt[flen - 1] == ' ') gt[--flen] = '\0';
    }

    /* Trim leading/trailing whitespace from each segment command */
    for (int i = 0; i < num_segments; i++) {
        char *s = segments[i].cmd;
        while (*s == ' ') s++;
        segments[i].cmd = s;
        int slen = strlen(s);
        while (slen > 0 && s[slen - 1] == ' ') s[--slen] = '\0';
    }
}

/* Write captured output to a file */
static void shell_write_to_file(const char *filename, int redir_type,
                                const char *data, uint32_t len) {
    if (!filename || !*filename) {
        printf("Error: missing filename for redirect\n");
        return;
    }
    uint32_t flags = O_WRONLY | O_CREAT;
    if (redir_type == REDIR_APPEND) flags |= O_APPEND;
    else flags |= O_TRUNC;
    int fd = fd_open(filename, flags);
    if (fd < 0) {
        printf("Error: cannot open '%s'\n", filename);
        return;
    }
    if (len > 0) fd_write(fd, data, len);
    fd_close(fd);
}

/* ================================================================== */
/*  Pipe-aware command helpers: grep, wc, head, tail                   */
/* ================================================================== */

static void shell_grep_data(const char *data, uint32_t size,
                            const char *pattern) {
    if (!data || !size || !pattern || !*pattern) return;
    uint32_t plen = strlen(pattern);
    const char *p = data;
    const char *end = data + size;

    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;
        uint32_t llen = eol - p;

        int found = 0;
        if (llen >= plen) {
            for (uint32_t i = 0; i + plen <= llen; i++) {
                if (memcmp(p + i, pattern, plen) == 0) {
                    found = 1;
                    break;
                }
            }
        }
        if (found) {
            for (uint32_t i = 0; i < llen; i++) putchar(p[i]);
            putchar('\n');
        }
        p = eol;
        if (p < end && *p == '\n') p++;
    }
}

static void shell_wc_data(const char *data, uint32_t size, const char *label) {
    if (!data) { printf("  0  0  0\n"); return; }
    uint32_t lines = 0, words = 0;
    int in_word = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (data[i] == '\n') lines++;
        if (data[i] == ' ' || data[i] == '\n' || data[i] == '\t')
            in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    printf("  %d  %d  %d", lines, words, size);
    if (label) printf("  %s", label);
    printf("\n");
}

static void shell_head_data(const char *data, uint32_t size, int n) {
    if (!data || !size) return;
    const char *p = data;
    const char *end = data + size;
    int count = 0;
    while (p < end && count < n) {
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;
        for (const char *c = p; c < eol; c++) putchar(*c);
        putchar('\n');
        count++;
        p = eol;
        if (p < end && *p == '\n') p++;
    }
}

static void shell_tail_data(const char *data, uint32_t size, int n) {
    if (!data || !size) return;
    int total = 0;
    for (uint32_t i = 0; i < size; i++)
        if (data[i] == '\n') total++;
    if (size > 0 && data[size - 1] != '\n') total++;

    int skip = total - n;
    if (skip < 0) skip = 0;

    const char *p = data;
    const char *end = data + size;
    int count = 0;
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;
        if (count >= skip) {
            for (const char *c = p; c < eol; c++) putchar(*c);
            putchar('\n');
        }
        count++;
        p = eol;
        if (p < end && *p == '\n') p++;
    }
}

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

/* Check if the shell window has been closed. If so, clean up and exit thread. */
static void shell_check_close(void) {
    window_t *sw = wm_get_shell_window();
    if (sw && (sw->flags & WIN_FLAG_CLOSE_REQ)) {
        fb_console_bind_window((window_t *)0);
        wm_destroy_window(sw);
        proc_kill(current_process->pid);
        for (;;) __asm__ volatile("hlt");
    }
}

void shell_readline(void) {
    line_len = 0;

    while (1) {
        wm_process_events();
        shell_check_close();

        /* Focus-gated keyboard: only consume keys when shell window is focused */
        window_t *sw = wm_get_shell_window();

        /* Scroll wheel → scroll shell history */
        if (sw && sw->scroll_accum != 0) {
            int32_t dz = sw->scroll_accum;
            sw->scroll_accum = 0;
            terminal_scroll_lines(dz * 3);
        }

        if (sw && !(sw->flags & WIN_FLAG_FOCUSED)) {
            __asm__ volatile ("hlt");
            continue;
        }

        key_event_t c = keyboard_get_event();

        if (c.type == KEY_NONE) {
            __asm__ volatile ("hlt");
            continue;
        }

        if (c.type == KEY_PAGE_UP) {
            terminal_page_up();
            continue;
        }

        if (c.type == KEY_PAGE_DOWN) {
            terminal_page_down();
            continue;
        }

        if (c.type == KEY_CTRL_C) {
            printf("^C\n");
            for (int i = 0; i < fg_count; i++)
                proc_kill(fg_pids[i]);
            fg_count = 0;
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

/* ================================================================== */
/*  Shell test commands                                               */
/* ================================================================== */

static int test_fd(void) {
    int pass = 1;

    printf("  fd_open  /_test_fd (O_CREAT|O_RDWR)... ");
    int fd = fd_open("/_test_fd", O_CREAT | O_RDWR);
    if (fd >= 0) { printf("[PASS] fd=%d\n", fd); }
    else         { printf("[FAIL] returned %d\n", fd); return 0; }

    printf("  fd_write 'hello'... ");
    int32_t w = fd_write(fd, "hello", 5);
    if (w == 5) { printf("[PASS] wrote %d bytes\n", w); }
    else        { printf("[FAIL] wrote %d bytes\n", w); pass = 0; }

    printf("  fd_seek  to 0... ");
    int32_t s = fd_seek(fd, 0, SEEK_SET);
    if (s == 0) { printf("[PASS]\n"); }
    else        { printf("[FAIL] offset=%d\n", s); pass = 0; }

    printf("  fd_read  5 bytes... ");
    char buf[16] = {0};
    int32_t r = fd_read(fd, buf, 5);
    if (r == 5 && memcmp(buf, "hello", 5) == 0) {
        printf("[PASS] got '%s'\n", buf);
    } else {
        printf("[FAIL] read %d bytes, got '%s'\n", r, buf);
        pass = 0;
    }

    printf("  fd_close... ");
    if (fd_close(fd) == 0) { printf("[PASS]\n"); }
    else                    { printf("[FAIL]\n"); pass = 0; }

    vfs_remove("/_test_fd");
    return pass;
}

static int test_pipe(void) {
    int pass = 1;

    printf("  pipe_create... ");
    int rfd, wfd;
    if (pipe_create(&rfd, &wfd) == 0) {
        printf("[PASS] rfd=%d wfd=%d\n", rfd, wfd);
    } else {
        printf("[FAIL]\n");
        return 0;
    }

    printf("  fd_write 'pipe!'... ");
    int32_t w = fd_write(wfd, "pipe!", 5);
    if (w == 5) { printf("[PASS]\n"); }
    else        { printf("[FAIL] wrote %d\n", w); pass = 0; }

    printf("  fd_read  5 bytes... ");
    char buf[16] = {0};
    int32_t r = fd_read(rfd, buf, 5);
    if (r == 5 && memcmp(buf, "pipe!", 5) == 0) {
        printf("[PASS] got '%s'\n", buf);
    } else {
        printf("[FAIL] read %d, got '%s'\n", r, buf);
        pass = 0;
    }

    fd_close(rfd);
    fd_close(wfd);
    return pass;
}

static int test_sleep(void) {
    printf("  sleeping 100 ticks (1 sec)... ");
    uint32_t before = timer_ticks();

    uint32_t target = before + 100;
    while (timer_ticks() < target) {
        hal_irq_enable();
        hal_halt();
    }

    uint32_t after = timer_ticks();
    uint32_t elapsed = after - before;

    if (elapsed >= 100) {
        printf("[PASS] elapsed=%d ticks\n", elapsed);
        return 1;
    } else {
        printf("[FAIL] elapsed=%d ticks (expected >= 100)\n", elapsed);
        return 0;
    }
}

static int test_stat(void) {
    int pass = 1;

    printf("  create /_test_stat... ");
    int32_t ino = vfs_create_file("/_test_stat");
    if (ino >= 0) { printf("[PASS] ino=%d\n", ino); }
    else          { printf("[FAIL]\n"); return 0; }

    const char *data = "test data";
    vfs_write((uint32_t)ino, data, 0, 9);

    printf("  check type/size... ");
    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
    if (node && node->type == VFS_TYPE_FILE && node->size == 9) {
        printf("[PASS] type=%d size=%d\n", node->type, node->size);
    } else {
        printf("[FAIL] type=%d size=%d\n",
               node ? node->type : -1, node ? node->size : 0);
        pass = 0;
    }

    vfs_remove("/_test_stat");
    return pass;
}

static int test_stdin(void) {
    printf("  Press a key (blocking fd_read on stdin)... ");
    char ch;
    int32_t r = fd_read(0, &ch, 1);
    if (r == 1) {
        printf("[PASS] got '%c' (0x%x)\n", ch >= 32 ? ch : '.', (uint8_t)ch);
        return 1;
    } else {
        printf("[FAIL] fd_read returned %d\n", r);
        return 0;
    }
}

static int test_mutex(void) {
    int pass = 1;

    printf("  mutex_init... ");
    mutex_t m = MUTEX_INIT;
    printf("[PASS]\n");

    printf("  mutex_lock... ");
    mutex_lock(&m);
    if (m.locked && m.owner == current_process) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    printf("  mutex_trylock (should fail)... ");
    if (mutex_trylock(&m) == 0) { printf("[PASS] correctly returned 0\n"); }
    else { printf("[FAIL] acquired already-locked mutex\n"); pass = 0; }

    printf("  mutex_unlock... ");
    mutex_unlock(&m);
    if (!m.locked && !m.owner) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    printf("  mutex_trylock (should succeed)... ");
    if (mutex_trylock(&m) == 1) { printf("[PASS]\n"); mutex_unlock(&m); }
    else { printf("[FAIL]\n"); pass = 0; }

    return pass;
}

static int test_semaphore(void) {
    int pass = 1;

    printf("  sem_init(2)... ");
    semaphore_t s;
    sem_init(&s, 2);
    if (s.count == 2) { printf("[PASS]\n"); }
    else { printf("[FAIL] count=%d\n", s.count); pass = 0; }

    printf("  sem_wait (2->1)... ");
    sem_wait(&s);
    if (s.count == 1) { printf("[PASS] count=%d\n", s.count); }
    else { printf("[FAIL] count=%d\n", s.count); pass = 0; }

    printf("  sem_wait (1->0)... ");
    sem_wait(&s);
    if (s.count == 0) { printf("[PASS] count=%d\n", s.count); }
    else { printf("[FAIL] count=%d\n", s.count); pass = 0; }

    printf("  sem_trywait (should fail at 0)... ");
    if (sem_trywait(&s) == 0) { printf("[PASS] correctly returned 0\n"); }
    else { printf("[FAIL] acquired at count=0\n"); pass = 0; }

    printf("  sem_post (0->1)... ");
    sem_post(&s);
    if (s.count == 1) { printf("[PASS] count=%d\n", s.count); }
    else { printf("[FAIL] count=%d\n", s.count); pass = 0; }

    printf("  sem_trywait (should succeed at 1)... ");
    if (sem_trywait(&s) == 1) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    return pass;
}

static int test_signal(void) {
    int pass = 1;

    printf("  proc_signal on non-existent PID... ");
    if (proc_signal(9999, SIGKILL) != 0) { printf("[PASS] returned -1\n"); }
    else { printf("[FAIL] should have failed\n"); pass = 0; }

    printf("  SIG_BIT(SIGKILL) = 0x%x... ", SIG_BIT(SIGKILL));
    if (SIG_BIT(SIGKILL) == (1u << 8)) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    printf("  SIG_BIT(SIGSEGV) = 0x%x... ", SIG_BIT(SIGSEGV));
    if (SIG_BIT(SIGSEGV) == (1u << 10)) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    printf("  SIG_BIT(SIGPIPE) = 0x%x... ", SIG_BIT(SIGPIPE));
    if (SIG_BIT(SIGPIPE) == (1u << 12)) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    return pass;
}

static int test_cwd(void) {
    int pass = 1;

    printf("  save original cwd... ");
    uint32_t saved_cwd = vfs_get_cwd();
    printf("[PASS] inode=%d\n", saved_cwd);

    printf("  mkdir /_test_cwd... ");
    int32_t ino = vfs_mkdir("/_test_cwd");
    if (ino >= 0) { printf("[PASS] ino=%d\n", ino); }
    else { printf("[FAIL]\n"); return 0; }

    printf("  cd /_test_cwd... ");
    if (vfs_chdir("/_test_cwd") == 0) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    printf("  verify cwd path... ");
    const char *path = vfs_get_cwd_path();
    if (strcmp(path, "/_test_cwd") == 0) { printf("[PASS] '%s'\n", path); }
    else { printf("[FAIL] got '%s'\n", path); pass = 0; }

    printf("  cd / (restore)... ");
    vfs_chdir("/");
    if (vfs_get_cwd() == 0) { printf("[PASS]\n"); }
    else { printf("[FAIL]\n"); pass = 0; }

    vfs_remove("/_test_cwd");
    return pass;
}

static int test_condvar(void) {
    int pass = 1;

    printf("  condvar_init... ");
    condvar_t cv = CONDVAR_INIT;
    printf("[PASS]\n");

    printf("  condvar_signal (empty queue)... ");
    condvar_signal(&cv);
    printf("[PASS] no crash\n");

    printf("  condvar_broadcast (empty queue)... ");
    condvar_broadcast(&cv);
    printf("[PASS] no crash\n");

    return pass;
}

static int test_rwlock(void) {
    int pass = 1;

    printf("  rwlock_init... ");
    rwlock_t rw = RWLOCK_INIT;
    printf("[PASS]\n");

    printf("  rwlock_read_lock (0->1)... ");
    rwlock_read_lock(&rw);
    if (rw.reader_count == 1) { printf("[PASS]\n"); }
    else { printf("[FAIL] count=%d\n", rw.reader_count); pass = 0; }

    printf("  rwlock_read_lock (1->2)... ");
    rwlock_read_lock(&rw);
    if (rw.reader_count == 2) { printf("[PASS]\n"); }
    else { printf("[FAIL] count=%d\n", rw.reader_count); pass = 0; }

    printf("  rwlock_read_unlock (2->1)... ");
    rwlock_read_unlock(&rw);
    if (rw.reader_count == 1) { printf("[PASS]\n"); }
    else { printf("[FAIL] count=%d\n", rw.reader_count); pass = 0; }

    printf("  rwlock_read_unlock (1->0)... ");
    rwlock_read_unlock(&rw);
    if (rw.reader_count == 0) { printf("[PASS]\n"); }
    else { printf("[FAIL] count=%d\n", rw.reader_count); pass = 0; }

    printf("  rwlock_write_lock... ");
    rwlock_write_lock(&rw);
    if (rw.writer_active == 1) { printf("[PASS]\n"); }
    else { printf("[FAIL] writer_active=%d\n", rw.writer_active); pass = 0; }

    printf("  rwlock_write_unlock... ");
    rwlock_write_unlock(&rw);
    if (rw.writer_active == 0) { printf("[PASS]\n"); }
    else { printf("[FAIL] writer_active=%d\n", rw.writer_active); pass = 0; }

    return pass;
}

static int test_mouse(void) {
    int pass = 1;

    printf("  mouse_get_state()... ");
    mouse_state_t ms = mouse_get_state();
    printf("[PASS] x=%d y=%d buttons=0x%x\n", ms.x, ms.y, ms.buttons);

    printf("  Move the mouse or click within 5 seconds...\n");

    uint32_t timeout = timer_ticks() + 500;  /* 5 seconds at 100Hz */
    int got_mouse = 0;

    while (timer_ticks() < timeout) {
        event_t e = event_poll();
        if (e.type == EVENT_MOUSE_MOVE) {
            printf("  [PASS] MOUSE_MOVE x=%d y=%d dx=%d dy=%d\n",
                   e.mouse_move.x, e.mouse_move.y,
                   e.mouse_move.dx, e.mouse_move.dy);
            got_mouse = 1;
            break;
        } else if (e.type == EVENT_MOUSE_BUTTON) {
            printf("  [PASS] MOUSE_BUTTON x=%d y=%d btn=0x%x %s\n",
                   e.mouse_button.x, e.mouse_button.y,
                   e.mouse_button.button,
                   e.mouse_button.pressed ? "pressed" : "released");
            got_mouse = 1;
            break;
        }
        hal_irq_enable();
        hal_halt();
    }

    if (!got_mouse) {
        printf("  [FAIL] no mouse event within 5 seconds\n");
        pass = 0;
    }

    return pass;
}

static void run_tests(int which) {
    /* which: 0=all, 1=fd, 2=pipe, 3=sleep, 4=stat, 5=waitpid, 6=stdin,
             7=mutex, 8=sem, 9=signal, 10=cwd, 11=condvar, 12=rwlock,
             13=mouse */
    int total = 0, passed = 0;

    if (which == 0 || which == 1) {
        printf("[test fd]\n");
        int r = test_fd();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 2) {
        printf("[test pipe]\n");
        int r = test_pipe();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 3) {
        printf("[test sleep]\n");
        int r = test_sleep();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 4) {
        printf("[test stat]\n");
        int r = test_stat();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 5) {
        printf("[test waitpid]\n");
        printf("  spawn/waitpid requires a user-mode ELF binary.\n");
        printf("  Use 'exec <name>' to run an initrd ELF, which exercises\n");
        printf("  process creation, scheduling, and exit.\n\n");
    }
    if (which == 0 || which == 6) {
        printf("[test stdin]\n");
        int r = test_stdin();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 7) {
        printf("[test mutex]\n");
        int r = test_mutex();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 8) {
        printf("[test semaphore]\n");
        int r = test_semaphore();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 9) {
        printf("[test signal]\n");
        int r = test_signal();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 10) {
        printf("[test cwd]\n");
        int r = test_cwd();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 11) {
        printf("[test condvar]\n");
        int r = test_condvar();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 0 || which == 12) {
        printf("[test rwlock]\n");
        int r = test_rwlock();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }
    if (which == 13) {
        printf("[test mouse]\n");
        int r = test_mouse();
        total++; if (r) passed++;
        printf("  result: %s\n\n", r ? "PASS" : "FAIL");
    }

    if (which == 0) {
        printf("=== %d/%d tests passed ===\n", passed, total);
    }
}

/* ================================================================== */
/*  Concurrent demo: two threads sharing a mutex-protected counter    */
/* ================================================================== */

static volatile int shared_counter = 0;
static mutex_t counter_mutex = MUTEX_INIT;

static void thread_counter_inc(void) {
    for (;;) {
        mutex_lock(&counter_mutex);
        shared_counter++;
        printf("[INC] counter = %d\n", shared_counter);
        mutex_unlock(&counter_mutex);
        for (volatile int i = 0; i < 5000000; i++);
    }
}

static void thread_counter_dec(void) {
    for (;;) {
        mutex_lock(&counter_mutex);
        shared_counter--;
        printf("[DEC] counter = %d\n", shared_counter);
        mutex_unlock(&counter_mutex);
        for (volatile int i = 0; i < 5000000; i++);
    }
}

static void shell_execute_cmd(void) {
    if (line_len == 0) return;

    /* Check for variable assignment: NAME=VALUE */
    if (try_var_assignment(line_buf)) return;

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
        printf("  rm -r <name>   - remove directory recursively\n");
        printf("  rename <o> <n> - rename file or directory\n");
        printf("  cat <name>     - display file contents\n");
        printf("  edit <name>    - open text editor (^S save, ^X exit)\n");
        printf("  write <n> <t>  - write text to file\n");
        printf("  mv <src> <dst> - move/rename\n");
        printf("  cp <src> <dst> - copy file\n");
        printf("  sync           - save filesystem to disk\n");
        printf("  format         - reformat disk (erases all data!)\n");
        printf("  exec <name>    - run ELF binary from initrd\n");
        printf("  run            - start thread_inc\n");
        printf("  run concurrent - mutex demo: two threads inc/dec a shared counter\n");
        printf("  run tetris     - play Tetris (WASD=move, Space=drop, Q=quit)\n");
        printf("  ps             - list processes\n");
        printf("  kill <pid>     - kill process by PID\n");
        printf("  meminfo        - show heap info\n");
        printf("  lspci          - list PCI devices\n");
        printf("  netinfo        - show network interface info\n");
        printf("  ping <ip>      - send ICMP echo requests\n");
        printf("  udpsend <ip> <port> <msg> - send UDP datagram\n");
        printf("  echo [text]    - print text (supports $VAR expansion)\n");
        printf("  grep <pat> [f] - search for pattern in file or piped input\n");
        printf("  wc [file]      - count lines, words, bytes\n");
        printf("  head [-N] [f]  - show first N lines (default 10)\n");
        printf("  tail [-N] [f]  - show last N lines (default 10)\n");
        printf("  VAR=value      - set shell variable\n");
        printf("  export VAR=val - set and export variable\n");
        printf("  set            - list all shell variables\n");
        printf("  unset <name>   - remove shell variable\n");
        printf("  cmd > file     - redirect output to file\n");
        printf("  cmd >> file    - append output to file\n");
        printf("  cmd1 | cmd2    - pipe output of cmd1 to cmd2\n");
        printf("  test <name>    - run kernel tests (fd|pipe|sleep|stat|stdin|waitpid|\n");
        printf("                   mutex|sem|signal|cwd|condvar|rwlock|all)\n");
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

    /* ---- rm [-r] ---- */
    else if (strncmp(line_buf, "rm ", 3) == 0) {
        const char *args = shell_arg(line_buf, 2);
        if (!args) {
            printf("Usage: rm [-r] <name>\n");
        } else if (strncmp(args, "-r ", 3) == 0) {
            const char *name = shell_arg(args, 2);
            if (!name) {
                printf("Usage: rm -r <name>\n");
            } else if (vfs_remove_recursive(name) != 0) {
                printf("rm: failed to remove '%s'\n", name);
            }
        } else if (vfs_remove(args) != 0) {
            printf("rm: failed to remove '%s'\n", args);
        }
    }

    /* ---- cat (no args — read from piped stdin) ---- */
    else if (strcmp(line_buf, "cat") == 0) {
        if (stdin_buf && stdin_len > 0) {
            for (uint32_t i = 0; i < stdin_len; i++)
                putchar(stdin_buf[i]);
            if (stdin_buf[stdin_len - 1] != '\n')
                putchar('\n');
        } else {
            printf("Usage: cat <name>\n");
        }
    }

    /* ---- cat <name> ---- */
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

    /* ---- edit <name> ---- */
    else if (strncmp(line_buf, "edit ", 5) == 0) {
        const char *name = shell_arg(line_buf, 4);
        if (!name) {
            printf("Usage: edit <filename>\n");
        } else {
            editor_run(name);
        }
    }

    /* ---- finder [path] ---- */
    else if (strncmp(line_buf, "finder", 6) == 0 &&
             (line_buf[6] == '\0' || line_buf[6] == ' ')) {
        const char *arg = (line_buf[6] == ' ') ? shell_arg(line_buf, 6) : NULL;
        if (arg && *arg)
            finder_open(arg);
        else
            finder_open(vfs_get_cwd_path());
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

    /* ---- rename <old> <new> ---- */
    else if (strncmp(line_buf, "rename ", 7) == 0) {
        const char *args = shell_arg(line_buf, 6);
        if (!args) {
            printf("Usage: rename <old> <new>\n");
        } else {
            const char *old_name, *new_name;
            shell_split_args(args, &old_name, &new_name);
            if (!new_name) {
                printf("Usage: rename <old> <new>\n");
            } else if (vfs_rename(old_name, new_name) != 0) {
                printf("rename: failed\n");
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

    /* ---- run concurrent ---- */
    else if (strcmp(line_buf, "run concurrent") == 0) {
        shared_counter = 0;
        counter_mutex = (mutex_t)MUTEX_INIT;
        struct process *p1 = proc_create_kernel_thread(thread_counter_inc);
        struct process *p2 = proc_create_kernel_thread(thread_counter_dec);
        if (p1 && p2) {
            fg_count = 0;
            fg_pids[fg_count++] = p1->pid;
            fg_pids[fg_count++] = p2->pid;
            printf("Started concurrent threads [PID %d inc, PID %d dec] - Ctrl+C to stop\n",
                   p1->pid, p2->pid);
        } else {
            printf("Error: could not create threads\n");
            if (p1) proc_kill(p1->pid);
            if (p2) proc_kill(p2->pid);
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
            fg_count = 1;
            fg_pids[0] = p->pid;
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
        for (int i = 0; i < fg_count; i++) {
            if (fg_pids[i] == pid) {
                fg_pids[i] = fg_pids[--fg_count];
                break;
            }
        }
        if (proc_signal(pid, SIGKILL) != 0)
            printf("kill: process %d not found\n", pid);
    }

    /* ---- meminfo ---- */
    else if (strcmp(line_buf, "meminfo") == 0) {
        heap_dump();
    }

    /* ---- lspci ---- */
    else if (strcmp(line_buf, "lspci") == 0) {
        int count = 0;
        pci_device_t *devs = pci_get_devices(&count);
        if (count == 0) {
            printf("No PCI devices found\n");
        } else {
            for (int i = 0; i < count; i++) {
                printf("%02x:%02x.%x %04x:%04x class=%02x:%02x IRQ=%d BAR0=0x%x\n",
                       devs[i].bus, devs[i].slot, devs[i].func,
                       devs[i].vendor_id, devs[i].device_id,
                       devs[i].class_code, devs[i].subclass,
                       devs[i].irq_line, devs[i].bar[0]);
            }
        }
    }

    /* ---- netinfo ---- */
    else if (strcmp(line_buf, "netinfo") == 0) {
        if (!nic) {
            printf("No network interface found\n");
        } else {
            printf("MAC:  %02x:%02x:%02x:%02x:%02x:%02x\n",
                   nic->mac[0], nic->mac[1], nic->mac[2],
                   nic->mac[3], nic->mac[4], nic->mac[5]);
            printf("Link: %s\n", nic->link_up ? "UP" : "DOWN");
            if (net_cfg.configured) {
                printf("IP:   %s\n", ip_fmt(net_cfg.ip));
                printf("Mask: %s\n", ip_fmt(net_cfg.subnet));
                printf("GW:   %s\n", ip_fmt(net_cfg.gateway));
                printf("DNS:  %s\n", ip_fmt(net_cfg.dns));
            } else {
                printf("IP:   (not configured)\n");
            }
        }
    }

    /* ---- ping ---- */
    else if (strncmp(line_buf, "ping ", 5) == 0) {
        const char *arg = shell_arg(line_buf, 4);
        if (!arg) {
            printf("Usage: ping <ip>\n");
        } else {
            uint32_t dst = ip_parse(arg);
            net_ping(dst);
        }
    }

    /* ---- udpsend <ip> <port> <msg> ---- */
    else if (strncmp(line_buf, "udpsend ", 8) == 0) {
        const char *args = shell_arg(line_buf, 7);
        if (!args) {
            printf("Usage: udpsend <ip> <port> <msg>\n");
        } else {
            /* Parse IP */
            const char *p = args;
            while (*p && *p != ' ') p++;
            char ip_buf[16];
            int ip_len = p - args;
            if (ip_len > 15) ip_len = 15;
            memcpy(ip_buf, args, ip_len);
            ip_buf[ip_len] = '\0';

            /* Parse port */
            while (*p == ' ') p++;
            uint16_t port = 0;
            while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');

            /* Parse message */
            while (*p == ' ') p++;
            if (*p == '\0') {
                printf("Usage: udpsend <ip> <port> <msg>\n");
            } else {
                uint32_t dst = ip_parse(ip_buf);
                uint16_t len = (uint16_t)strlen(p);
                if (udp_send(dst, 12345, port, p, len) == 0)
                    printf("Sent %d bytes to %s:%d\n", len, ip_buf, port);
                else
                    printf("udpsend: failed\n");
            }
        }
    }

    /* ---- exec ---- */
    else if (strncmp(line_buf, "exec ", 5) == 0) {
        const char *name = line_buf + 5;
        struct process *p = elf_spawn(name);
        if (p) {
            printf("Started '%s' [PID %d]\n", name, p->pid);
        } else {
            printf("Failed to load '%s'\n", name);
        }
    }

    /* ---- test ---- */
    else if (strcmp(line_buf, "test all") == 0) {
        run_tests(0);
    }
    else if (strcmp(line_buf, "test fd") == 0) {
        run_tests(1);
    }
    else if (strcmp(line_buf, "test pipe") == 0) {
        run_tests(2);
    }
    else if (strcmp(line_buf, "test sleep") == 0) {
        run_tests(3);
    }
    else if (strcmp(line_buf, "test stat") == 0) {
        run_tests(4);
    }
    else if (strcmp(line_buf, "test waitpid") == 0) {
        run_tests(5);
    }
    else if (strcmp(line_buf, "test stdin") == 0) {
        run_tests(6);
    }
    else if (strcmp(line_buf, "test mutex") == 0) {
        run_tests(7);
    }
    else if (strcmp(line_buf, "test sem") == 0) {
        run_tests(8);
    }
    else if (strcmp(line_buf, "test signal") == 0) {
        run_tests(9);
    }
    else if (strcmp(line_buf, "test cwd") == 0) {
        run_tests(10);
    }
    else if (strcmp(line_buf, "test condvar") == 0) {
        run_tests(11);
    }
    else if (strcmp(line_buf, "test rwlock") == 0) {
        run_tests(12);
    }
    else if (strcmp(line_buf, "test mouse") == 0) {
        run_tests(13);
    }
    else if (strcmp(line_buf, "test") == 0) {
        printf("Usage: test <fd|pipe|sleep|stat|stdin|waitpid|mutex|sem|signal|cwd|condvar|rwlock|mouse|all>\n");
    }

    /* ---- clear ---- */
    else if (strcmp(line_buf, "clear") == 0) {
        shell_clear();
    }

    /* ---- echo ---- */
    else if (strcmp(line_buf, "echo") == 0) {
        printf("\n");
    }
    else if (strncmp(line_buf, "echo ", 5) == 0) {
        const char *text = shell_arg(line_buf, 4);
        if (text)
            printf("%s\n", text);
        else
            printf("\n");
    }

    /* ---- grep ---- */
    else if (strncmp(line_buf, "grep ", 5) == 0) {
        const char *args = shell_arg(line_buf, 4);
        if (!args) {
            printf("Usage: grep <pattern> [file]\n");
        } else {
            const char *pattern, *filename;
            shell_split_args(args, &pattern, &filename);
            if (filename) {
                int32_t ino = vfs_resolve(filename, (uint32_t *)0, (char *)0);
                if (ino < 0) {
                    printf("grep: %s: not found\n", filename);
                } else {
                    vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
                    if (!node || node->type != VFS_TYPE_FILE)
                        printf("grep: %s: not a regular file\n", filename);
                    else
                        shell_grep_data((const char *)node->data, node->size,
                                        pattern);
                }
            } else if (stdin_buf) {
                shell_grep_data(stdin_buf, stdin_len, pattern);
            } else {
                printf("Usage: grep <pattern> [file]\n");
            }
        }
    }

    /* ---- export ---- */
    else if (strcmp(line_buf, "export") == 0) {
        for (int i = 0; i < num_vars; i++) {
            if (shell_vars[i].exported)
                printf("export %s=%s\n", shell_vars[i].name,
                       shell_vars[i].value);
        }
    }
    else if (strncmp(line_buf, "export ", 7) == 0) {
        const char *arg = shell_arg(line_buf, 6);
        if (arg) {
            const char *eq = strchr(arg, '=');
            if (eq) {
                char name[VAR_NAME_MAX + 1];
                int nlen = eq - arg;
                if (nlen > VAR_NAME_MAX) nlen = VAR_NAME_MAX;
                memcpy(name, arg, nlen);
                name[nlen] = '\0';
                shell_setvar(name, eq + 1);
                for (int i = 0; i < num_vars; i++) {
                    if (strcmp(shell_vars[i].name, name) == 0) {
                        shell_vars[i].exported = 1;
                        break;
                    }
                }
            } else {
                for (int i = 0; i < num_vars; i++) {
                    if (strcmp(shell_vars[i].name, arg) == 0) {
                        shell_vars[i].exported = 1;
                        break;
                    }
                }
            }
        }
    }

    /* ---- set (list all variables) ---- */
    else if (strcmp(line_buf, "set") == 0) {
        for (int i = 0; i < num_vars; i++) {
            printf("%s=%s%s\n", shell_vars[i].name, shell_vars[i].value,
                   shell_vars[i].exported ? " [exported]" : "");
        }
    }

    /* ---- unset ---- */
    else if (strncmp(line_buf, "unset ", 6) == 0) {
        const char *name = shell_arg(line_buf, 5);
        if (!name) {
            printf("Usage: unset <name>\n");
        } else {
            for (int i = 0; i < num_vars; i++) {
                if (strcmp(shell_vars[i].name, name) == 0) {
                    for (int j = i; j < num_vars - 1; j++)
                        shell_vars[j] = shell_vars[j + 1];
                    num_vars--;
                    break;
                }
            }
        }
    }

    /* ---- wc ---- */
    else if (strcmp(line_buf, "wc") == 0) {
        if (stdin_buf)
            shell_wc_data(stdin_buf, stdin_len, (const char *)0);
        else
            printf("Usage: wc [file]\n");
    }
    else if (strncmp(line_buf, "wc ", 3) == 0) {
        const char *name = shell_arg(line_buf, 2);
        if (!name) {
            if (stdin_buf)
                shell_wc_data(stdin_buf, stdin_len, (const char *)0);
            else
                printf("Usage: wc [file]\n");
        } else {
            int32_t ino = vfs_resolve(name, (uint32_t *)0, (char *)0);
            if (ino < 0) {
                printf("wc: %s: not found\n", name);
            } else {
                vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
                if (!node || node->type != VFS_TYPE_FILE)
                    printf("wc: %s: not a regular file\n", name);
                else
                    shell_wc_data((const char *)node->data, node->size, name);
            }
        }
    }

    /* ---- head ---- */
    else if (strcmp(line_buf, "head") == 0) {
        if (stdin_buf)
            shell_head_data(stdin_buf, stdin_len, 10);
        else
            printf("Usage: head [-N] [file]\n");
    }
    else if (strncmp(line_buf, "head ", 5) == 0) {
        const char *arg = shell_arg(line_buf, 4);
        int n = 10;
        if (arg && arg[0] == '-' && arg[1] >= '0' && arg[1] <= '9') {
            n = (int)parse_uint(arg + 1);
            if (n <= 0) n = 10;
            /* Skip past -N to get filename */
            const char *p = arg + 1;
            while (*p >= '0' && *p <= '9') p++;
            while (*p == ' ') p++;
            arg = (*p) ? p : (const char *)0;
        }
        if (arg) {
            int32_t ino = vfs_resolve(arg, (uint32_t *)0, (char *)0);
            if (ino < 0)
                printf("head: %s: not found\n", arg);
            else {
                vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
                if (!node || node->type != VFS_TYPE_FILE)
                    printf("head: %s: not a regular file\n", arg);
                else
                    shell_head_data((const char *)node->data, node->size, n);
            }
        } else if (stdin_buf) {
            shell_head_data(stdin_buf, stdin_len, n);
        } else {
            printf("Usage: head [-N] [file]\n");
        }
    }

    /* ---- tail ---- */
    else if (strcmp(line_buf, "tail") == 0) {
        if (stdin_buf)
            shell_tail_data(stdin_buf, stdin_len, 10);
        else
            printf("Usage: tail [-N] [file]\n");
    }
    else if (strncmp(line_buf, "tail ", 5) == 0) {
        const char *arg = shell_arg(line_buf, 4);
        int n = 10;
        if (arg && arg[0] == '-' && arg[1] >= '0' && arg[1] <= '9') {
            n = (int)parse_uint(arg + 1);
            if (n <= 0) n = 10;
            /* Skip past -N to get filename */
            const char *p = arg + 1;
            while (*p >= '0' && *p <= '9') p++;
            while (*p == ' ') p++;
            arg = (*p) ? p : (const char *)0;
        }
        if (arg) {
            int32_t ino = vfs_resolve(arg, (uint32_t *)0, (char *)0);
            if (ino < 0)
                printf("tail: %s: not found\n", arg);
            else {
                vfs_inode_t *node = vfs_get_inode((uint32_t)ino);
                if (!node || node->type != VFS_TYPE_FILE)
                    printf("tail: %s: not a regular file\n", arg);
                else
                    shell_tail_data((const char *)node->data, node->size, n);
            }
        } else if (stdin_buf) {
            shell_tail_data(stdin_buf, stdin_len, n);
        } else {
            printf("Usage: tail [-N] [file]\n");
        }
    }

    else {
        printf("Unknown command: '%s' (type 'help')\n", line_buf);
    }
}

/* ================================================================== */
/*  Pipeline / redirect wrapper around shell_execute_cmd               */
/* ================================================================== */

void shell_execute(void) {
    if (line_len == 0) return;

    /* Expand variables in the whole line first */
    static char expanded[LINE_BUF_SIZE * 2];
    shell_expand_vars(line_buf, expanded, sizeof(expanded));

    /* Parse the expanded line for pipes and redirects */
    static char parse_buf[LINE_BUF_SIZE * 2];
    int elen = strlen(expanded);
    memcpy(parse_buf, expanded, elen + 1);
    shell_parse_line(parse_buf);

    /* Fast path: simple command, no pipe, no redirect */
    if (num_segments == 1 && segments[0].redir_type == REDIR_NONE) {
        int slen = strlen(segments[0].cmd);
        if (slen >= LINE_BUF_SIZE) slen = LINE_BUF_SIZE - 1;
        memcpy(line_buf, segments[0].cmd, slen);
        line_buf[slen] = '\0';
        line_len = slen;
        shell_execute_cmd();
        return;
    }

    /* Pipeline and/or redirection */
    char *input = (char *)0;
    uint32_t input_len = 0;

    for (int i = 0; i < num_segments; i++) {
        int is_last = (i == num_segments - 1);

        /* Set up piped stdin from previous stage */
        if (input) shell_stdin_set(input, input_len);

        /* Capture output if piping to next stage or redirecting to file */
        int capturing = !is_last || segments[i].redir_type != REDIR_NONE;
        if (capturing) capture_start();

        /* Copy segment command into line_buf and execute */
        int slen = strlen(segments[i].cmd);
        if (slen >= LINE_BUF_SIZE) slen = LINE_BUF_SIZE - 1;
        memcpy(line_buf, segments[i].cmd, slen);
        line_buf[slen] = '\0';
        line_len = slen;
        shell_execute_cmd();

        /* Collect captured output */
        char *output = (char *)0;
        uint32_t output_len = 0;
        if (capturing) capture_stop(&output, &output_len);

        /* Clean up previous input */
        if (input) { kfree(input); input = (char *)0; }
        shell_stdin_clear();

        /* Route output */
        if (is_last && segments[i].redir_type != REDIR_NONE) {
            shell_write_to_file(segments[i].redir_file,
                                segments[i].redir_type, output, output_len);
            if (output) kfree(output);
        } else if (!is_last) {
            input = output;
            input_len = output_len;
        }
    }

    if (input) kfree(input);
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
    while (1) {
        shell_init_prefix();
        shell_readline();
        shell_execute();
    }
}
