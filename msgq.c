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
    uintptr_t msgs_buffer;
    /* producer and consumer can change the tail
    *  the MSB shows who has last modified the tail */
    atomic_index_t *tail;
    /* head is only written by producer and only used
    * in consumer_get_head */
    atomic_index_t *head;
    /* circular queue for ordering the messages,
    * initialized simple as queue[i] = (i + 1) % n,
    * but due to overruns might get scrambled.
    * only producer can modify the queue */
    atomic_index_t *queue;
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


static void* get_msg(const msgq_t *msgq, index_t index)
{
    if (index >= msgq->n) {
        return NULL;
    }

    return (void*)(msgq->msgs_buffer + (index * msgq->msg_size));
}


static index_t get_next(const msgq_t *msgq, index_t current)
{
    return atomic_load(&msgq->queue[current]);
}

static bool move_tail(msgq_t *msgq, index_t tail)
{
    index_t next = get_next(msgq, tail & INDEX_MASK);

    return atomic_compare_exchange_weak(msgq->tail, &tail, next);
}

/* set the current message as head
* get_next(msgq, producer->current) after this call
* will return INDEX_END */
static void enqueue_msg(producer_t *producer)
{
    msgq_t *msgq = &producer->msgq;

    /* current message is the new end of chain*/
    atomic_store(&msgq->queue[producer->current], INDEX_END);

    if (producer->head == INDEX_END) {
        /* first message */
        atomic_store(msgq->tail, producer->current);
    } else {
        /* append current message to the chain */
        atomic_store(&msgq->queue[producer->head], producer->current);
    }

    producer->head = producer->current;

    /* announce the new head for consumer_get_head */
    atomic_store(msgq->head, producer->head);
}


/* try to jump over tail blocked by consumer */
static bool overrun(producer_t *producer, index_t tail)
{
    msgq_t *msgq = &producer->msgq;
    index_t new_current = get_next(msgq, tail & INDEX_MASK); /* next */
    index_t new_tail = get_next(msgq, new_current); /* after next */

    /* if atomic_compare_exchange_weak fails expected will be overwritten */
    index_t expected = tail;

    if (atomic_compare_exchange_weak(msgq->tail, &expected, new_tail)) {
        producer->overrun = tail & INDEX_MASK;
        producer->current = new_current;

        return true;
    } else {
        /* consumer just released tail, so use it */
        producer->current = tail & INDEX_MASK;

        return false;
    }
}


/* inserts the current message into the queue and
 * if the queue is full, discard the last message that is not
 * used by consumer. Returns pointer to new message */
void* producer_force_put(producer_t *producer)
{
    msgq_t *msgq = &producer->msgq;

    index_t next = get_next(msgq, producer->current);

    enqueue_msg(producer);

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
            /* requeue overrun */
            atomic_store(&msgq->queue[producer->overrun], next);

            producer->current = producer->overrun;
            producer->overrun = INDEX_END;
        } else {
            /* consumer still blocks overran message, move the tail again,
             * because the message queue is still full */
            if (move_tail(msgq, tail)) {
                producer->current = tail & INDEX_MASK;
            } else {
                /* consumer just released overrun message, so we can use it */
                /* requeue overrun */
                atomic_store(&msgq->queue[producer->overrun], next);

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
                /* message queue is full, but no message is consumed yet, so try to move tail */
                if (move_tail(msgq, tail)) {
                    /* message queue is full -> tail & INDEX_MASK == next */
                    producer->current = next;
                } else {
                   /*  consumer just started and consumed tail
                    *  we're assuming that consumer flagged tail (tail | CONSUMED_FLAG),
                    *  if this this is not the case, consumer already moved on
                    *  and we will use tail  */
                    overrun(producer, tail | CONSUMED_FLAG);
                }
            } else {
                /* overrun the consumer, if the consumer keeps tail*/
                overrun(producer, tail);
            }
        }
    }

    assert(old_current != producer->current);

    return get_msg(msgq, producer->current);
}


/* trys to insert the current message into the queue */
void* producer_try_put(producer_t *producer)
{
    msgq_t *msgq = &producer->msgq;

    index_t next = get_next(msgq, producer->current);

    index_t tail = atomic_load(msgq->tail);

    bool consumed = !!(tail & CONSUMED_FLAG);

    bool full = (next == (tail & INDEX_MASK));

    if (producer->overrun != INDEX_END) {
        if (consumed) {
            /* consumer released overrun message, so we can use it */
            /* requeue overrun */
            enqueue_msg(producer);

            atomic_store(&msgq->queue[producer->overrun], next);

            producer->current = producer->overrun;
            producer->overrun = INDEX_END;

            return get_msg(msgq, producer->current);
        }
    } else {
        /* no previous overrun, use next or after next message */
        if (!full) {
            enqueue_msg(producer);

            producer->current = next;

            return get_msg(msgq, producer->current);
        }
    }

    return NULL;
}


void* producer_get_current_msg(const producer_t *producer)
{
     return get_msg(&producer->msgq, producer->current);
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
            /* only accept head if producer didn't move tail,
            *  otherwise the producer could fill the whole queue and the head could be the
            *  producers current message  */
            consumer->current = head;
            break;
        }
    }

    return get_msg(msgq, consumer->current);
}


void* consumer_get_tail(consumer_t *consumer)
{
    msgq_t *msgq = &consumer->msgq;
    index_t tail = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);

    if (tail == INDEX_END)
        return NULL;

    if (tail & CONSUMED_FLAG) {
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

    return get_msg(msgq, consumer->current);
}

static void msgq_init(msgq_t *msgq, msgq_shm_t *shm)
{
    *msgq = (msgq_t) {
        .n = shm->n,
        .msg_size = shm->msg_size,
        .tail = msgq_shm_get_tail(shm),
        .head = msgq_shm_get_head(shm),
        .queue = msgq_shm_get_list(shm),
        .msgs_buffer = msgq_shm_get_buffer(shm),
    };
}

producer_t *producer_new(msgq_shm_t *shm)
{
    producer_t *producer = malloc(sizeof(producer_t));

    if (!producer)
        return NULL;

    msgq_init(&producer->msgq, shm);

    producer->current = 0;
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
