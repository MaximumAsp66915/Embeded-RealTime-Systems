/*
 * main.c — Smart Surveillance System, Step 5 (MQTT + Email)
 *
 * A standalone daemon, separate from the Step 3/4 web server, that:
 *   - polls persons.json + CPU temp on a timer
 *   - publishes both to MQTT every cycle (home/persons/<id>,
 *     home/telemetry/<id>), QoS 1
 *   - on seeing count >= 1, sends an alert email — but at most once per
 *     debounce_seconds (see should_send_email() below for the mechanism,
 *     worth citing directly in the report)
 *   - sets up the LWT on connect so the broker notifies subscribers if
 *     this process (and by extension, the board) disappears uncleanly
 *
 * Usage: ./surveillance_notifier notifier.conf
 */

#define _POSIX_C_SOURCE 200809L /* for gmtime_r */

#include "config.h"
#include "mqtt_client.h"
#include "email_notifier.h"
#include "persons_reader.h"
#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/*
 * Debounce mechanism (cite this section in the report):
 *
 * `last_email_epoch` holds the wall-clock time (seconds since epoch) of
 * the last email actually sent, or 0 if none has been sent yet. Every
 * poll cycle where count >= 1, we check whether at least
 * cfg->debounce_seconds have elapsed since that timestamp. If not, the
 * detection is simply NOT emailed this cycle — it is not queued or
 * batched for later, it's dropped for notification purposes (though it's
 * still published to MQTT every cycle regardless of the email debounce,
 * since MQTT isn't rate-limited by this project's spec, only email is).
 * This guarantees "at most 1 email per 30 seconds" by construction: the
 * only way an email fires is this check passing, and passing resets the
 * timestamp immediately (before the email send itself, which can take a
 * moment over the network) so two rapid poll cycles can never both slip
 * through while a send is in flight.
 */
static int should_send_email(time_t now, time_t *last_email_epoch, int debounce_seconds) {
    if (*last_email_epoch != 0 && (now - *last_email_epoch) < debounce_seconds) {
        return 0;
    }
    *last_email_epoch = now; /* reserve this slot before sending, not after */
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <notifier.conf>\n", argv[0]);
        return 1;
    }

    notifier_config_t cfg;
    if (config_load(argv[1], &cfg) != 0) {
        fprintf(stderr, "[main] could not open config file: %s\n", argv[1]);
        return 1;
    }

    fprintf(stdout, "[main] student_id=%s mqtt=%s:%d poll=%dms debounce=%ds\n",
            cfg.student_id, cfg.mqtt_host, cfg.mqtt_port, cfg.poll_interval_ms, cfg.debounce_seconds);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    mqtt_client_t *mqtt = mqtt_client_start(&cfg);
    if (!mqtt) {
        fprintf(stderr, "[main] failed to start MQTT client — continuing without it "
                        "(persons/telemetry publishes will just fail silently below; "
                        "email alerts still work independently)\n");
    } else {
        mqtt_client_announce_online(mqtt);
    }

    time_t last_email_epoch = 0;
    char last_seen_timestamp[64] = "";

    /* mqtt_client_start() already retries internally for a bounded time
     * (~24s) to ride out a typical boot-time network race. This covers
     * the remaining case: the broker/network is down for LONGER than
     * that budget. Rather than leave MQTT permanently disabled for the
     * rest of this process's life, keep trying again periodically —
     * mqtt_client_start()'s own retry loop means each attempt here is
     * naturally spaced out when it fails, so this doesn't need its own
     * elaborate backoff on top. */
    time_t last_mqtt_retry_epoch = (mqtt == NULL) ? time(NULL) : 0;
    const int mqtt_retry_interval_s = 60;

    while (g_running) {
        if (!mqtt && (time(NULL) - last_mqtt_retry_epoch) >= mqtt_retry_interval_s) {
            fprintf(stdout, "[main] retrying MQTT connection...\n");
            mqtt = mqtt_client_start(&cfg);
            if (mqtt) {
                mqtt_client_announce_online(mqtt);
                fprintf(stdout, "[main] MQTT reconnected\n");
            }
            last_mqtt_retry_epoch = time(NULL);
        }

        persons_info_t p;
        int have_persons = (persons_reader_read(cfg.persons_path, &p) == 0);

        double cpu_temp_c = sysinfo_read_cpu_temp_c();

        time_t now = time(NULL);
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        char now_str[32];
        strftime(now_str, sizeof(now_str), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

        if (mqtt) {
            if (have_persons) {
                mqtt_client_publish_persons(mqtt, &cfg, p.count, p.timestamp);
            }
            mqtt_client_publish_telemetry(mqtt, &cfg, cpu_temp_c, now_str);
        }

        /* Only act on a genuinely NEW detection frame (timestamp
         * advanced since last cycle), same reasoning as Step 4's
         * history_log.c — otherwise polling faster than the detector
         * updates would let the debounce check fire on a frame we've
         * already handled. */
        if (have_persons && p.count >= 1 && strcmp(p.timestamp, last_seen_timestamp) != 0) {
            snprintf(last_seen_timestamp, sizeof(last_seen_timestamp), "%s", p.timestamp);

            if (should_send_email(now, &last_email_epoch, cfg.debounce_seconds)) {
                if (email_notifier_send(&cfg, p.count, p.timestamp, cpu_temp_c) != 0) {
                    fprintf(stderr, "[main] email send failed for count=%d timestamp=%s\n",
                            p.count, p.timestamp);
                }
            }
        }

        sleep_ms(cfg.poll_interval_ms);
    }

    fprintf(stdout, "[main] shutting down\n");
    mqtt_client_stop(mqtt);
    return 0;
}
