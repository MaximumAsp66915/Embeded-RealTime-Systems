/*
 * mqtt_client.h — Smart Surveillance System, Step 5 (MQTT + Email)
 *
 * Thin wrapper around libmosquitto (Eclipse Mosquitto's C client library)
 * for the three things this project needs: connect with a Last Will and
 * Testament (LWT) configured BEFORE connecting (so the broker delivers
 * it if this client ever disappears without a clean disconnect), publish
 * JSON at QoS 1, and a graceful shutdown that distinguishes "I'm going
 * offline on purpose" from "I disappeared" by publishing an explicit
 * offline status before disconnecting cleanly.
 *
 * Topics (per the assignment spec):
 *   home/persons/<student_id>    — {"count", "timestamp", "student_id"}
 *   home/telemetry/<student_id>  — {"cpu_temp_c", "timestamp", "student_id"}
 *   home/status/<student_id>     — "online" / "offline" (this project's
 *                                  own addition, carrying the LWT)
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "config.h"

typedef struct mqtt_client mqtt_client_t;

/* Creates the client, sets the LWT (retained "offline" on the status
 * topic), and connects + starts libmosquitto's background network
 * thread. Returns NULL on failure (details on stderr). */
mqtt_client_t *mqtt_client_start(const notifier_config_t *cfg);

/* Publishes the "online" retained status message. Call once after a
 * successful start — separate from mqtt_client_start so main.c can log
 * "connected" only once the broker has actually acknowledged it, if
 * desired (currently called right after start; kept as its own function
 * for clarity/testability). */
void mqtt_client_announce_online(mqtt_client_t *client);

/* QoS 1, not retained — a rolling live value, not a "last known state"
 * that a new subscriber should see immediately on subscribe. */
void mqtt_client_publish_persons(mqtt_client_t *client, const notifier_config_t *cfg,
                                  int count, const char *timestamp);

void mqtt_client_publish_telemetry(mqtt_client_t *client, const notifier_config_t *cfg,
                                    double cpu_temp_c, const char *timestamp);

/* Guard Mode alarm — note the topic SHAPE is deliberately different
 * from persons/telemetry above: the spec calls for exactly
 * "home/<student_id>/alarm" (prefix/id/alarm), not
 * "home/alarm/<student_id>" (prefix/type/id) like the other two. QoS 1,
 * not retained (a rolling alert, not "last known alarm state" — that's
 * what the status topic + persistence in black_box.c's DB are for). */
void mqtt_client_publish_alarm(mqtt_client_t *client, const notifier_config_t *cfg,
                                int count, const char *timestamp);

/* Publishes a retained "offline" status (distinguishing a clean shutdown
 * from the broker later delivering the LWT for an unclean one), then
 * disconnects and frees the client. Safe to call with NULL. */
void mqtt_client_stop(mqtt_client_t *client);

#endif /* MQTT_CLIENT_H */
