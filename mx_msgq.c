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


#define INDEX_INVALID     0xffffffff
#define CONSUMED_FLAG 0x80000000
#define INDEX_MASK    0x7fffffff

#define ORIGIN_MASK CONSUMED_FLAG




#define CONSUME_RESULT_ERROR -2
#define CONSUME_RESULT_NO_MSG -1
#define CONSUME_RESULT_NO_UPDATE 0
#define CONSUME_RESULT_SUCCESS 1
#define CONSUME_RESULT_DISCARDED 2


#define PRODUCE_RESULT_ERROR -2
#define PRODUCE_RESULT_FAIL -1
#define PRODUCE_RESULT_SUCCESS 1
#define PRODUCE_RESULT_DISCARDED 2


typedef struct msgq {
	unsigned *tail;
	unsigned *head;
	unsigned *chain;
} msgq_t;


typedef struct producer {
	msgq_t msgq;
	unsigned head; /* last message in chain that can be used by consumer, chain[head] is always INDEX_INVALID */
	unsigned current; /* message used by producer, will become head  */
	unsigned overrun; /* message used by consumer when tail moved away by producer, will become current when released by consumer */
} producer_t;


typedef struct consumer {
	msgq_t msgq;
	unsigned current;
} consumer_t;


unsigned g_producer_current = INDEX_INVALID;
unsigned g_consumer_current = INDEX_INVALID;



unsigned g_msgq_shm_tail = INDEX_INVALID;
unsigned g_msgq_shm_head = INDEX_INVALID;
unsigned g_msgq_shm_chain[NUM_MSGS];



void enqueue_first_msg(producer_t *producer)
{
	msgq_t *msgq;

	msgq = &producer->msgq;

	/* current message is the new end of chain*/
	msgq->chain[producer->current] = INDEX_INVALID;

	*msgq->tail = producer->current;

	producer->head = producer->current;

	/* announce the new head for consumer_get_head */
	*msgq->head = producer->head;
}

/* set the current message as head */
void enqueue_msg(producer_t *producer)
{
	msgq_t *msgq;

	msgq = &producer->msgq;

	/* current message is the new end of chain*/
	msgq->chain[producer->current] = INDEX_INVALID;

	/* append current message to the chain */
	msgq->chain[producer->head] = producer->current;

	producer->head = producer->current;

	/* announce the new head for consumer_get_head */
	*msgq->head = producer->head;
}



int producer_move_tail(producer_t *producer, unsigned tail)
{
	msgq_t *msgq;
	unsigned next;
	int b;

	msgq = &producer->msgq;
	next = msgq->chain[tail & INDEX_MASK];

	b = atomic_compare_exchange_strong(msgq->tail, &tail, next);
	
	return b;
}


/* try to jump over tail blocked by consumer */
int producer_overrun(producer_t *producer, unsigned tail)
{
	int b;
	msgq_t *msgq;
	unsigned new_current, new_tail, expected;

	msgq = &producer->msgq;
	new_current = msgq->chain[tail & INDEX_MASK]; /* next */
	new_tail  = msgq->chain[new_current]; /* after next */
		
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
	return b;
}



/* inserts the current message into the queue and
 * if the queue is full, discard the last message that is not
 * used by consumer. Returns pointer to new message */
int producer_force_push(producer_t *producer)
{
	msgq_t *msgq;
	msgq = &producer->msgq;
	unsigned next, tail; 
	int consumed, full;
	int discarded;
	discarded = 0;
	
	
	if (producer->current == INDEX_INVALID) {
		producer->current = 0;
		return producer->current;
	}

	next = msgq->chain[producer->current];
	
	if (producer->head == INDEX_INVALID) {
		enqueue_first_msg(producer);
		producer->current = next;
		return PRODUCE_RESULT_SUCCESS;
	}

	enqueue_msg(producer);

	tail = *msgq->tail;

	 if ((tail & INDEX_MASK) >= NUM_MSGS) {
		return PRODUCE_RESULT_ERROR;
	}

	consumed = !!(tail & CONSUMED_FLAG);

	full = (next == (tail & INDEX_MASK));

	if (producer->overrun != INDEX_INVALID) {
		/* we overran the consumer and moved the tail, use overran message as
		* soon as the consumer releases it */
		if (consumed) {
			/* consumer released overrun message, so we can use it */
			/* requeue overrun */
			msgq->chain[producer->overrun] = next;

			producer->current = producer->overrun;
			producer->overrun = INDEX_INVALID;
		} else {
			/* consumer still blocks overran message, move the tail again,
			* because the message queue is still full */
			discarded = producer_move_tail(producer, tail);
			if (discarded) {
				producer->current = tail & INDEX_MASK;
			} else {
				/* consumer just released overrun message, so we can use it */
				/* requeue overrun */
				msgq->chain[producer->overrun] = next;

				producer->current = producer->overrun;
				producer->overrun = INDEX_INVALID;
			}
		}
	} else {
		/* no previous overrun, use next or after next message */
		if (!full) {
			/* message queue not full, simply use next */
			producer->current = next;
		} else if (!consumed) {
			/* message queue is full, but no message is consumed yet, so try to move tail */
			discarded = producer_move_tail(producer, tail);
			if (discarded) {
				producer->current = tail & INDEX_MASK;
			} else {
				/* consumer just started and consumed tail
				if consumer already moved on, we will use tail  */
				producer_overrun(producer, tail | CONSUMED_FLAG);
			}
		} else {
			/* overrun the consumer, if the consumer keeps tail*/
			discarded = producer_overrun(producer, tail);
		}
	}
	if (discarded) {
		return PRODUCE_RESULT_DISCARDED;
	}

	return PRODUCE_RESULT_SUCCESS;
}


