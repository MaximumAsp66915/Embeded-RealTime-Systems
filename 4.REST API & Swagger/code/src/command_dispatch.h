/*
 * command_dispatch.h — Smart Surveillance System, Step 4 (REST API)
 *
 * POST /api/v1/command needs to support new commands being added later
 * "without a rewrite" — this is a plain lookup table of
 * {name, description, dangerous, handler}. Adding a command means adding
 * one array entry in command_dispatch.c; nothing else in the codebase
 * needs to change.
 */

#ifndef COMMAND_DISPATCH_H
#define COMMAND_DISPATCH_H

#include "config.h"
#include <stddef.h>

typedef enum {
    CMD_RESULT_OK = 0,
    CMD_RESULT_UNKNOWN_COMMAND,
    CMD_RESULT_REFUSED_DANGEROUS,
} cmd_result_status_t;

/*
 * Looks up `cmd_name` in the command table and, if found and allowed,
 * runs it. `message_out` receives a short human-readable result string
 * (always null-terminated, even on failure).
 */
cmd_result_status_t command_dispatch_run(const server_config_t *cfg, const char *cmd_name,
                                          char *message_out, size_t message_out_size);

#endif /* COMMAND_DISPATCH_H */
