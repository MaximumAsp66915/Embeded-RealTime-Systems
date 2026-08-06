/*
 * guard_state.h — Smart Surveillance System, Step 6 (Advanced Features)
 *
 * READ-ONLY here — the WRITER is Step 4's REST API
 * (POST /api/v1/guard {"enabled": true/false} and the dashboard page
 * toggle button), which writes this same JSON file. This daemon just
 * polls it each cycle, same IPC pattern as persons.json/frame.jpg
 * throughout this whole project: one small JSON file, atomically
 * written by whoever produces the state, freely read by whoever needs
 * it, no sockets/RPC needed for something this simple and this
 * infrequently changed.
 */

#ifndef GUARD_STATE_H
#define GUARD_STATE_H

#include <stddef.h>

/* Reads {"enabled": bool} from path. Returns 1 if guard mode is
 * currently enabled, 0 if disabled OR the file doesn't exist yet
 * (guard mode defaults to OFF, not an error state — Step 4 hasn't
 * necessarily been toggled on yet, that's normal). */
int guard_state_is_enabled(const char *path);

#endif /* GUARD_STATE_H */
