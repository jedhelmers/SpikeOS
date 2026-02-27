#ifndef _VIRTIO_GPU_H
#define _VIRTIO_GPU_H

#include <stdint.h>
#include <kernel/surface.h>

/*
 * VirtIO GPU device definitions.
 * Based on VirtIO 1.1 specification, section 5.7 (GPU Device).
 */

/* VirtIO GPU command types */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO     0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF       0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT          0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH       0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107

/* VirtIO GPU 3D command types */
#define VIRTIO_GPU_CMD_CTX_CREATE              0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY             0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE     0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE     0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D      0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D     0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D   0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D               0x0207

/* VirtIO GPU response types */
#define VIRTIO_GPU_RESP_OK_NODATA           0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO     0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC          0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY   0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

/* VirtIO GPU formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM    1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM    2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM    3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM    4  /* XRGB8888 — matches our FB */
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM    67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM    68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM    121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM    134

/* VirtIO GPU flag bits for control header */
#define VIRTIO_GPU_FLAG_FENCE  (1u << 0)

/* Maximum scanouts (displays) */
#define VIRTIO_GPU_MAX_SCANOUTS  16

/* ------------------------------------------------------------------ */
/*  Command/response structures                                       */
/* ------------------------------------------------------------------ */

/* Control header — prefix of every command and response */
typedef struct {
    uint32_t type;       /* VIRTIO_GPU_CMD_* or VIRTIO_GPU_RESP_* */
    uint32_t flags;      /* VIRTIO_GPU_FLAG_* */
    uint64_t fence_id;   /* fence ID if FLAG_FENCE is set */
    uint32_t ctx_id;     /* 3D context ID (0 for 2D) */
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

/* Rectangle */
typedef struct {
    uint32_t x, y, width, height;
} __attribute__((packed)) virtio_gpu_rect_t;

/* Display info response */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    struct {
        virtio_gpu_rect_t r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

/* RESOURCE_CREATE_2D */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;     /* VIRTIO_GPU_FORMAT_* */
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

/* RESOURCE_UNREF */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

/* SET_SCANOUT */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

/* RESOURCE_FLUSH */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

/* TRANSFER_TO_HOST_2D */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;     /* offset in resource backing */
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

/* Memory entry for RESOURCE_ATTACH_BACKING */
typedef struct {
    uint64_t addr;       /* guest-physical address */
    uint32_t length;     /* length in bytes */
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

/* RESOURCE_ATTACH_BACKING (variable-length: header + nr_entries mem entries) */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* followed by nr_entries * virtio_gpu_mem_entry_t */
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

/* RESOURCE_DETACH_BACKING */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_detach_backing_t;

/* ------------------------------------------------------------------ */
/*  3D command/response structures                                    */
/* ------------------------------------------------------------------ */

/* CTX_CREATE */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t nlen;           /* debug name length */
    uint32_t padding;
    char debug_name[64];
} __attribute__((packed)) virtio_gpu_ctx_create_t;

/* CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctx_resource_t;

/* RESOURCE_CREATE_3D */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;         /* pipe_texture_target */
    uint32_t format;         /* virgl_formats */
    uint32_t bind;           /* VIRGL_BIND_* flags */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_create_3d_t;

/* 3D box (for transfers) */
typedef struct {
    uint32_t x, y, z, w, h, d;
} __attribute__((packed)) virtio_gpu_box_t;

/* TRANSFER_TO_HOST_3D / TRANSFER_FROM_HOST_3D */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed)) virtio_gpu_transfer_host_3d_t;

/* SUBMIT_3D (header only — command data follows inline) */
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t size;           /* size of command buffer in bytes */
    uint32_t padding;
    /* followed by `size` bytes of VirGL command stream */
} __attribute__((packed)) virtio_gpu_cmd_submit_t;

/* ------------------------------------------------------------------ */
/*  Driver API                                                        */
/* ------------------------------------------------------------------ */

/* Initialize the VirtIO GPU driver. Returns 0 on success, -1 if not found. */
int virtio_gpu_init(void);

/* Get the display resolution detected from the device.
 * Returns 0 on success, -1 if no display info available. */
int virtio_gpu_get_display_size(uint32_t *width, uint32_t *height);

/* Create a 2D resource. Returns 0 on success. */
int virtio_gpu_create_resource(uint32_t resource_id, uint32_t format,
                               uint32_t width, uint32_t height);

/* Attach backing pages to a resource. */
int virtio_gpu_attach_backing(uint32_t resource_id,
                              uint32_t phys_addr, uint32_t size);

/* Transfer a rectangle from guest memory to the host resource. */
int virtio_gpu_transfer_to_host(uint32_t resource_id,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h);

/* Set a resource as the scanout for display 0. */
int virtio_gpu_set_scanout(uint32_t resource_id,
                           uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h);

/* Flush a rectangle of the current scanout to the display. */
int virtio_gpu_flush(uint32_t resource_id,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h);

/* Set up GPU-backed scanout: allocate contiguous backing memory,
 * create resource, attach backing, configure scanout.
 * Must be called after virtio_gpu_init() succeeds.
 * Returns 0 on success, -1 on failure. */
int virtio_gpu_setup_scanout(void);

/* Present a frame: copy compositor surface pixels to GPU backing buffer,
 * transfer to host, and flush to display.
 * Call after compositing a frame. */
void virtio_gpu_present(surface_t *compositor);

/* Returns 1 if GPU scanout is active and ready for present calls. */
int virtio_gpu_scanout_active(void);

/* Returns 1 if the device supports VirGL 3D rendering. */
int virtio_gpu_has_virgl(void);

/* ------------------------------------------------------------------ */
/*  3D / VirGL API                                                    */
/* ------------------------------------------------------------------ */

/* Create a VirGL 3D rendering context. Returns 0 on success. */
int virtio_gpu_ctx_create(uint32_t ctx_id, const char *debug_name);

/* Destroy a VirGL context. */
int virtio_gpu_ctx_destroy(uint32_t ctx_id);

/* Attach/detach a resource to/from a context. */
int virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id);
int virtio_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id);

/* Create a 3D resource (texture, buffer, etc.). */
int virtio_gpu_create_resource_3d(uint32_t resource_id, uint32_t target,
                                  uint32_t format, uint32_t bind,
                                  uint32_t width, uint32_t height,
                                  uint32_t depth, uint32_t array_size,
                                  uint32_t last_level, uint32_t nr_samples);

/* Submit a VirGL 3D command buffer to a context. */
int virtio_gpu_submit_3d(uint32_t ctx_id, const uint32_t *cmdbuf,
                         uint32_t size_bytes);

/* Run the kernel-mode VirGL triangle demo.
 * Draws a solid-color triangle using GPU-accelerated 3D rendering. */
void virtio_gpu_3d_demo(void);

#endif
