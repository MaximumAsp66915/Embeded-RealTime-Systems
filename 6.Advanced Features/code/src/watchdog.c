/*
 * watchdog.c — see watchdog.h
 */

#include "watchdog.h"

#include <string.h>
#include <stdio.h>

void watchdog_init(watchdog_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->last_change_epoch = time(NULL); /* start the clock from process startup */
}

int watchdog_check(watchdog_state_t *state, const char *current_timestamp, int timeout_seconds) {
    time_t now = time(NULL);

    int changed = (current_timestamp && current_timestamp[0] != '\0' &&
                   strcmp(current_timestamp, state->last_seen_timestamp) != 0);

    if (changed) {
        snprintf(state->last_seen_timestamp, sizeof(state->last_seen_timestamp),
                  "%s", current_timestamp);
        state->last_change_epoch = now;
        state->timeout_fired = 0; /* feed recovered — arm for the next stale period */
        return 0;
    }

    int stale_seconds = (int)(now - state->last_change_epoch);
    if (stale_seconds > timeout_seconds && !state->timeout_fired) {
        state->timeout_fired = 1; /* fire once per stale period, not every cycle */
        return 1;
    }

    return 0;
}
