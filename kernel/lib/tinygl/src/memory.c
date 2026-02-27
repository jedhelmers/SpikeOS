/*
 * Memory allocator for TinyGL — SpikeOS kernel integration.
 * Uses kmalloc/kfree/kcalloc from kernel heap.
 */
#include "../include/zfeatures.h"

static inline void required_for_compilation_(void){
	return;
}

#if TGL_FEATURE_CUSTOM_MALLOC == 1
#include "zgl.h"
#include <kernel/heap.h>

void gl_free(void *p) { kfree(p); }

void *gl_malloc(GLint size) { return kmalloc((unsigned int)size); }

void *gl_zalloc(GLint size) { return kcalloc(1, (unsigned int)size); }
#endif
