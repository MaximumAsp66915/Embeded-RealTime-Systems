/*
 * service_ctl.c — see service_ctl.h
 */

#define _POSIX_C_SOURCE 200809L

#include "service_ctl.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* Add a service here (and nowhere else) to make it manageable via the
 * API. Names must exactly match the systemd unit name (without .service). */
const char *const SERVICE_ALLOWLIST[] = {
    "surveillance-web",
    "surveillance-imgproc",
    "surveillance-mqtt",
    "surveillance-gateway",
};
const size_t SERVICE_ALLOWLIST_COUNT = sizeof(SERVICE_ALLOWLIST) / sizeof(SERVICE_ALLOWLIST[0]);

/* The unit that IS this process — restarting it needs special handling,
 * see service_ctl_restart(). */
#define SELF_SERVICE_NAME "surveillance-web"

int service_ctl_is_allowed(const char *name) {
    for (size_t i = 0; i < SERVICE_ALLOWLIST_COUNT; i++) {
        if (strcmp(SERVICE_ALLOWLIST[i], name) == 0) return 1;
    }
    return 0;
}

/*
 * Runs argv[0] with the given arguments (execvp — PATH-searched, but NOT
 * a shell: no string is ever interpreted for metacharacters, so this is
 * safe regardless of what ends up in argv as long as argv itself was
 * built from validated/allowlisted pieces, which every caller in this
 * file does). Captures combined stdout+stderr into `out`. Returns bytes
 * captured, or -1 on a fork/pipe failure. Child's exit code is written
 * to *exit_code_out if non-NULL.
 */
static int run_capture(char *const argv[], char *out, size_t out_size, int *exit_code_out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127); /* only reached if execvp failed */
    }

    /* parent */
    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < out_size - 1 &&
           (n = read(pipefd[0], out + total, out_size - 1 - total)) > 0) {
        total += (size_t)n;
    }
    out[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (exit_code_out) {
        *exit_code_out = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return (int)total;
}

int service_ctl_get_status(const char *name, char *status_out, size_t status_out_size) {
    if (!service_ctl_is_allowed(name)) return -1;

    char *argv[] = {"systemctl", "is-active", (char *)name, NULL};
    char buf[128];
    run_capture(argv, buf, sizeof(buf), NULL);

    /* Trim the trailing newline systemctl always prints. */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    snprintf(status_out, status_out_size, "%s", buf[0] ? buf : "unknown");
    return 0;
}

int service_ctl_get_logs(const char *name, int lines, char *out, size_t out_size) {
    if (!service_ctl_is_allowed(name)) return -1;

    if (lines <= 0) lines = 50;
    if (lines > 500) lines = 500; /* keep responses bounded */

    char lines_str[16];
    snprintf(lines_str, sizeof(lines_str), "%d", lines);

    char *argv[] = {"journalctl", "-u", (char *)name, "-n", lines_str, "--no-pager", NULL};
    return run_capture(argv, out, out_size, NULL);
}

int service_ctl_restart(const char *name, char *message_out, size_t message_out_size) {
    if (!service_ctl_is_allowed(name)) {
        snprintf(message_out, message_out_size, "'%s' is not a manageable service", name);
        return -1;
    }

    if (strcmp(name, SELF_SERVICE_NAME) == 0) {
        /* Restarting our own unit would SIGTERM this very process before
         * the HTTP response could be flushed to the client. Instead,
         * spawn a short-delayed, fully detached restart (double-fork so
         * the delayed process isn't reaped/orphaned oddly when this
         * request's connection thread finishes) and return immediately
         * so the response actually reaches the caller first. */
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            pid_t pid2 = fork();
            if (pid2 == 0) {
                sleep(1);
                execlp("systemctl", "systemctl", "restart", name, (char *)NULL);
                _exit(127);
            }
            _exit(0);
        }
        if (pid > 0) {
            waitpid(pid, NULL, 0); /* reap the immediate (fast-exiting) child */
        }
        snprintf(message_out, message_out_size,
                 "restart scheduled in ~1s (this is the web server's own "
                 "process — result not confirmed, since restarting it ends "
                 "this connection)");
        return 0;
    }

    char *argv[] = {"systemctl", "restart", (char *)name, NULL};
    char buf[256];
    int exit_code = -1;
    run_capture(argv, buf, sizeof(buf), &exit_code);

    if (exit_code == 0) {
        snprintf(message_out, message_out_size, "'%s' restarted successfully", name);
        return 0;
    }

    snprintf(message_out, message_out_size,
             "systemctl restart '%s' failed (exit %d)%s%s",
             name, exit_code, buf[0] ? ": " : "", buf);
    return -1;
}
