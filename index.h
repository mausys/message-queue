#pragma once

#include <stdint.h>
#include <stdatomic.h>

typedef uint32_t index_t;
typedef atomic_uint atomic_index_t;

#define INDEX_END UINT32_MAX

#define CONSUMED_FLAG ((index_t)1 << 31)

#define ORIGIN_MASK CONSUMED_FLAG

#define INDEX_MASK (~ORIGIN_MASK)
