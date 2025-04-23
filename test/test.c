#include "msgq.h"
#include "msgq_private.h"

#include <stdlib.h>
#include <stdint.h>
#include <check.h>

#include "fiber.h"


static msgq_shm_t g_shm;



#define DEFAULT_STACK_SIZE 4024
#define NUM_MESSAGE 5
#define COUNTER_INIT 100

typedef struct msg {
    uint64_t counter;
} msg_t;

typedef struct {
    atomic_uint *object;
    unsigned *expected;
    unsigned desired;
} cas_t;

typedef struct {
    cas_t cas;
    unsigned msg_cnt;
    unsigned error_cnt;
    bool active;
    fid_t fid;
} fiber_data_t;

typedef struct {
    producer_t *producer;
    msg_t *msg;
    uint64_t counter;
    fiber_data_t fiber;
} producer_data_t;


typedef struct {
    consumer_t *consumer;
    msg_t *msg;
    fiber_data_t fiber;
} consumer_data_t;


typedef enum {
    RUN_PRODUCER,
    RUN_CONSUMER,
    WAIT_PRODUCER_NCAS,
    WAIT_CONSUMER_NCAS,
} step_action_t;

typedef struct {
    step_action_t action;
    unsigned num;
} step_t;

typedef struct {
    const step_t *steps;
    unsigned num_steps;
    unsigned step;
    unsigned substep;
} sched_seq_t;


static consumer_data_t g_consumer_data;
static producer_data_t g_producer_data;




static void produce(unsigned n);

static uint64_t consume(unsigned n, uint64_t counter);

#define MAX_WAIT_CHANGE 100


bool fiber_atomic_compare_exchange_weak(atomic_uint *object, unsigned *expected, unsigned desired)
{
    fiber_data_t *cfd = &g_consumer_data.fiber;
    fiber_data_t *pfd = &g_producer_data.fiber;

    fid_t fid = fiber_self();

    if (fid == cfd->fid) {
        cfd->cas = (cas_t) {
            .object = object,
            .expected = expected,
            .desired = desired,
        };
    } else if (fid == pfd->fid) {
        pfd->cas = (cas_t) {
            .object = object,
            .expected = expected,
            .desired = desired,
        };
    }

    fiber_yield();

    if (*object == *expected) {
        *object = desired;
        return true;
    } else {
        *expected = *object;
        return false;
    }
}

static void  consumer_error(void)
{
     consumer_data_t *cd = &g_consumer_data;
    cd->fiber.error_cnt++;
}


static void producer_error(void)
{
    producer_data_t *pd = &g_producer_data;
    pd->fiber.error_cnt++;
}

static fid_t fiber_next(fid_t current, void *user_data)
{
    sched_seq_t *seq = user_data;

    consumer_data_t *cd = &g_consumer_data;
    producer_data_t *pd = &g_producer_data;

    bool next_step = false;

    const step_t *step = &seq->steps[seq->step];

    fid_t fid = FIBER_ID_INVAL;

    switch (step->action) {
    case RUN_PRODUCER:
        fid =  pd->fiber.fid;
        break;
    case RUN_CONSUMER:
         fid = cd->fiber.fid;
        break;
    case WAIT_PRODUCER_NCAS:
        if (*pd->fiber.cas.object == *pd->fiber.cas.expected) {
            fid =  cd->fiber.fid;
        } else {
            fid =  pd->fiber.fid;
            next_step = true;
        }
        break;
    case WAIT_CONSUMER_NCAS:
        if (*cd->fiber.cas.object == *cd->fiber.cas.expected) {
            fid =  pd->fiber.fid;
        } else {
            fid =  cd->fiber.fid;
            next_step = true;
        }
        break;
    }

    seq->substep++;

    if (seq->substep >= step->num) {
        if (step->action == WAIT_PRODUCER_NCAS)
            producer_error();

        if (step->action == WAIT_CONSUMER_NCAS)
            consumer_error();

        seq->substep = 0;
        seq->step++;
    } else if (next_step) {
        seq->substep = 0;
        seq->step++;
    }

    if (seq->step >= seq->num_steps) {
        cd->fiber.active = false;
        pd->fiber.active = false;
        fid = FIBER_ID_INVAL;
    }


    return fid;
}

static void produce_one(void)
{
    msg_t consumer_old;
    consumer_data_t *cd = &g_consumer_data;
    producer_data_t *pd = &g_producer_data;

    if (cd->msg) {
        consumer_old = *cd->msg;
    }

    pd->msg  = producer_force_put(pd->producer);
    ck_assert_ptr_nonnull(pd->msg);

    if (g_consumer_data.msg) {
        ck_assert_mem_eq(&consumer_old, cd->msg, sizeof(consumer_old));
    }

    ck_assert_ptr_ne(cd->msg, pd->msg);

    pd->msg->counter = pd->counter;
    pd->counter++;
}


