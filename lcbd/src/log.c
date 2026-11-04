#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"

#define RING_MAX 1024 // lines on the way from log_msg to the writer thread
#define WRITER_PERIOD_MS 20

typedef struct {
    atomic_ulong sequence; // index + 1 when the slot holds that line, 0 while it is written
    double time_s;
    char text[LOG_TEXT_MAX];
} Slot;

static Slot ring[RING_MAX];
static atomic_ulong write_index;
static atomic_bool running;
static struct timespec start_time;
static pthread_t writer_thread;
static pthread_mutex_t recent_lock = PTHREAD_MUTEX_INITIALIZER;
static char recent[LOG_RECENT_MAX][LOG_LINE_MAX];
static int recent_head, n_recent;

static void print_line(double time_s, const char *text)
{
    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line), "%.3f %s", time_s, text);
    puts(line);
    pthread_mutex_lock(&recent_lock);
    snprintf(recent[recent_head], sizeof(recent[0]), "%s", line);
    recent_head = (recent_head + 1) % LOG_RECENT_MAX;
    if (n_recent < LOG_RECENT_MAX) {
        n_recent++;
    }

    pthread_mutex_unlock(&recent_lock);
}

static unsigned long print_complete(unsigned long next)
{
    for (;;) {
        Slot *slot = &ring[next % RING_MAX];
        unsigned long sequence = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        if (sequence > next + 1) {
            char lost[64];
            snprintf(lost, sizeof(lost), "(log overrun, %lu lines lost)", sequence - 1 - next);
            print_line(log_uptime(), lost);
            next = sequence - 1;
            continue;
        }

        if (sequence != next + 1) {
            return next;
        }

        char text[LOG_TEXT_MAX];
        double time_s = slot->time_s;
        memcpy(text, slot->text, sizeof(text));
        text[LOG_TEXT_MAX - 1] = '\0';
        if (atomic_load_explicit(&slot->sequence, memory_order_acquire) != sequence) {
            continue; // overwritten while copying
        }

        print_line(time_s, text);
        next++;
    }
}

static void *writer_main(void *arg)
{
    (void)arg;
    unsigned long next = 0;

    while (atomic_load(&running)) {
        next = print_complete(next);
        fflush(stdout);
        usleep(WRITER_PERIOD_MS * US_PER_MS);
    }

    print_complete(next);
    fflush(stdout);

    return NULL;
}

void log_msg(const char *fmt, ...)
{
    unsigned long index = atomic_fetch_add(&write_index, 1);
    Slot *slot = &ring[index % RING_MAX];
    atomic_store_explicit(&slot->sequence, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    slot->time_s = log_uptime();
    va_list args;
    va_start(args, fmt);
    vsnprintf(slot->text, sizeof(slot->text), fmt, args);
    va_end(args);
    atomic_store_explicit(&slot->sequence, index + 1, memory_order_release);
}

bool log_start(void)
{
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    atomic_store(&running, true);

    return pthread_create(&writer_thread, NULL, writer_main, NULL) == 0;
}

void log_stop(void)
{
    atomic_store(&running, false);
    pthread_join(writer_thread, NULL);
}

int log_recent(char lines[LOG_RECENT_MAX][LOG_LINE_MAX])
{
    pthread_mutex_lock(&recent_lock);

    int n = n_recent;
    for (int i = 0; i < n; i++) {
        memcpy(lines[i], recent[(recent_head - n + i + LOG_RECENT_MAX) % LOG_RECENT_MAX], sizeof(recent[0]));
    }

    pthread_mutex_unlock(&recent_lock);

    return n;
}

double log_uptime(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (double)(now.tv_sec - start_time.tv_sec) + (double)(now.tv_nsec - start_time.tv_nsec) * S_PER_NS;
}
