#pragma once

#include_next <stdatomic.h>




#undef atomic_compare_exchange_weak

#define atomic_compare_exchange_weak(object, expected, desired) fiber_atomic_compare_exchange_weak(object, expected, desired)


bool fiber_atomic_compare_exchange_weak(atomic_uint *object, unsigned *expected, unsigned desired);