static uint64_t consume_one(uint64_t expected, bool missing)
{
    consumer_data_t *cd = &g_consumer_data;
    producer_data_t *pd = &g_producer_data;

    cd->msg = consumer_get_tail(cd->consumer);
    ck_assert_ptr_nonnull(cd->msg);

    if (missing) {
        ck_assert_uint_ge(cd->msg->counter, expected);
    } else {
        ck_assert_uint_eq(cd->msg->counter, expected);
    }

    ck_assert_ptr_ne(cd->msg, pd->msg);

    return cd->msg->counter;
}


static void producer_run(void *user_data)
{
    consumer_data_t *cd = &g_consumer_data;
    producer_data_t *pd = &g_producer_data;

    while (pd->fiber.active) {
        pd->msg  = producer_force_put(pd->producer);
        pd->fiber.msg_cnt++;

        if (!pd->msg) {
            producer_error();
            continue;
        }

        if (pd->msg == cd->msg)
            pd->counter++;

        pd->msg->counter = pd->counter;
        pd->counter++;
    }
}


static void consumer_run(void *user_data)
{
    uint64_t counter = COUNTER_INIT;

    consumer_data_t *cd = &g_consumer_data;
    producer_data_t *pd = &g_producer_data;

    while (cd->fiber.active) {
        cd->msg = consumer_get_tail(cd->consumer);
        pd->fiber.msg_cnt++;
        if (!cd->msg) {
            consumer_error();
            continue;
        }

        if (pd->msg == cd->msg)
            pd->counter++;


        if (cd->msg->counter < counter) {
            consumer_error();
        }

        counter = cd->msg->counter;
    }
}

static void producer_data_init(producer_data_t *pd, msgq_shm_t *shm)
{
    if (pd->producer) {
         producer_delete(pd->producer);
    }

    pd->producer = producer_new(shm);
    pd->counter = COUNTER_INIT;
    pd->msg = NULL;
}


static void consumer_data_init(consumer_data_t *cd, msgq_shm_t *shm)
{
    if (cd->consumer) {
        consumer_delete(cd->consumer);
    }

    cd->consumer = consumer_new(shm);
    cd->msg = NULL;
}

void setup(void)
{
    g_shm = msgq_shm_new(NUM_MESSAGE, sizeof(msg_t));
    producer_data_init(&g_producer_data, &g_shm);
    consumer_data_init(&g_consumer_data, &g_shm);
}

void teardown(void)
{
    producer_data_t *pd = &g_producer_data;
    consumer_data_t *cd = &g_consumer_data;

    producer_delete(pd->producer);
    pd->producer = NULL;

    consumer_delete(cd->consumer);
    cd->consumer = NULL;

    msgq_shm_delete(&g_shm);
}


void setup_fibers(void)
{
    setup();

    producer_data_t *pd = &g_producer_data;
    consumer_data_t *cd = &g_consumer_data;

    cd->fiber = (fiber_data_t) {
        .active = true,
    };

    pd->fiber = (fiber_data_t) {
        .active = true,
    };

    pd->fiber.fid = fiber_new(DEFAULT_STACK_SIZE, producer_run, NULL);
    cd->fiber.fid = fiber_new(DEFAULT_STACK_SIZE, consumer_run, NULL);
}


void teardown_fibers(void)
{
    teardown();
    fiber_reset();
}

static uint64_t consume(unsigned n, uint64_t counter)
{
    for (unsigned i = 0; i < n; i++) {
        consume_one(counter,false);
        counter++;
    }
    return counter;
}


static void produce(unsigned n)
{

    for (unsigned i = 0; i < n; i++) {
        produce_one();
    }
}



START_TEST(test_empty)
{
    consumer_data_t *cd = &g_consumer_data;

    cd->msg = consumer_get_tail(cd->consumer);
    ck_assert_ptr_null(cd->msg);

    cd->msg = consumer_get_head(cd->consumer);
    ck_assert_ptr_null(cd->msg);
}
END_TEST

START_TEST(test_one)
{
    consumer_data_t *cd = &g_consumer_data;

    produce(1);

    cd->msg = consumer_get_tail(cd->consumer);
    ck_assert_ptr_null(cd->msg);

    produce(1);

    cd->msg = consumer_get_tail(cd->consumer);
    ck_assert_ptr_nonnull(cd->msg);
    ck_assert_uint_eq(cd->msg->counter, COUNTER_INIT);

}
END_TEST


START_TEST(test_fill)
{
    produce(NUM_MESSAGE);
    consume(NUM_MESSAGE - 1, COUNTER_INIT);
}
END_TEST


START_TEST(test_refill)
{
    produce(NUM_MESSAGE);
    uint64_t counter = consume(NUM_MESSAGE - 1, COUNTER_INIT);
    produce(NUM_MESSAGE - 2);
    consume(NUM_MESSAGE - 2, counter);
}
END_TEST

