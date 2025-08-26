#include <pthread.h>
#include <stdio.h>
#include <assert.h>

#ifndef MODEX
#include <stdatomic.h>
#endif

//  verify -p -E 7_q_update.c # using embedded printfs
// or:
//  modex -run 7_q_update.c; verify replay
//
// illustrates importing a data structure
// and defining an atomic function (cas)
// both are done with a .prx file

#define NUM_MSGS 5


#define INDEX_END     0xffffffff
#define CONSUMED_FLAG 0x80000000
#define INDEX_MASK    0x7fffffff

#define ORIGIN_MASK CONSUMED_FLAG


typedef struct msgq {
	unsigned *tail;
	unsigned *head;
	unsigned *queue;
} msgq_t;


typedef struct producer {
	msgq_t msgq;
	unsigned head; /* last message in chain that can be used by consumer, chain[head] is always INDEX_END */
	unsigned current; /* message used by producer, will become head  */
	unsigned overrun; /* message used by consumer when tail moved away by producer, will become current when released by consumer */
} producer_t;


typedef struct consumer {
	msgq_t msgq;
	unsigned current;
} consumer_t;


unsigned g_producer_current = INDEX_END;
unsigned g_consumer_current = INDEX_END;



unsigned g_msgq_shm_tail = INDEX_END;
unsigned g_msgq_shm_head = INDEX_END;
unsigned g_msgq_shm_queue[NUM_MSGS];




/* set the current message as head */
unsigned enqueue_msg(producer_t *producer)
{
	msgq_t *msgq;
	unsigned next;

	msgq = &producer->msgq;

	next = msgq->queue[producer->current];

	/* current message is the new end of chain*/
	msgq->queue[producer->current] = INDEX_END;

	if (producer->head == INDEX_END) {
		/* first message */
		*msgq->tail = producer->current;
	} else {
		/* append current message to the chain */
		msgq->queue[producer->head] = producer->current;
	}

	producer->head = producer->current;

	/* announce the new head for consumer_get_head */
	*msgq->head = producer->head;

	return next;
}



int producer_move_tail(producer_t *producer, unsigned tail)
{
	msgq_t *msgq;
	unsigned next;
	int b;

	msgq = &producer->msgq;
	next = msgq->queue[tail & INDEX_MASK];

	b = atomic_compare_exchange_strong(msgq->tail, &tail, next);
	
	return b;
}


/* try to jump over tail blocked by consumer */
void producer_overrun(producer_t *producer, unsigned tail)
{
	int b;
	msgq_t *msgq;
	unsigned new_current, new_tail, expected;

	msgq = &producer->msgq;
	new_current = msgq->queue[tail & INDEX_MASK]; /* next */
	new_tail  = msgq->queue[new_current]; /* after next */
		
	/* if atomic_compare_exchange_strong fails expected will be overwritten */
	expected = tail;
	
	b = atomic_compare_exchange_strong(msgq->tail, &expected, new_tail);
	if (b) {
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
unsigned producer_force_push(producer_t *producer)
{
	msgq_t *msgq;
	msgq = &producer->msgq;
	unsigned next, tail; 
	int consumed, full;

	if (producer->current == INDEX_END) {
		producer->current = 0;
		return producer->current;
	}

	next = enqueue_msg(producer);

	tail = *msgq->tail;

	consumed = !!(tail & CONSUMED_FLAG);

	full = (next == (tail & INDEX_MASK));


	if (producer->overrun != INDEX_END) {
		/* we overran the consumer and moved the tail, use overran message as
		* soon as the consumer releases it */
		if (consumed) {
			/* consumer released overrun message, so we can use it */
			/* requeue overrun */
			msgq->queue[producer->overrun] = next;

			producer->current = producer->overrun;
			producer->overrun = INDEX_END;
		} else {
			/* consumer still blocks overran message, move the tail again,
			* because the message queue is still full */
			int b;
			b = producer_move_tail(producer, tail);
			if (b) {
				producer->current = tail & INDEX_MASK;
			} else {
				/* consumer just released overrun message, so we can use it */
				/* requeue overrun */
				msgq->queue[producer->overrun] = next;

				producer->current = producer->overrun;
				producer->overrun = INDEX_END;
			}
		}
	} else {
		/* no previous overrun, use next or after next message */
		if (!full) {
			/* message queue not full, simply use next */
			producer->current = next;
		} else if (!consumed) {
			/* message queue is full, but no message is consumed yet, so try to move tail */
			int b;
			b = producer_move_tail(producer, tail);
			if (b) {
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
	return producer->current;
}



unsigned consumer_pop(consumer_t *consumer)
{
	msgq_t *msgq;
	unsigned tail;

	msgq = &consumer->msgq;

	tail = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);

	if (tail == INDEX_END) {
		return INDEX_END;
	}

	if ((tail & CONSUMED_FLAG) == 0) {
		consumer->current = tail;
		return consumer->current;
	}
	unsigned next;
	/* try to get next message */
	next = msgq->queue[consumer->current];
	
	if (next == INDEX_END) {
		return consumer->current;
	}

	
	int r;
	r = atomic_compare_exchange_strong(msgq->tail, &tail, next | CONSUMED_FLAG);
	if (r) {
		consumer->current = next;
	} else {
		/* producer just moved tail, use it */
		consumer->current = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);
	}
	return consumer->current;
}

void *producer_thread(void *arg)
{	
	producer_t producer;
	msgq_t *msgq;
	int i;
	
	msgq = &producer.msgq;
	
	for (i = 0; i < NUM_MSGS - 1; i++) {
		g_msgq_shm_queue[i] = i + 1;
	}
	
	g_msgq_shm_queue[NUM_MSGS - 1] = 0;
	
	producer.current = INDEX_END;
	producer.head = INDEX_END;
	producer.overrun = INDEX_END;
	
	msgq->head = &g_msgq_shm_head;
	msgq->tail = &g_msgq_shm_tail;
	msgq->queue = g_msgq_shm_queue;
	

	for (i = 0; i < NUM_MSGS + 2; i++) {
		g_producer_current = INDEX_END;
		g_producer_current = producer_force_push(&producer);
		assert(g_producer_current != g_consumer_current);
	}
	
	return NULL;
}

void* consumer_thread(void *arg)
{	
	consumer_t consumer;
	msgq_t *msgq;
	int i;
	
	msgq = &consumer.msgq;
	
	msgq->head = &g_msgq_shm_head;
	msgq->tail = &g_msgq_shm_tail;
	msgq->queue = g_msgq_shm_queue;
	
	consumer.current = INDEX_END;
	
	for (i = 0; i < NUM_MSGS + 2; i++) {
		g_consumer_current = INDEX_END;
		g_consumer_current = consumer_pop(&consumer);
		assert((g_producer_current != g_consumer_current) || (g_consumer_current == INDEX_END));
	}
}

int
main(void)
{	
	int i;
	pthread_t  a, b;
	
	
	pthread_create(&a, 0, producer_thread, 0);
	pthread_create(&b, 0, consumer_thread, 0);

	pthread_join(a, 0);
        pthread_join(b, 0);


	return 0;
}
