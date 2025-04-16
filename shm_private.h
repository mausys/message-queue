#pragma once

#include "shm.h"

#include <stdint.h>

#include "index.h"

uintptr_t msgq_shm_get_buffer(msgq_shm_t *shm);

atomic_index_t* msgq_shm_get_head(msgq_shm_t *shm);

atomic_index_t* msgq_shm_get_tail(msgq_shm_t *shm);

atomic_index_t* msgq_shm_get_list(msgq_shm_t *shm);
