/*
 * config.h — Smart Surveillance System, Step 3 (Web Server)
 *
 * Loads server.conf (simple "key=value" text file) into a struct so the
 * rest of the program never touches the filesystem for settings again.
 */

#ifndef CONFIG_H
#define CONFIG_H

#define CFG_MAXLEN 256

typedef struct {
    char student_name[CFG_MAXLEN];
    char student_id[CFG_MAXLEN];

    int http_port;
    int https_port;

    char cert_file[CFG_MAXLEN];
    char key_file[CFG_MAXLEN];

    char shared_dir[CFG_MAXLEN];
    char frame_filename[CFG_MAXLEN];
    char persons_filename[CFG_MAXLEN];

    int mjpeg_frame_interval_ms;
    int stats_poll_interval_ms;

    /* --- Step 4: REST API --- */
    /* Safety switch for /api/v1/command entries marked "dangerous"
     * (reboot, shutdown, ...). Defaults to 0 (refuse) so the API can't
     * accidentally reboot the board mid-grading/testing — flip this on
     * deliberately in server.conf when you actually want to test it. */
    int allow_dangerous_commands;
    /* How often the /api/v1/history background sampler polls persons.json
     * for a new record, in milliseconds. */
    int history_poll_interval_ms;

    /* Convenience fields, built once at load time from the pieces above */
    char frame_path[CFG_MAXLEN * 2];
    char persons_path[CFG_MAXLEN * 2];
} server_config_t;

/*
 * Loads `path` into `out`. Returns 0 on success, -1 on failure (file
 * missing, or a required field never got set). Unset optional fields keep
 * sane defaults so a slightly incomplete conf file doesn't crash the server.
 */
int config_load(const char *path, server_config_t *out);

#endif /* CONFIG_H */
