#include "msgq.h"
#include "log.h"


#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <threads.h>

#define NUM_MESSAGES 3

typedef struct msg {
    volatile int64_t counter;
} msg_t;

atomic_uintptr_t g_msg_producer;

atomic_uintptr_t g_msg_consumer;

static msgq_shm_t g_shm;
thrd_t thrd_consumer;
thrd_t thrd_producer;

producer_t *g_producer;
consumer_t *g_consumer;

unsigned g_producer_cnt;

unsigned g_consumer_cnt;

#define MAX_CYCLES 100000000

#define PRODUCER_BUSY_CYCLES 10
#define CONSUMER_BUSY_CYCLES 10

int producer_run(void *arg)
{
    int64_t counter = 0;

    msg_t *msg = producer_get_current_msg(g_producer);

    for (g_producer_cnt = 0; g_producer_cnt < MAX_CYCLES; g_producer_cnt++) {
        /* during get/put message, pointer may be the same */
        atomic_store(&g_msg_producer, (uintptr_t)msg);
        msg->counter = counter++;
        for (int i = 0; i < PRODUCER_BUSY_CYCLES; i++) {
            msg->counter = -1;
            uintptr_t msg_consumer = atomic_load(&g_msg_consumer);
            if (msg_consumer == g_msg_producer) {
                LOG_ERR("producer_run error=%u", g_producer_cnt);
            }
        }

        atomic_store(&g_msg_producer, 0);
        msg = producer_force_put(g_producer);

    }

    return 0;
}


int consumer_run(void *arg)
{
    uint64_t counter = 0;

    for (g_consumer_cnt = 0; g_consumer_cnt < MAX_CYCLES; g_consumer_cnt++) {
        /* during get/put message, pointer may be the same */
        atomic_store(&g_msg_consumer, 0);
        msg_t *msg = consumer_get_tail(g_consumer);
        atomic_store(&g_msg_consumer, (uintptr_t)msg);

        if (!msg)
            continue;

        if (counter > msg->counter) {
            LOG_ERR("conusmer_run error counter (%lu) > msg->counter (%lu) | %u %u", counter, msg->counter, g_consumer_cnt, g_producer_cnt);
        }

        counter = msg->counter;
        for (int i = 0; i < CONSUMER_BUSY_CYCLES; i++ ) {
            if (counter != msg->counter) {
                LOG_ERR("conusmer_run error counter (%lu) != msg->counter (%lu) | %u %u", counter, msg->counter, g_consumer_cnt, g_producer_cnt);
            }
            uintptr_t msg_producer = atomic_load(&g_msg_producer);
            if (g_msg_consumer == msg_producer) {
                LOG_ERR("conusmer_run error g_msg_consumer == msg_producer | %u %u", g_consumer_cnt, g_producer_cnt);
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{

    g_shm = msgq_shm_new(NUM_MESSAGES, sizeof(msg_t));
    g_producer = producer_new(&g_shm);
    g_consumer = consumer_new(&g_shm);

    thrd_create(&thrd_producer, producer_run, NULL);
    thrd_create(&thrd_consumer, consumer_run, NULL);

    thrd_join(thrd_producer, 0);
    thrd_join(thrd_consumer, 0);

    producer_delete(g_producer);
    consumer_delete(g_consumer);
    msgq_shm_delete(&g_shm);

    return 0;
}
