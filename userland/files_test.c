/*
 * files_test.c — Exercise filesystem syscalls from userland.
 *
 * Tests: open, write, read, close, stat, lseek, mkdir, chdir, getcwd, unlink.
 * Run with: exec files_test.elf
 */
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/unistd.h"
#include "libc/string.h"
#include "libc/stat.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *name, int condition) {
    if (condition) {
        printf("  [PASS] %s\n", name);
        tests_passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        tests_failed++;
    }
}

int main(void) {
    printf("=== Filesystem Test (PID %d) ===\n\n", getpid());

    /* Test 1: getcwd */
    printf("Test 1: getcwd\n");
    {
        char buf[128];
        char *r = getcwd(buf, sizeof(buf));
        check("getcwd returns non-NULL", r != NULL);
        if (r) printf("  cwd = \"%s\"\n", buf);
    }

    /* Test 2: Create and write a file */
    printf("\nTest 2: Create/write/read file\n");
    {
        int fd = open("/test_file.txt", O_CREAT | O_WRONLY);
        check("open(/test_file.txt, CREAT|WRONLY) >= 0", fd >= 0);

        if (fd >= 0) {
            const char *msg = "Hello from files_test!";
            int n = write(fd, msg, strlen(msg));
            check("write returned correct count", n == (int)strlen(msg));
            close(fd);
        }

        fd = open("/test_file.txt", O_RDONLY);
        check("open for reading >= 0", fd >= 0);

        if (fd >= 0) {
            char buf[64];
            memset(buf, 0, sizeof(buf));
            int n = read(fd, buf, sizeof(buf) - 1);
            check("read returned > 0", n > 0);
            check("content matches", strcmp(buf, "Hello from files_test!") == 0);
            close(fd);
        }
    }

    /* Test 3: stat */
    printf("\nTest 3: stat\n");
    {
        struct spike_stat st;
        int r = stat("/test_file.txt", &st);
        check("stat returns 0", r == 0);
        if (r == 0) {
            check("type == S_TYPE_FILE", st.type == S_TYPE_FILE);
            check("size > 0", st.size > 0);
            printf("  size=%u, ino=%u, nlink=%u\n", st.size, st.ino, st.nlink);
        }
    }

    /* Test 4: lseek */
    printf("\nTest 4: lseek\n");
    {
        int fd = open("/test_file.txt", O_RDONLY);
        check("open for seek test", fd >= 0);

        if (fd >= 0) {
            int pos = lseek(fd, 6, SEEK_SET);
            check("lseek to offset 6", pos == 6);

            char buf[16];
            memset(buf, 0, sizeof(buf));
            read(fd, buf, 4);
            check("read from offset 6 gives 'from'", strncmp(buf, "from", 4) == 0);

            close(fd);
        }
    }

    /* Test 5: mkdir and chdir */
    printf("\nTest 5: mkdir/chdir\n");
    {
        int r = mkdir("/testdir");
        check("mkdir /testdir", r == 0);

        r = chdir("/testdir");
        check("chdir /testdir", r == 0);

        char buf[128];
        getcwd(buf, sizeof(buf));
        check("cwd is /testdir", strcmp(buf, "/testdir") == 0);

        chdir("/");
    }

    /* Test 6: unlink */
    printf("\nTest 6: unlink\n");
    {
        int r = unlink("/test_file.txt");
        check("unlink /test_file.txt", r == 0);

        struct spike_stat st;
        r = stat("/test_file.txt", &st);
        check("stat after unlink fails", r < 0);

        r = unlink("/testdir");
        check("unlink /testdir (empty dir)", r == 0);
    }

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
