/*
 * guard_mode.h — Smart Surveillance System, Step 6 (Advanced Features)
 *
 * WRITE side — this server owns guard_state.json (the Step 5/6 notifier
 * daemon only reads it, see that project's guard_state.c). Toggled via
 * POST /api/v1/guard from either the REST API directly or the dashboard
 * page's toggle button (index_page.c), which just calls the same
 * endpoint via JS fetch — "via API or page" from the spec means both
 * paths end up here, not two separate implementations.
 */

#ifndef GUARD_MODE_H
#define GUARD_MODE_H

#include <stddef.h>

/* Writes {"enabled": true/false, "changed_at": "<ISO8601>"} to path,
 * atomically (write-to-temp + rename, so a concurrent reader never sees
 * a half-written file). Returns 0 on success, -1 on failure. */
int guard_mode_set(const char *path, int enabled);

/* Reads the current state back. Returns 1 if enabled, 0 if disabled OR
 * the file doesn't exist yet (guard mode defaults to OFF). */
int guard_mode_get(const char *path);

#endif /* GUARD_MODE_H */
