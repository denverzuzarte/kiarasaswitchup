#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    uint64_t main_start;
    uint64_t worker_start;
    uint64_t first_parse;
    uint64_t last_parse;
    uint64_t processing_done;
    uint64_t first_write;
    uint64_t last_write;
} PipelineTimes;

/*
 * Nanoseconds from an arbitrary monotonic reference point.
 */
static inline uint64_t timer_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ULL
         + (uint64_t)ts.tv_nsec;
}

static inline double elapsed_ms(uint64_t start, uint64_t end)
{
    return (double)(end - start) / 1000000.0;
}

