/*
 * mmap_test.c — test mmap/munmap syscalls
 */
#include "libc/stdio.h"
#include "libc/unistd.h"
#include "libc/string.h"

static int test_count = 0;
static int pass_count = 0;

static void check(int cond, const char *name) {
    test_count++;
    if (cond) {
        printf("  [PASS] %s\n", name);
        pass_count++;
    } else {
        printf("  [FAIL] %s\n", name);
    }
}

int main(void) {
    printf("=== mmap/munmap test ===\n\n");

    /* Test 1: basic anonymous mmap */
    printf("Test 1: anonymous mmap\n");
    void *p = spike_mmap(0, 4096, PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check(p != MAP_FAILED, "mmap returns valid address");
    check((unsigned int)p >= 0x40000000, "address >= MMAP_BASE");
    check(((unsigned int)p & 0xFFF) == 0, "address is page-aligned");

    /* Test 2: write to mapped memory */
    printf("Test 2: read/write mapped memory\n");
    if (p != MAP_FAILED) {
        unsigned int *ip = (unsigned int *)p;
        ip[0] = 0xDEADBEEF;
        ip[1] = 0xCAFEBABE;
        check(ip[0] == 0xDEADBEEF, "write/read word 0");
        check(ip[1] == 0xCAFEBABE, "write/read word 1");
    }

    /* Test 3: multi-page mmap */
    printf("Test 3: multi-page mmap (16KB)\n");
    void *p2 = spike_mmap(0, 16384, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check(p2 != MAP_FAILED, "16KB mmap succeeds");
    if (p2 != MAP_FAILED) {
        /* Write to first and last page */
        ((unsigned char *)p2)[0]     = 0xAA;
        ((unsigned char *)p2)[16383] = 0xBB;
        check(((unsigned char *)p2)[0]     == 0xAA, "first page accessible");
        check(((unsigned char *)p2)[16383] == 0xBB, "last page accessible");
    }

    /* Test 4: munmap */
    printf("Test 4: munmap\n");
    if (p != MAP_FAILED) {
        int ret = spike_munmap(p, 4096);
        check(ret == 0, "munmap first region");
    }
    if (p2 != MAP_FAILED) {
        int ret = spike_munmap(p2, 16384);
        check(ret == 0, "munmap second region");
    }

    /* Test 5: mmap after munmap (reuse) */
    printf("Test 5: mmap after munmap\n");
    void *p3 = spike_mmap(0, 4096, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check(p3 != MAP_FAILED, "re-mmap after munmap");
    if (p3 != MAP_FAILED) {
        spike_munmap(p3, 4096);
    }

    /* Test 6: zero-length mmap should fail */
    printf("Test 6: invalid args\n");
    void *bad = spike_mmap(0, 0, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check(bad == MAP_FAILED, "zero-length mmap fails");

    printf("\n=== Results: %d/%d passed ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
