/*
 * command_dispatch.c — see command_dispatch.h
 */

#define _DEFAULT_SOURCE

#include "command_dispatch.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

typedef struct {
    const char *name;
    const char *description;
    int dangerous; /* if 1, requires cfg->allow_dangerous_commands = true */
    void (*handler)(char *message_out, size_t message_out_size);
} command_entry_t;

/* --- Handlers --- */

static void cmd_noop(char *message_out, size_t message_out_size) {
    snprintf(message_out, message_out_size,
             "noop executed — dispatch pipeline is working, no side effects");
}

static void cmd_reboot(char *message_out, size_t message_out_size) {
    snprintf(message_out, message_out_size, "rebooting now");
    sync();
    reboot(RB_AUTOBOOT); /* requires root (CAP_SYS_BOOT); unreachable on success */
}

static void cmd_shutdown(char *message_out, size_t message_out_size) {
    snprintf(message_out, message_out_size, "shutting down now");
    sync();
    reboot(RB_POWER_OFF); /* requires root (CAP_SYS_BOOT); unreachable on success */
}

/* --- Table: add new commands here, nowhere else --- */

static const command_entry_t COMMANDS[] = {
    {"noop",     "Harmless test command — verifies the /command pipeline end-to-end", 0, cmd_noop},
    {"reboot",   "Reboots the board immediately",                                     1, cmd_reboot},
    {"shutdown", "Powers off the board immediately",                                  1, cmd_shutdown},
};
#define NUM_COMMANDS (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

cmd_result_status_t command_dispatch_run(const server_config_t *cfg, const char *cmd_name,
                                          char *message_out, size_t message_out_size) {
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(COMMANDS[i].name, cmd_name) == 0) {
            if (COMMANDS[i].dangerous && !cfg->allow_dangerous_commands) {
                snprintf(message_out, message_out_size,
                         "command '%s' is marked dangerous and allow_dangerous_commands "
                         "is not enabled in server.conf", cmd_name);
                return CMD_RESULT_REFUSED_DANGEROUS;
            }
            COMMANDS[i].handler(message_out, message_out_size);
            return CMD_RESULT_OK;
        }
    }
    snprintf(message_out, message_out_size, "unknown command '%s'", cmd_name);
    return CMD_RESULT_UNKNOWN_COMMAND;
}