int producer_try_push(producer_t *producer)
{
	msgq_t *msgq;
	msgq = &producer->msgq;
	unsigned next, tail; 
	int consumed, full;
	
	if (producer->current == INDEX_INVALID) {
		producer->current = 0;
		return producer->current;
	}

	next = msgq->chain[producer->current];
  

	if (producer->head == INDEX_INVALID) {
		enqueue_first_msg(producer);
		producer->current = next;
		return PRODUCE_RESULT_SUCCESS;
	}



	tail = *msgq->tail;

	if ((tail & INDEX_MASK) >= NUM_MSGS)
		return PRODUCE_RESULT_ERROR;
	
	
	consumed = !!(tail & CONSUMED_FLAG);

	full = (next == (tail & INDEX_MASK));


	if (producer->overrun != INDEX_INVALID) {
		if (consumed) {
			/* consumer released overrun message, so we can use it */
			/* requeue overrun */
			enqueue_msg(producer);

			msgq->chain[producer->overrun] = next;

			producer->current = producer->overrun;
			producer->overrun = INDEX_INVALID;

			return PRODUCE_RESULT_SUCCESS;
		}
	} else {
		/* no previous overrun, use next or after next message */
		if (!full) {
			enqueue_msg(producer);

			producer->current = next;

			return PRODUCE_RESULT_SUCCESS;
		}
	}

	return PRODUCE_RESULT_FAIL;
}


int consumer_pop(consumer_t *consumer)
{
	msgq_t *msgq;
	unsigned tail;

	msgq = &consumer->msgq;

	tail = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);

	if (tail == INDEX_INVALID) {
		return CONSUME_RESULT_NO_MSG;
	}
	
	if ((tail & INDEX_MASK) >= NUM_MSGS) {
		return CONSUME_RESULT_ERROR;
	}
	

	if ((tail & CONSUMED_FLAG) == 0) {
		consumer->current = tail;
		return CONSUME_RESULT_DISCARDED;
	}
	
	unsigned next;
	/* try to get next message */
	next = msgq->chain[consumer->current];
	
	if (next == INDEX_INVALID) {
		return consumer->current;
	}
	
	if (next >= NUM_MSGS) {
		return CONSUME_RESULT_ERROR;
	}

	int r;
	r = atomic_compare_exchange_strong(msgq->tail, &tail, next | CONSUMED_FLAG);
	if (r) {
		consumer->current = next;
		return CONSUME_RESULT_SUCCESS;
	} else {
		/* producer just moved tail, use it */
		consumer->current = atomic_fetch_or(msgq->tail, CONSUMED_FLAG);
	}
	return CONSUME_RESULT_DISCARDED;
}

void *producer_thread(void *arg)
{	
	producer_t producer;
	msgq_t *msgq;
	int i,r;
	
	msgq = &producer.msgq;
	
	for (i = 0; i < NUM_MSGS - 1; i++) {
		g_msgq_shm_chain[i] = i + 1;
	}
	
	g_msgq_shm_chain[NUM_MSGS - 1] = 0;
	
	producer.current = INDEX_INVALID;
	producer.head = INDEX_INVALID;
	producer.overrun = INDEX_INVALID;
	
	msgq->head = &g_msgq_shm_head;
	msgq->tail = &g_msgq_shm_tail;
	msgq->chain = g_msgq_shm_chain;
	

	for (i = 0; i < NUM_MSGS + 2; i++) {
		g_producer_current = INDEX_INVALID;
		r = producer_force_push(&producer);
		assert(g_producer_current != producer.current);
		g_producer_current = producer.current;
		assert(r != PRODUCE_RESULT_ERROR);
		assert(g_producer_current != g_consumer_current);
	}
	
	return NULL;
}

void* consumer_thread(void *arg)
{	
	consumer_t consumer;
	msgq_t *msgq;
	int i, r;
	
	msgq = &consumer.msgq;
	
	msgq->head = &g_msgq_shm_head;
	msgq->tail = &g_msgq_shm_tail;
	msgq->chain = g_msgq_shm_chain;
	
	consumer.current = INDEX_INVALID;
	
	for (i = 0; i < NUM_MSGS + 2; i++) {
		g_consumer_current = INDEX_INVALID;
		r = consumer_pop(&consumer);
		g_consumer_current = consumer.current;
		assert(r != PRODUCE_RESULT_ERROR);
		assert((g_producer_current != g_consumer_current) || (g_consumer_current == INDEX_INVALID));
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
