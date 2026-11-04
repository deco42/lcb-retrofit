#include "shared.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "clock.h"

#define REQUEST_TIMEOUT_MS 2000
#define REQUEST_POLL_MS 2
#define PAUSE_STEP_MS 100

void shared_init(Shared *sh)
{
    memset(sh, 0, sizeof(*sh));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&sh->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    nlr_init(&sh->link);
    station_init(&sh->station);
    io_init(&sh->io);
}

bool shared_request(Shared *sh, const ServiceRequest *request, char *reply, size_t size)
{
    // Only the web thread posts, one request at a time, so the mailbox is free here
    pthread_mutex_lock(&sh->lock);
    sh->request = *request;
    sh->request_state = REQUEST_PENDING;
    pthread_mutex_unlock(&sh->lock);

    for (int i = 0; i < REQUEST_TIMEOUT_MS / REQUEST_POLL_MS && !atomic_load(&sh->stop); i++) {
        usleep(REQUEST_POLL_MS * US_PER_MS);
        pthread_mutex_lock(&sh->lock);
        if (sh->request_state == REQUEST_DONE) {
            bool ok = sh->request_ok;
            snprintf(reply, size, "%s", sh->request_reply);
            sh->request_state = REQUEST_IDLE;
            pthread_mutex_unlock(&sh->lock);
            return ok;
        }

        pthread_mutex_unlock(&sh->lock);
    }

    pthread_mutex_lock(&sh->lock);
    sh->request_state = REQUEST_IDLE; // drop the unanswered request
    pthread_mutex_unlock(&sh->lock);
    snprintf(reply, size, "no answer from the controller");

    return false;
}

void shared_pause(Shared *sh, Worker worker, int ms)
{
    for (int i = 0; i < ms && !atomic_load(&sh->stop); i += PAUSE_STEP_MS) {
        shared_progress(sh, worker);
        usleep(PAUSE_STEP_MS * US_PER_MS);
    }
}

void shared_history_push(Shared *sh, const HistoryEntry *entry)
{
    sh->history[sh->history_head] = *entry;
    sh->history_head = (sh->history_head + 1) % HISTORY_MAX;
    if (sh->n_history < HISTORY_MAX) {
        sh->n_history++;
    }
}

int shared_history_copy(const Shared *sh, HistoryEntry out[HISTORY_MAX])
{
    int n = sh->n_history;
    for (int i = 0; i < n; i++) {
        out[i] = sh->history[(sh->history_head - n + i + HISTORY_MAX) % HISTORY_MAX];
    }

    return n;
}
