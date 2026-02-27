/*
 * TinyGL message handling — SpikeOS kernel integration.
 * Uses kernel printf instead of stdio fprintf.
 */
#include "msghandling.h"
#include "../include/GL/gl.h"
#include "zgl.h"
#include <stdarg.h>
#include <stdio.h>  /* kernel's printf */

void tgl_warning(const char* format, ...) {
	(void)format;
	/* Warnings suppressed in kernel mode to avoid log spam */
}

void tgl_trace(const char* format, ...) {
	(void)format;
	/* Trace suppressed in kernel mode */
}

void tgl_fixme(const char* format, ...) {
	(void)format;
	/* Fixme suppressed in kernel mode */
}

void gl_fatal_error(char* format, ...) {
	printf("TinyGL FATAL: ");
	/* Note: kernel printf doesn't support vprintf, just print the format string */
	printf("%s\n", format);
	/* In kernel mode, just loop forever — can't exit() */
	for (;;) ;
}
