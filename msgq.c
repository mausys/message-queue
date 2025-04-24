#include "msgq_private.h"
#include "shm_private.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <assert.h>


typedef struct msgq {
    unsigned n;
    size_t msg_size;
    atomic_index_t *tail;
    atomic_index_t *head;
    /* circular list for ordering the messages,
    * initialized simple as list[i] = (i + 1) % n,
    * but due to overruns might get scrambled */
    atomic_index_t *list;
    uintptr_t msgs_buffer;
} msgq_t;


typedef struct producer {
    msgq_t msgq;
    index_t head; /* last message in chain that can be used by consumer, chain[head] is always INDEX_END */
    index_t current; /* message used by producer, will become head  */
    index_t overrun; /* message used by consumer when tail moved away by producer, will become current when released by consumer */
} producer_t;


typedef struct consumer {
    msgq_t msgq;
    index_t current;
} consumer_t;



static inline void* mem_offset(void *p, size_t offset)
{
    return (void*)((uintptr_t)p + offset);
}


static void* get_message(const msgq_t *msgq, index_t index)
{
    if (index >= msgq->n) {
        return NULL;
    }

    return (void*)(msgq->msgs_buffer + (index * msgq->msg_size));
}


static index_t get_next(msgq_t *msgq, index_t current)
{
    return atomic_load(&msgq->list[current]);
}

/* set the current message as head */
static index_t append_msg(producer_t *producer)
{
    msgq_t *msgq = &producer->msgq;

    index_t next = get_next(msgq, producer->current);

    /* current message is the new end of chain*/
    atomic_store(&msgq->list[producer->current], INDEX_END);

    if (producer->head == INDEX_END) {
        /* first message */
        atomic_store(msgq->tail, producer->current);
    } else {
        /* append current message to the chain */
        atomic_store(&msgq->list[producer->head], producer->current);
    }

    producer->head = producer->current;

    /* announce the new head for consumer_get_head */
    atomic_store(msgq->head, producer->head);

    return next;
}


static bool producer_move_tail(producer_t *producer, index_t tail)
{
    msgq_t *msgq = &producer->msgq;
    bool tail_consumed = !!(tail & ORIGIN_MASK);
    index_t next = get_next(msgq, tail & INDEX_MASK);

    return atomic_compare_exchange_weak(producer->msgq.tail, &tail, next);
}


/* try to jump over tail blocked by consumer */
static void producer_overrun(producer_t *producer, index_t tail)
{
    msgq_t *msgq = &producer->msgq;
    index_t new_current = get_next(msgq, tail & INDEX_MASK); /* next */
    index_t new_tail = get_next(msgq, new_current); /* after next */

    /* if atomic_compare_exchange_weak fails expected will be overwritten */
    index_t expected = tail;

    if (atomic_compare_exchange_weak(producer->msgq.tail, &expected, new_tail)) {
        producer->current = new_current;
        producer->overrun = tail & INDEX_MASK;
    } else {
        /* consumer just released tail, so use it */
        producer->current = tail & INDEX_MASK;
    }
}

/* inserts the current message into the queue and
 * if the queue is full, discard the last message that is not
 * used by consumer. Returns pointer to new message */
