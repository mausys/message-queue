#pragma once

#include <stdlib.h>
#include <stdbool.h>
#include "shm.h"



typedef struct producer producer_t;
typedef struct consumer consumer_t;

msgq_shm_t msgq_shm_new(size_t n, size_t msg_size);
void msgq_shm_delete(msgq_shm_t *shm);

producer_t *producer_new(msgq_shm_t *shm);
void producer_delete(producer_t *producer);
consumer_t* consumer_new(msgq_shm_t *shm);
void consumer_delete(consumer_t *consumer);

void* producer_force_put(producer_t *producer);
void* producer_try_put(producer_t *producer);
void* consumer_get_tail(consumer_t *consumer);
void* consumer_get_head(consumer_t *consumer);


