/*
 * main.c — Smart Surveillance System, Step 5/6 (MQTT + Email, Advanced Features)
 *
 * A standalone daemon, separate from the Step 3/4 web server, that:
 *   - polls persons.json + CPU temp on a timer
 *   - publishes both to MQTT every cycle (home/persons/<id>,
 *     home/telemetry/<id>), QoS 1
 *   - sets up the LWT on connect so the broker notifies subscribers if
 *     this process (and by extension, the board) disappears uncleanly
 *
 * Step 6 adds four more triggers feeding into the SAME single email
 * path and its ONE debounce clock (should_send_email() below) — see
 * that function's own comment for why this matters:
 *   - Guard Mode: when armed (Step 4's REST API writes guard_state.json,
 *     this daemon only reads it) and a detection occurs, publishes
 *     immediately to home/<student_id>/alarm (QoS 1, NOT debounced —
 *     only email is rate-limited, MQTT alarms aren't) and sends a
 *     GUARD-flavored email.
 *   - Black box: every detection/alarm/watchdog/thermal event gets
 *     logged to a SQLite circular buffer (black_box.c) — logging itself
 *     is NEVER debounced, only the email is; the black box is meant to
 *     be a complete record even during an email-suppressed burst.
 *   - Watchdog: if persons.json's timestamp hasn't advanced in
 *     watchdog_timeout_s, logs + emails + restarts the configured
 *     systemd unit (person_detector.py's own loop never exits on a bad
 *     camera read, so nothing else would ever notice or recover this).
 *   - Adaptive thermal: CPU temp crossing thermal_trigger_c writes a
 *     throttle signal (control.json) that person_detector.py polls each
 *     frame to reduce its own FPS/processing scale, plus an alert email.
 *
 * Usage: ./surveillance_notifier notifier.conf
 */

#define _POSIX_C_SOURCE 200809L /* for gmtime_r */

#include "config.h"
#include "mqtt_client.h"
#include "email_notifier.h"
#include "persons_reader.h"
#include "sysinfo.h"
#include "guard_state.h"
#include "black_box.h"
#include "watchdog.h"
#include "thermal.h"
#include "service_ctl.h"

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
 * time ANY of this daemon's four triggers (detection, guard alarm,
 * watchdog, thermal) wants to send an email, it goes through THIS one
 * function first. If fewer than cfg->debounce_seconds have elapsed
 * since the last send, the email is simply NOT sent this cycle — not
 * queued or batched for later, just dropped for notification purposes
 * (though whatever triggered it is still logged to the black box and,
 * for detections, still published to MQTT, regardless of the email
 * debounce — only email is rate-limited by spec). Sharing this single
 * timer/function across all four triggers is what guarantees "at most 1
 * email per 30 seconds" as a SYSTEM-WIDE property rather than a
 * per-trigger one — four independent debounce timers could together
 * still send up to 4 emails in the same 30s window, which would violate
 * the actual requirement. The timestamp is updated BEFORE the email
 * send itself (which can take a moment over the network), so two
 * triggers landing close together can never both slip through while a
 * send is in flight — the slot is reserved first, sent second.
 */
static int should_send_email(time_t now, time_t *last_email_epoch, int debounce_seconds) {
    if (*last_email_epoch != 0 && (now - *last_email_epoch) < debounce_seconds) {
        return 0;
    }
    *last_email_epoch = now; /* reserve this slot before sending, not after */
    return 1;
}