void* producer_force_put(producer_t *producer)
{
    msgq_t *msgq = &producer->msgq;

    if (producer->current == INDEX_END) {
        producer->current = 0;
         return get_message(msgq, producer->current);
    }

    index_t next = append_msg(producer);

    index_t tail = atomic_load(msgq->tail);

    bool consumed = !!(tail & CONSUMED_FLAG);

    bool full = (next == (tail & INDEX_MASK));

    /* only for testing */
    index_t old_current = producer->current;

    if (producer->overrun != INDEX_END) {
        /* we overran the consumer and moved the tail, use overran message as
        * soon as the consumer releases it */
        if (consumed) {
            /* consumer released overrun message, so we can use it */
            /* relist overrun */
            atomic_store(&msgq->list[producer->overrun], next);

            producer->current = producer->overrun;
            producer->overrun = INDEX_END;

        } else {
            /* consumer still blocks overrun message, move the tail again,
             * because the message queue is still full */
            if (producer_move_tail(producer, tail)) {
                producer->current = tail & INDEX_MASK;
            } else {
                /* consumer just released overrun message, so we can use it */
                /* relist overrun */
                atomic_store(&msgq->list[producer->overrun], next);

                producer->current = producer->overrun;
                producer->overrun = INDEX_END;
            }
        }
    } else {
        /* no previous overrun, use next or after next message */
        if (!full) {
            /* message queue not full, simply use next */
            producer->current = next;
        } else {
            if (!consumed) {
                /* message queue is full, but no message has consumed yet, so try to move tail */
                if (producer_move_tail(producer, tail)) {
                    producer->current = tail & INDEX_MASK;
                } else {
                   /* consumer just started and consumed tail
                      if consumer already moved on, we will use tail  */
                    producer_overrun(producer, tail | CONSUMED_FLAG);
                }
            } else {
                /* overrun the consumer, if the consumer keeps tail*/
                producer_overrun(producer, tail);
            }
        }
    }

    assert(old_current != producer->current);

    return get_message(msgq, producer->current);
}


void* consumer_get_head(consumer_t *consumer)
{
    msgq_t *msgq = &consumer->msgq;

    for (;;) {
        index_t tail = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);

        if (tail == INDEX_END) {
            /* or CONSUMED_FLAG doesn't change INDEX_END*/
            return NULL;
        }

        index_t head = atomic_load(msgq->head);

        tail |= CONSUMED_FLAG;

        if (atomic_compare_exchange_weak(msgq->tail, &tail, head | CONSUMED_FLAG)) {
            /* only accept head, if producer didn't move tail,
            *  otherwise the producer could fill the whole queue and the head could be the
            *  producer current message  */
            consumer->current = head;
            break;
        }
    }

    return get_message(msgq, consumer->current);
}


void* consumer_get_tail(consumer_t *consumer)
{
    msgq_t *msgq = &consumer->msgq;
    index_t tail = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);

    if (tail == INDEX_END)
        return NULL;

    if (tail == (consumer->current | CONSUMED_FLAG)) {
        /* try to get next message */
        index_t next = get_next(msgq, consumer->current);

        if (next != INDEX_END) {
            if (atomic_compare_exchange_weak(msgq->tail, &tail, next | CONSUMED_FLAG)) {
                consumer->current = next;
            } else {
                /* producer just moved tail, use it */
                 consumer->current = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);
            }
        }
    } else {
        /* producer moved tail, use it*/
        consumer->current = tail;
    }

    if (consumer->current == INDEX_END) {
        /* nothing produced yet */
        return NULL;
    }

    return get_message(msgq, consumer->current);
}

static void msgq_init(msgq_t *msgq, msgq_shm_t *shm)
{
    *msgq = (msgq_t) {
        .n = shm->n,
        .msg_size = shm->msg_size,
        .tail = msgq_shm_get_tail(shm),
        .head = msgq_shm_get_head(shm),
        .list = msgq_shm_get_list(shm),
        .msgs_buffer = msgq_shm_get_buffer(shm),
    };
}

producer_t *producer_new(msgq_shm_t *shm)
{
    producer_t *producer = malloc(sizeof(producer_t));

    if (!producer)
        return NULL;

    msgq_init(&producer->msgq, shm);

    producer->current = INDEX_END;
    producer->overrun = INDEX_END;
    producer->head = INDEX_END;

    return producer;
}

void producer_delete(producer_t *producer)
{
    free(producer);
}

consumer_t* consumer_new(msgq_shm_t *shm)
{
    consumer_t *consumer = malloc(sizeof(consumer_t));

    if (!consumer)
        return NULL;

    msgq_init(&consumer->msgq, shm);

    consumer->current = INDEX_END;

    return consumer;
}

void consumer_delete(consumer_t *consumer)
{
    free(consumer);
}

index_t producer_get_current(producer_t *producer)
{
    return producer->current;
}

index_t producer_get_overrun(producer_t *producer)
{
    return producer->overrun;
}

index_t consumer_get_current(consumer_t *consumer)
{
    return consumer->current;
}
