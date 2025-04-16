#pragma once


#include <limits.h>
#include <stdatomic.h>

typedef unsigned int index_t;
typedef atomic_uint atomic_index_t;

#define INDEX_END UINT_MAX

#define CONSUMED_FLAG ((index_t)1 << (UINT_WIDTH - 1))

#define ORIGIN_MASK CONSUMED_FLAG

#define INDEX_MASK (~ORIGIN_MASK)