static void maybe_send_email(const notifier_config_t *cfg, time_t now, time_t *last_email_epoch,
                              const char *reason, int count, const char *timestamp, double cpu_temp_c) {
    if (!should_send_email(now, last_email_epoch, cfg->debounce_seconds)) {
        return;
    }
    if (email_notifier_send(cfg, reason, count, timestamp, cpu_temp_c) != 0) {
        fprintf(stderr, "[main] email send failed (%s) count=%d timestamp=%s\n",
                reason, count, timestamp);
    }
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
    fprintf(stdout, "[main] watchdog_timeout=%ds watchdog_service=%s\n",
            cfg.watchdog_timeout_s, cfg.watchdog_service_name);
    fprintf(stdout, "[main] thermal trigger=%.1fC recover=%.1fC\n",
            cfg.thermal_trigger_c, cfg.thermal_recover_c);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    mqtt_client_t *mqtt = mqtt_client_start(&cfg);
    if (!mqtt) {
        fprintf(stderr, "[main] failed to start MQTT client — continuing without it "
                        "(persons/telemetry/alarm publishes will just fail silently below; "
                        "email alerts still work independently)\n");
    } else {
        mqtt_client_announce_online(mqtt);
    }

    black_box_t *bb = black_box_open(cfg.blackbox_db_path, cfg.blackbox_max_events);
    if (!bb) {
        fprintf(stderr, "[main] failed to open black box DB at %s — event logging disabled "
                        "for this run (everything else still works)\n", cfg.blackbox_db_path);
    }

    watchdog_state_t wd_state;
    watchdog_init(&wd_state);

    thermal_state_t thermal_state;
    thermal_init(&thermal_state);
    thermal_config_t thermal_cfg = {
        .trigger_threshold_c = cfg.thermal_trigger_c,
        .recover_threshold_c = cfg.thermal_recover_c,
        .normal_fps = cfg.thermal_normal_fps,
        .normal_scale = cfg.thermal_normal_scale,
        .throttled_fps = cfg.thermal_throttled_fps,
        .throttled_scale = cfg.thermal_throttled_scale,
    };

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

        /* --- Watchdog --- */
        int wd_fired = watchdog_check(&wd_state, have_persons ? p.timestamp : NULL,
                                       cfg.watchdog_timeout_s);
        if (wd_fired) {
            fprintf(stderr, "[watchdog] no new frame for >%ds — alerting and restarting %s\n",
                    cfg.watchdog_timeout_s, cfg.watchdog_service_name);

            if (bb) {
                char detail[128];
                snprintf(detail, sizeof(detail), "no new frame for over %ds", cfg.watchdog_timeout_s);
                black_box_log_event(bb, BB_EVENT_WATCHDOG_TIMEOUT,
                                     have_persons ? p.count : 0, cpu_temp_c, detail);
            }

            maybe_send_email(&cfg, now, &last_email_epoch,
                              "Watchdog - camera feed stale, restarting service",
                              have_persons ? p.count : 0, now_str, cpu_temp_c);

            char restart_msg[256];
            int restart_rc = service_ctl_restart(cfg.watchdog_service_name, restart_msg, sizeof(restart_msg));
            fprintf(stdout, "[watchdog] restart %s: %s\n",
                    restart_rc == 0 ? "OK" : "FAILED", restart_msg);
        }

        /* --- Adaptive thermal management --- */
        thermal_transition_t tt = thermal_check(&thermal_state, &thermal_cfg, cpu_temp_c,
                                                 cfg.thermal_control_path);
        if (tt == THERMAL_JUST_THROTTLED) {
            fprintf(stdout, "[thermal] %.1fC >= trigger %.1fC — throttling to %.1ffps/%.0f%% scale\n",
                    cpu_temp_c, cfg.thermal_trigger_c, cfg.thermal_throttled_fps,
                    cfg.thermal_throttled_scale * 100);
            if (bb) {
                black_box_log_event(bb, BB_EVENT_THERMAL_THROTTLE,
                                     have_persons ? p.count : 0, cpu_temp_c, "trigger threshold crossed");
            }
            maybe_send_email(&cfg, now, &last_email_epoch,
                              "Thermal - CPU over threshold, throttling FPS/resolution",
                              have_persons ? p.count : 0, now_str, cpu_temp_c);
        } else if (tt == THERMAL_JUST_RECOVERED) {
            fprintf(stdout, "[thermal] %.1fC <= recover %.1fC — back to normal settings\n",
                    cpu_temp_c, cfg.thermal_recover_c);
            if (bb) {
                black_box_log_event(bb, BB_EVENT_THERMAL_RECOVER,
                                     have_persons ? p.count : 0, cpu_temp_c, "recovered below threshold");
            }
            /* No email on recovery — spec ties the alert email to the
             * triggering event, not the all-clear. Still logged to the
             * black box (needed for the 4-4 experiment's before/after
             * evidence) and printed above for live visibility. */
        }

        /* --- Detection handling: plain notice, or Guard Mode alarm --- *
         * Only act on a genuinely NEW detection frame (timestamp
         * advanced since last cycle), same reasoning as Step 4's
         * history_log.c — otherwise polling faster than the detector
         * updates would let this fire twice for one real detection. */
        if (have_persons && p.count >= 1 && strcmp(p.timestamp, last_seen_timestamp) != 0) {
            snprintf(last_seen_timestamp, sizeof(last_seen_timestamp), "%s", p.timestamp);

            int guard_on = guard_state_is_enabled(cfg.guard_state_path);

            if (guard_on) {
                /* Immediate MQTT alarm — deliberately NOT gated by the
                 * email debounce, since only email is rate-limited by
                 * spec, and "immediate" is the whole point of Guard
                 * Mode's alarm topic. */
                if (mqtt) {
                    mqtt_client_publish_alarm(mqtt, &cfg, p.count, p.timestamp);
                }
                if (bb) {
                    black_box_log_event(bb, BB_EVENT_GUARD_ALARM, p.count, cpu_temp_c,
                                         "guard mode armed");
                }
                maybe_send_email(&cfg, now, &last_email_epoch, "GUARD MODE ALARM - person detected",
                                  p.count, p.timestamp, cpu_temp_c);
            } else {
                if (bb) {
                    black_box_log_event(bb, BB_EVENT_DETECTION, p.count, cpu_temp_c, "person detected");
                }
                maybe_send_email(&cfg, now, &last_email_epoch, "Person detected",
                                  p.count, p.timestamp, cpu_temp_c);
            }
        }

        sleep_ms(cfg.poll_interval_ms);
    }

    fprintf(stdout, "[main] shutting down\n");
    black_box_close(bb);
    mqtt_client_stop(mqtt);
    return 0;
}
