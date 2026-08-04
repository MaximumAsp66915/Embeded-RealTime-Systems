/*
 * history_log.c — see history_log.h
 */

#define _POSIX_C_SOURCE 200809L

#include "history_log.h"
#include "persons_reader.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define HISTORY_SLOTS 5

typedef struct {
    int count;
    char timestamp[64];
} history_record_t;

static history_record_t g_records[HISTORY_SLOTS];
static int g_filled = 0;      /* how many slots actually hold data (0..5) */
static int g_next_slot = 0;   /* ring buffer write position */
static char g_last_seen_timestamp[64] = "";
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void push_record(int count, const char *timestamp) {
    pthread_mutex_lock(&g_lock);
    g_records[g_next_slot].count = count;
    snprintf(g_records[g_next_slot].timestamp, sizeof(g_records[g_next_slot].timestamp), "%s", timestamp);
    g_next_slot = (g_next_slot + 1) % HISTORY_SLOTS;
    if (g_filled < HISTORY_SLOTS) g_filled++;
    pthread_mutex_unlock(&g_lock);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void *poll_loop(void *arg) {
    const server_config_t *cfg = (const server_config_t *)arg;

    for (;;) {
        persons_info_t p;
        if (persons_reader_read(cfg->persons_path, &p) == 0 &&
            strcmp(p.timestamp, "unavailable") != 0 &&
            strcmp(p.timestamp, g_last_seen_timestamp) != 0) {
            /* A genuinely new detector frame (timestamp advanced), not
             * just us polling faster than person_detector.py updates. */
            snprintf(g_last_seen_timestamp, sizeof(g_last_seen_timestamp), "%s", p.timestamp);
            push_record(p.count, p.timestamp);
        }
        sleep_ms(cfg->history_poll_interval_ms > 0 ? cfg->history_poll_interval_ms : 2000);
    }

    return NULL; /* unreachable */
}

void history_log_start(const server_config_t *cfg) {
    pthread_t tid;
    /* cfg is only read from the poll thread for the lifetime of the
     * process (it's the same struct main() loaded once at startup and
     * never frees), so passing the pointer through is safe here. */
    if (pthread_create(&tid, NULL, poll_loop, (void *)cfg) != 0) {
        fprintf(stderr, "[history_log] failed to start polling thread\n");
        return;
    }
    pthread_detach(tid);
}

int history_log_get_json(char *out, size_t out_size) {
    pthread_mutex_lock(&g_lock);

    /* Oldest-first order: if the buffer isn't full yet, slot 0 is oldest.
     * Once full, the oldest is at g_next_slot (about to be overwritten). */
    int start = (g_filled < HISTORY_SLOTS) ? 0 : g_next_slot;

    int pos = 0;
    int n = snprintf(out + pos, out_size - pos, "[");
    if (n < 0 || (size_t)n >= out_size - pos) { pthread_mutex_unlock(&g_lock); return -1; }
    pos += n;

    for (int i = 0; i < g_filled; i++) {
        int idx = (start + i) % HISTORY_SLOTS;
        n = snprintf(out + pos, out_size - pos, "%s{\"count\": %d, \"timestamp\": \"%s\"}",
                     (i == 0) ? "" : ", ",
                     g_records[idx].count, g_records[idx].timestamp);
        if (n < 0 || (size_t)n >= out_size - pos) { pthread_mutex_unlock(&g_lock); return -1; }
        pos += n;
    }

    n = snprintf(out + pos, out_size - pos, "]");
    if (n < 0 || (size_t)n >= out_size - pos) { pthread_mutex_unlock(&g_lock); return -1; }
    pos += n;

    pthread_mutex_unlock(&g_lock);
    return pos;
}

int history_log_get_summary_json(char *out, size_t out_size) {
    pthread_mutex_lock(&g_lock);

    int min_count = 0, max_count = 0;
    double sum = 0.0;

    if (g_filled > 0) {
        min_count = g_records[0].count;
        max_count = g_records[0].count;
        for (int i = 0; i < g_filled; i++) {
            int c = g_records[i].count;
            if (c < min_count) min_count = c;
            if (c > max_count) max_count = c;
            sum += c;
        }
    }

    double avg_count = (g_filled > 0) ? (sum / g_filled) : 0.0;
    int filled = g_filled;

    pthread_mutex_unlock(&g_lock);

    int n = snprintf(out, out_size,
        "{\"records_stored\": %d, \"capacity\": %d, \"min_count\": %d, "
        "\"max_count\": %d, \"avg_count\": %.2f}",
        filled, HISTORY_SLOTS, min_count, max_count, avg_count);

    if (n < 0 || (size_t)n >= out_size) return -1;
    return n;
}
