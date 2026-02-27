#ifndef _STDLIB_H
#define _STDLIB_H 1

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((__noreturn__))
void abort(void);

/* Kernel-mode exit: loop forever (no userspace to return to) */
__attribute__((__noreturn__))
static inline void exit(int status) {
    (void)status;
    for (;;) ;
}

#ifdef __cplusplus
}
#endif

#endif