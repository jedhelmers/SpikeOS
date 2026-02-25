/*
 * alloc_test.c — Exercise the userland heap allocator.
 *
 * Tests: malloc, free, calloc, realloc, and edge cases.
 * Run with: exec alloc_test.elf
 */
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/unistd.h"
#include "libc/string.h"

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
    printf("=== Userland Allocator Test (PID %d) ===\n\n", getpid());

    /* Test 1: Basic malloc and free */
    printf("Test 1: Basic malloc/free\n");
    {
        char *p = (char *)malloc(100);
        check("malloc(100) != NULL", p != NULL);

        memset(p, 'A', 100);
        check("memset succeeded", p[0] == 'A' && p[99] == 'A');

        free(p);
        check("free(p) completed", 1);
    }

    /* Test 2: Edge cases */
    printf("\nTest 2: Edge cases\n");
    {
        void *p = malloc(0);
        check("malloc(0) == NULL", p == NULL);

        free(NULL);
        check("free(NULL) safe", 1);
    }

    /* Test 3: Multiple allocations */
    printf("\nTest 3: Multiple allocations\n");
    {
        int *a = (int *)malloc(sizeof(int) * 10);
        int *b = (int *)malloc(sizeof(int) * 20);
        int *c = (int *)malloc(sizeof(int) * 5);

        check("a != NULL", a != NULL);
        check("b != NULL", b != NULL);
        check("c != NULL", c != NULL);
        check("a != b", (void *)a != (void *)b);
        check("b != c", (void *)b != (void *)c);

        for (int i = 0; i < 10; i++) a[i] = i;
        for (int i = 0; i < 20; i++) b[i] = i * 2;
        for (int i = 0; i < 5; i++)  c[i] = i * 3;

        check("a[9] == 9",   a[9] == 9);
        check("b[19] == 38", b[19] == 38);
        check("c[4] == 12",  c[4] == 12);

        free(a);
        free(b);
        free(c);
    }

    /* Test 4: calloc (zero-initialized) */
    printf("\nTest 4: calloc\n");
    {
        int *p = (int *)calloc(10, sizeof(int));
        check("calloc(10, 4) != NULL", p != NULL);

        int all_zero = 1;
        for (int i = 0; i < 10; i++) {
            if (p[i] != 0) { all_zero = 0; break; }
        }
        check("calloc memory is zeroed", all_zero);

        free(p);
    }

    /* Test 5: realloc */
    printf("\nTest 5: realloc\n");
    {
        char *p = (char *)malloc(16);
        check("initial malloc(16)", p != NULL);

        strcpy(p, "Hello");

        p = (char *)realloc(p, 64);
        check("realloc to 64", p != NULL);
        check("data preserved after realloc", strcmp(p, "Hello") == 0);

        p = (char *)realloc(p, 8);
        check("realloc to 8 (shrink)", p != NULL);
        check("data preserved after shrink", p[0] == 'H' && p[4] == 'o');

        char *q = (char *)realloc(NULL, 32);
        check("realloc(NULL, 32) works", q != NULL);

        free(p);
        free(q);

        char *r = (char *)malloc(16);
        r = (char *)realloc(r, 0);
        check("realloc(p, 0) returns NULL", r == NULL);
    }

    /* Test 6: Stress test */
    printf("\nTest 6: Stress (64 allocations)\n");
    {
        void *ptrs[64];
        int success = 1;

        for (int i = 0; i < 64; i++) {
            ptrs[i] = malloc(32);
            if (!ptrs[i]) { success = 0; break; }
            memset(ptrs[i], (unsigned char)i, 32);
        }
        check("64 x malloc(32) all succeeded", success);

        for (int i = 0; i < 64; i += 2)
            free(ptrs[i]);

        int realloc_ok = 1;
        for (int i = 0; i < 64; i += 2) {
            ptrs[i] = malloc(16);
            if (!ptrs[i]) { realloc_ok = 0; break; }
        }
        check("re-malloc after partial free", realloc_ok);

        for (int i = 0; i < 64; i++)
            free(ptrs[i]);
        check("free all 64 ptrs", 1);
    }

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
