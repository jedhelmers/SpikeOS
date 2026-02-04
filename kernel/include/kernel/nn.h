#ifndef _NN_H
#define _NN_H

#include <stdint.h>
#include <stddef.h>

// Base address where the NN will be copied in RAM
// This will eventually need moved
#define NN_BASE_ADDR  ((uint8_t*)0x01000000)
#define NN_MAX_SIZE   (512 * 1024)   // 512 KB reserved

// NNET header layout (matches my Python exporter)
typedef struct {
    char     magic[4];      // "NNET"
    uint32_t version;
    uint32_t weight_count;
    uint32_t bias_count;
    uint32_t weight_scale_q16;
    uint32_t bias_scale_q16;
} nn_header_t;

// Public API
int      nn_load_embedded(void);
uint8_t* nn_base(void);
size_t   nn_size(void);
nn_header_t* nn_header(void);

void nn_dump_bytes(size_t count);

#endif