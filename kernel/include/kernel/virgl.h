#ifndef _VIRGL_H
#define _VIRGL_H

/*
 * VirGL 3D rendering protocol definitions.
 * Based on virglrenderer's virgl_protocol.h and Gallium3D pipe definitions.
 *
 * VirGL commands are encoded as a stream of 32-bit words submitted to the
 * host via VIRTIO_GPU_CMD_SUBMIT_3D. Each command starts with a header word
 * encoding the command type, object type, and payload length.
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Command encoding                                                  */
/* ------------------------------------------------------------------ */

/*
 * VIRGL_CMD0(cmd, obj, len)
 *   bits  0-7:  command type (virgl_context_cmd)
 *   bits  8-15: object type (virgl_object_type)
 *   bits 16-31: payload length in uint32 words (not including header)
 */
#define VIRGL_CMD0(cmd, obj, len) \
    ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))

/* ------------------------------------------------------------------ */
/*  VirGL command types (virgl_context_cmd)                           */
/* ------------------------------------------------------------------ */

#define VIRGL_CCMD_NOP                    0
#define VIRGL_CCMD_CREATE_OBJECT          1
#define VIRGL_CCMD_BIND_OBJECT            2
#define VIRGL_CCMD_DESTROY_OBJECT         3
#define VIRGL_CCMD_SET_VIEWPORT_STATE     4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE  5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS     6
#define VIRGL_CCMD_CLEAR                  7
#define VIRGL_CCMD_DRAW_VBO              8
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE  9
#define VIRGL_CCMD_SET_SAMPLER_VIEWS     10
#define VIRGL_CCMD_SET_INDEX_BUFFER      11
#define VIRGL_CCMD_SET_CONSTANT_BUFFER   12
#define VIRGL_CCMD_SET_STENCIL_REF       13
#define VIRGL_CCMD_SET_BLEND_COLOR       14
#define VIRGL_CCMD_SET_SCISSOR_STATE     15
#define VIRGL_CCMD_BLIT                  16
#define VIRGL_CCMD_BIND_SHADER           31

/* ------------------------------------------------------------------ */
/*  VirGL object types (virgl_object_type)                            */
/* ------------------------------------------------------------------ */

#define VIRGL_OBJECT_NULL             0
#define VIRGL_OBJECT_BLEND            1
#define VIRGL_OBJECT_RASTERIZER       2
#define VIRGL_OBJECT_DSA              3
#define VIRGL_OBJECT_SHADER           4
#define VIRGL_OBJECT_VERTEX_ELEMENTS  5
#define VIRGL_OBJECT_SAMPLER_VIEW     6
#define VIRGL_OBJECT_SAMPLER_STATE    7
#define VIRGL_OBJECT_SURFACE          8
#define VIRGL_OBJECT_QUERY            9
#define VIRGL_OBJECT_STREAMOUT_TARGET 10

/* ------------------------------------------------------------------ */
/*  Gallium pipe_texture_target                                       */
/* ------------------------------------------------------------------ */

#define PIPE_BUFFER       0
#define PIPE_TEXTURE_2D   2

/* ------------------------------------------------------------------ */
/*  VirGL format codes (matches Gallium pipe_format)                  */
/* ------------------------------------------------------------------ */

#define VIRGL_FORMAT_B8G8R8A8_UNORM     1
#define VIRGL_FORMAT_B8G8R8X8_UNORM     2
#define VIRGL_FORMAT_R32G32B32A32_FLOAT  31
#define VIRGL_FORMAT_R8G8B8A8_UNORM     67

/* ------------------------------------------------------------------ */
/*  VIRGL_BIND_* flags                                                */
/* ------------------------------------------------------------------ */

#define VIRGL_BIND_DEPTH_STENCIL   (1u << 0)
#define VIRGL_BIND_RENDER_TARGET   (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW    (1u << 3)
#define VIRGL_BIND_VERTEX_BUFFER   (1u << 4)
#define VIRGL_BIND_INDEX_BUFFER    (1u << 5)
#define VIRGL_BIND_CONSTANT_BUFFER (1u << 6)
#define VIRGL_BIND_DISPLAY_TARGET  (1u << 7)

/* ------------------------------------------------------------------ */
/*  Gallium pipe_prim_type                                            */
/* ------------------------------------------------------------------ */

#define PIPE_PRIM_POINTS          0
#define PIPE_PRIM_LINES           1
#define PIPE_PRIM_TRIANGLES       4
#define PIPE_PRIM_TRIANGLE_STRIP  5
#define PIPE_PRIM_TRIANGLE_FAN    6

/* ------------------------------------------------------------------ */
/*  Gallium pipe_shader_type                                          */
/* ------------------------------------------------------------------ */

#define PIPE_SHADER_VERTEX    0
#define PIPE_SHADER_FRAGMENT  1

/* ------------------------------------------------------------------ */
/*  PIPE_CLEAR_* flags                                                */
/* ------------------------------------------------------------------ */

#define PIPE_CLEAR_DEPTH    (1u << 0)
#define PIPE_CLEAR_STENCIL  (1u << 1)
#define PIPE_CLEAR_COLOR0   (1u << 2)

/* ------------------------------------------------------------------ */
/*  Helper: float bits as uint32 for command buffer encoding          */
/* ------------------------------------------------------------------ */

static inline uint32_t virgl_float_bits(float f) {
    union { float f; uint32_t u; } u;
    u.f = f;
    return u.u;
}

#endif
