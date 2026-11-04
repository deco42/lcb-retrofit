#ifndef LCBD_CLOCK_H
#define LCBD_CLOCK_H

#include <stdint.h>
#include <time.h>

#define MS_PER_S 1000L
#define US_PER_MS 1000L
#define US_PER_S 1000000L
#define NS_PER_US 1000L
#define NS_PER_MS 1000000L
#define NS_PER_S 1000000000L
#define S_PER_NS 1e-9

static inline double ms_to_s(long ms) { return (double)ms / MS_PER_S; }

// Wraps after 49.7 days, so only differences are meaningful
static inline uint32_t clock_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * MS_PER_S + t.tv_nsec / NS_PER_MS);
}

static inline double clock_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * S_PER_NS;
}

// Only for ns below one second
static inline void clock_add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    if (t->tv_nsec >= NS_PER_S) {
        t->tv_nsec -= NS_PER_S;
        t->tv_sec++;
    }
}

static inline long clock_diff_ns(const struct timespec *later, const struct timespec *earlier)
{
    return (later->tv_sec - earlier->tv_sec) * NS_PER_S + (later->tv_nsec - earlier->tv_nsec);
}

// Truncates the nanosecond difference on its own, so the result can be 1 us above clock_diff_ns / NS_PER_US
static inline long clock_diff_us(const struct timespec *later, const struct timespec *earlier)
{
    return (later->tv_sec - earlier->tv_sec) * US_PER_S + (later->tv_nsec - earlier->tv_nsec) / NS_PER_US;
}

#endif
