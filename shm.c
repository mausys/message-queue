#include "shm_private.h"

#include <stddef.h>
#include <stdalign.h>

#include "index.h"

#define MESSAGE_ALIGNMENT (alignof(max_align_t))


static size_t get_buffer_offset(size_t n)
{
    size_t size = (2 + n) * sizeof (index_t); // head + tail + chain
    return mem_align(size, MESSAGE_ALIGNMENT);
}


uintptr_t msgq_shm_get_buffer(msgq_shm_t *shm)
{
    return (uintptr_t)shm->mem + get_buffer_offset(shm->n);
}


size_t msgq_shm_calc_size(size_t n, size_t msg_size)
{
    msg_size = mem_align(msg_size, MESSAGE_ALIGNMENT);

    size_t size = get_buffer_offset(n);

    size += n * msg_size;

    return size;
}

atomic_index_t* msgq_shm_get_head(msgq_shm_t *shm)
{
    atomic_index_t *indices = shm->mem;
    return &indices[0];
}


atomic_index_t* msgq_shm_get_tail(msgq_shm_t *shm)
{
    atomic_index_t *indices = shm->mem;
    return &indices[1];
}


atomic_index_t* msgq_shm_get_list(msgq_shm_t *shm)
{
    atomic_index_t *indices = shm->mem;
    return &indices[2];
}


msgq_shm_t msgq_shm_new(size_t n, size_t msg_size)
{
    msgq_shm_t shm = { 0 };

    /* algorithm won't work with less than 3 messages */
    if (n < 3)
        return shm;

    size_t size = msgq_shm_calc_size(n, msg_size);

    shm.mem =  malloc(size);

    if (!shm.mem)
        return shm;

    shm.n = n;
    shm.msg_size = msg_size;

    atomic_index_t *tail = msgq_shm_get_head(&shm);
    atomic_index_t *head = msgq_shm_get_tail(&shm);
    atomic_index_t *chain = msgq_shm_get_list(&shm);

    *tail = INDEX_END;
    *head = INDEX_END;

    /* initialize chain, except for last element */
    for (unsigned i = 0; i < n - 1; i++)
        chain[i] = i + 1;

    /* circular, last next is zero */
    chain[n - 1] = 0;

    return shm;
}


void msgq_shm_delete(msgq_shm_t *shm)
{
    free(shm->mem);

    *shm = (msgq_shm_t) { 0 };
}
