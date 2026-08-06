/*
 * watchdog.h — Smart Surveillance System, Step 6 (Advanced Features)
 *
 * Detects a STALE detection feed — person_detector.py's own loop never
 * exits on a bad camera read (see main() in person_detector.py: a
 * failed cap.read() just logs and `continue`s forever), so systemd's
 * Restart=on-failure never fires even if the actual video feed has been
 * dead for minutes. This module is what notices that from the outside,
 * by tracking whether persons.json's timestamp is actually still
 * advancing.
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <time.h>

typedef struct {
    char last_seen_timestamp[64];
    time_t last_change_epoch; /* wall-clock time we last saw the timestamp advance */
    int timeout_fired;        /* have we already alerted for the CURRENT stale period? */
} watchdog_state_t;

void watchdog_init(watchdog_state_t *state);

/*
 * Call every poll cycle with the CURRENT persons.json timestamp (or
 * NULL/empty if it couldn't be read this cycle — treated the same as
 * "no change", not specially). Returns 1 exactly once per stale period
 * (the moment timeout_seconds is first exceeded — not every cycle
 * afterward, so callers don't re-alert every 2s while still stale), 0
 * otherwise. Automatically resets once the feed recovers (a new
 * timestamp is seen), so a second stale period later fires again.
 */
int watchdog_check(watchdog_state_t *state, const char *current_timestamp, int timeout_seconds);

#endif /* WATCHDOG_H */