START_TEST(test_part_refill)
{
    produce(NUM_MESSAGE - 1);
    uint64_t counter = consume(2, COUNTER_INIT);
    produce(2);
    consume(NUM_MESSAGE - 2, counter);
}
END_TEST

START_TEST(test_discard_one)
{
   produce(NUM_MESSAGE);
   consume(NUM_MESSAGE - 1, COUNTER_INIT);
}
END_TEST

START_TEST(test_discard_3)
{
    produce(NUM_MESSAGE + 2);
    consume(NUM_MESSAGE - 1, COUNTER_INIT + 2);
}
END_TEST

START_TEST(test_overrun)
{
    produce(NUM_MESSAGE);
    uint64_t counter = consume(1, COUNTER_INIT);
    produce(1);
    consume(1, counter + 1);
}
END_TEST


START_TEST(test_overrun_2)
{
    produce(NUM_MESSAGE);
    uint64_t counter = consume(1, COUNTER_INIT);
    produce(NUM_MESSAGE - 2);
    consume(NUM_MESSAGE - 2, counter + 3);
}
END_TEST



START_TEST(test_overrun_3)
{
    produce(NUM_MESSAGE);
    uint64_t counter = consume(1, COUNTER_INIT);
    produce(NUM_MESSAGE - 2);
    counter = consume(NUM_MESSAGE - 2, counter + 3);
    produce(NUM_MESSAGE - 2);
    consume(NUM_MESSAGE - 2, counter);
}
END_TEST



START_TEST(test_fiber_move_tail)
{
    consumer_data_t *cd = &g_consumer_data;
    producer_data_t *pd = &g_producer_data;

    const step_t steps[] = {
        { .action = RUN_PRODUCER, .num = NUM_MESSAGE },
        { .action = WAIT_PRODUCER_NCAS, .num = 3 },
        { .action = RUN_PRODUCER, .num = NUM_MESSAGE - 1 },
        { .action = RUN_CONSUMER, .num = NUM_MESSAGE - 1 },
    };

    sched_seq_t seq = {
        .steps = steps,
        .num_steps = sizeof(steps) / sizeof(steps[0]),
    };

    fiber_set_sched(fiber_next, &seq);

    fiber_run();

    ck_assert_uint_eq(cd->fiber.error_cnt, 0);
    ck_assert_uint_eq(pd->fiber.error_cnt, 0);
}
END_TEST


START_TEST(test_fiber_move_overrun)
{
    consumer_data_t *cd = &g_consumer_data;
    producer_data_t *pd = &g_producer_data;

    const step_t steps[] = {
        { .action = RUN_PRODUCER, .num = 2 },
        { .action = RUN_CONSUMER, .num = 1 },
        { .action = RUN_PRODUCER, .num = NUM_MESSAGE - 1 },
        { .action = WAIT_PRODUCER_NCAS, .num = 3 },
        { .action = RUN_PRODUCER, .num = NUM_MESSAGE - 1 },
        { .action = RUN_CONSUMER, .num = NUM_MESSAGE - 1 },
    };

    sched_seq_t seq = {
        .steps = steps,
        .num_steps = sizeof(steps) / sizeof(steps[0]),
    };

    fiber_set_sched(fiber_next, &seq);

    fiber_run();

    ck_assert_uint_eq(cd->fiber.error_cnt, 0);
    ck_assert_uint_eq(pd->fiber.error_cnt, 0);
}
END_TEST


Suite* msgq_suite(void)
{
    Suite *s = suite_create("serial");

    TCase *tc = tcase_create("msqg");
    tcase_add_checked_fixture(tc, setup, teardown);
    tcase_add_test(tc, test_empty);
    tcase_add_test(tc, test_one);
    tcase_add_test(tc, test_fill);
    tcase_add_test(tc, test_refill);
    tcase_add_test(tc, test_part_refill);
    tcase_add_test(tc, test_discard_one);
    tcase_add_test(tc, test_discard_3);
    tcase_add_test(tc, test_overrun);
    tcase_add_test(tc, test_overrun_2);
    tcase_add_test(tc, test_overrun_3);
    suite_add_tcase(s, tc);

    TCase *tc_fiber = tcase_create("fiber");
    tcase_add_checked_fixture(tc_fiber, setup_fibers, teardown_fibers);
    tcase_add_test(tc_fiber, test_fiber_move_tail);
    tcase_add_test(tc_fiber, test_fiber_move_overrun);

    suite_add_tcase(s, tc_fiber);

    return s;
}


int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = msgq_suite();
    sr = srunner_create(s);
    /* for debugging */
    srunner_set_fork_status(sr,CK_NOFORK);
    srunner_run_all(sr, CK_VERBOSE);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
