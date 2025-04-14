#pragma once

#include <stdlib.h>

#include "index.h"

typedef struct msgq_shm {
    unsigned n;
    size_t msg_size;
    void *mem;
} msgq_shm_t;


static inline size_t mem_align(size_t size, size_t alignment)
{
    return (size + alignment - 1) & ~(alignment - 1);
}




size_t msgq_shm_calc_size(size_t n, size_t msg_size);

uintptr_t msgq_shm_get_buffer(msgq_shm_t *shm);

atomic_index_t* msgq_shm_get_head(msgq_shm_t *shm);

atomic_index_t* msgq_shm_get_tail(msgq_shm_t *shm);

atomic_index_t* msgq_shm_get_list(msgq_shm_t *shm);
