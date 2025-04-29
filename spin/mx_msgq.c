#include <pthread.h>
#include <stdio.h>
#include <assert.h>


//  verify -p -E 7_q_update.c # using embedded printfs
// or:
//  modex -run 7_q_update.c; verify replay
//
// illustrates importing a data structure
// and defining an atomic function (cas)
// both are done with a .prx file

#define NUM_MSGS 5


#define INDEX_END (~(unsigned)0)

#define CONSUMED_FLAG ((unsigned)1 << (31))

#define ORIGIN_MASK CONSUMED_FLAG

#define INDEX_MASK (~ORIGIN_MASK)



typedef struct msgq msgq_t;

struct msgq {
	unsigned tail;
	unsigned head;
	unsigned queue[NUM_MSGS];
};


typedef struct producer {
	msgq_t *msgq;
	unsigned head; /* last message in chain that can be used by consumer, chain[head] is always INDEX_END */
	unsigned current; /* message used by producer, will become head  */
	unsigned overrun; /* message used by consumer when tail moved away by producer, will become current when released by consumer */
} producer_t;


typedef struct consumer {
	msgq_t *msgq;
	unsigned current;
} consumer_t;


producer_t g_producer;

consumer_t g_consumer;

msgq_t g_msgq;



/* set the current message as head */
inline unsigned append_msg(producer_t *producer)
{
	msgq_t *msgq;
	msgq = producer->msgq;
	unsigned next;
	
	next = msgq->queue[producer->current];
	msgq = producer->msgq;

	/* current message is the new end of chain*/
	msgq->queue[producer->current] = INDEX_END;

	if (producer->head == INDEX_END) {
		/* first message */
		msgq->tail = producer->current;
	} else {
		/* append current message to the chain */
		msgq->queue[producer->head] = producer->current;
	}

	producer->head = producer->current;

	/* announce the new head for consumer_get_head */
	msgq->head = producer->head;

	return next;
}



inline int producer_move_tail(producer_t *producer, unsigned tail)
{
	msgq_t *msgq;
	unsigned next;
	int ret;

	msgq = producer->msgq;
	next = msgq->queue[tail & INDEX_MASK];

	ret = atomic_compare_exchange_weak(&msgq->tail, &tail, next);
	
	return ret;
}


/* try to jump over tail blocked by consumer */
inline void producer_overrun(producer_t *producer, unsigned tail)
{
	int r;
	msgq_t *msgq;
	unsigned new_current, new_tail, expected;

	msgq = producer->msgq;
	new_current = msgq->queue[tail & INDEX_MASK]; /* next */
	new_tail  = msgq->queue[new_current]; /* after next */
		
	/* if atomic_compare_exchange_weak fails expected will be overwritten */
	expected = tail;
	
	r = atomic_compare_exchange_weak(&producer->msgq->tail, &expected, new_tail);
	if (r) {
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
inline void producer_force_put(producer_t *producer)
{
	msgq_t *msgq;
	msgq = producer->msgq;
	unsigned next, tail; 
	int consumed, full;

	if (producer->current == INDEX_END) {
		producer->current = 0;
		return;
	}

	next = append_msg(producer);

	tail = msgq->tail;

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
			if (producer_move_tail(producer, tail)) {
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
		} else {
			if (!consumed) {
				/* message queue is full, but no message is consumed yet, so try to move tail */
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
}



inline void consumer_get_tail(consumer_t *consumer)
{
	msgq_t *msgq;
	unsigned tail;

	msgq = consumer->msgq;

	tail = atomic_fetch_or(&msgq->tail, CONSUMED_FLAG);

	if (tail == INDEX_END)
		return;

	if (tail == (consumer->current | CONSUMED_FLAG)) {
		unsigned next;
		/* try to get next message */
		next = msgq->queue[consumer->current];

		if (next != INDEX_END) {
			int r;
			r = atomic_compare_exchange_weak(&msgq->tail, &tail, next | CONSUMED_FLAG);
			if (r) {
				consumer->current = next;
			} else {
				/* producer just moved tail, use it */
				consumer->current = atomic_fetch_or(&msgq->tail, CONSUMED_FLAG);
			}
		}
	} else {
		/* producer moved tail, use it*/
		consumer->current = tail;
	}
}

void *producer(void *arg)
{	
	int i;
	for (i = 0; i < 10; i++) {
		producer_force_put(&g_producer);
		assert(g_producer.current != g_consumer.current);
	}
	
	return NULL;
}

void* consumer(void *arg)
{
	for (;;) {
		consumer_get_tail(&g_consumer);
		assert((g_producer.current != g_consumer.current) || (g_consumer.current == INDEX_END));
	}
}

int
main(void)
{	
	int i;
	pthread_t  a, b;
	
	g_msgq.tail = INDEX_END;
	g_msgq.head = INDEX_END;
	
	for (i = 0; i < NUM_MSGS - 1; i++)
		g_msgq.queue[i] = i + 1;
	
	g_msgq.queue[NUM_MSGS - 1] = 0; /* wrap around */
	
	g_producer.msgq = &g_msgq;
	g_producer.current = INDEX_END;
	g_producer.head = INDEX_END;
	g_producer.overrun = INDEX_END;
	
	g_consumer.msgq = &g_msgq;
	g_consumer.current = INDEX_END;
	
	

	pthread_create(&a, 0, producer, 0);
	pthread_create(&b, 0, consumer, 0);


	return 0;
}
