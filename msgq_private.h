#pragma once

#include "msgq.h"

#include "index.h"

index_t producer_get_current(producer_t *producer);
index_t consumer_get_current(consumer_t *consumer);
index_t producer_get_overrun(producer_t *producer);
