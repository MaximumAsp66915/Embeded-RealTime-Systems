/*
 * mqtt_client.c — see mqtt_client.h
 */

#define _POSIX_C_SOURCE 200809L /* for nanosleep */

#include "mqtt_client.h"

#include <mosquitto.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#define STATUS_ONLINE "online"
#define STATUS_OFFLINE "offline"

/* Bounds on the startup retry loop below — chosen to comfortably ride
 * out a typical "just booted, DHCP/network still coming up" race (a few
 * seconds, sometimes 10-20s on a slow board) without hanging the whole
 * process indefinitely if the broker is genuinely down/unreachable for
 * longer than that. */
#define CONNECT_MAX_ATTEMPTS 8
#define CONNECT_RETRY_DELAY_S 3

struct mqtt_client {
    struct mosquitto *mosq;
    char persons_topic[CFG_MAXLEN * 2 + 32];
    char telemetry_topic[CFG_MAXLEN * 2 + 32];
    char status_topic[CFG_MAXLEN * 2 + 32];
};

static void on_connect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq;
    (void)userdata;
    if (rc == 0) {
        fprintf(stdout, "[mqtt] connected to broker\n");
    } else {
        fprintf(stderr, "[mqtt] connect failed: %s\n", mosquitto_connack_string(rc));
    }
}

static void on_disconnect(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq;
    (void)userdata;
    if (rc != 0) {
        fprintf(stderr, "[mqtt] unexpectedly disconnected (rc=%d) — broker will "
                        "deliver our LWT to subscribers\n", rc);
    } else {
        fprintf(stdout, "[mqtt] disconnected cleanly\n");
    }
}

mqtt_client_t *mqtt_client_start(const notifier_config_t *cfg) {
    mosquitto_lib_init();

    mqtt_client_t *client = calloc(1, sizeof(mqtt_client_t));
    if (!client) return NULL;

    snprintf(client->persons_topic, sizeof(client->persons_topic),
             "%s/persons/%s", cfg->mqtt_topic_prefix, cfg->student_id);
    snprintf(client->telemetry_topic, sizeof(client->telemetry_topic),
             "%s/telemetry/%s", cfg->mqtt_topic_prefix, cfg->student_id);
    snprintf(client->status_topic, sizeof(client->status_topic),
             "%s/status/%s", cfg->mqtt_topic_prefix, cfg->student_id);

    client->mosq = mosquitto_new(cfg->mqtt_client_id, true /* clean session */, NULL);
    if (!client->mosq) {
        fprintf(stderr, "[mqtt] mosquitto_new failed\n");
        free(client);
        return NULL;
    }

    mosquitto_connect_callback_set(client->mosq, on_connect);
    mosquitto_disconnect_callback_set(client->mosq, on_disconnect);

    /* LWT MUST be set before connecting — this is the whole mechanism:
     * if this process dies, loses network, or its TCP connection times
     * out (keepalive), the BROKER (not us) publishes this message on our
     * behalf, retained, so anything subscribed to the status topic sees
     * "offline" even though we never got to say so ourselves. QoS 1 per
     * spec — "at least once", matching the persons/telemetry publishes. */
    int will_rc = mosquitto_will_set(client->mosq, client->status_topic,
                                      (int)strlen(STATUS_OFFLINE), STATUS_OFFLINE,
                                      1 /* QoS 1 */, true /* retained */);
    if (will_rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqtt] mosquitto_will_set failed: %s\n", mosquitto_strerror(will_rc));
    }

    /* Auth — required for a broker set up like secure_setup.sh's (Step
     * 1), which sets allow_anonymous false. Must be set before connect,
     * same as the LWT above. Left as a no-op (anonymous connect) if the
     * config has no username, so this still works against an
     * unauthenticated broker without any special-casing. */
    if (cfg->mqtt_username[0] != '\0') {
        int auth_rc = mosquitto_username_pw_set(client->mosq, cfg->mqtt_username, cfg->mqtt_password);
        if (auth_rc != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "[mqtt] mosquitto_username_pw_set failed: %s\n", mosquitto_strerror(auth_rc));
        }
    }

    /* Retry the INITIAL connect a few times before giving up — this is
     * specifically for the "just booted, network/DHCP not fully up yet"
     * race: After=network-online.target frequently doesn't actually wait
     * for a real route on minimal Armbian/Debian images unless a
     * wait-online service is properly enabled, so the very first
     * connect() attempt can hit ENETUNREACH a few seconds before the
     * network genuinely comes up. Once actually connected,
     * mosquitto_loop_start()'s background thread handles ongoing
     * reconnection automatically (that's a separate, already-working
     * mechanism) — this loop only covers the one-shot initial attempt,
     * which is otherwise a single point of permanent failure: without
     * this, one bad first attempt disabled MQTT for the process's entire
     * lifetime, even if the network came up moments later. */
    int rc = MOSQ_ERR_SUCCESS;
    int attempt;
    for (attempt = 1; attempt <= CONNECT_MAX_ATTEMPTS; attempt++) {
        rc = mosquitto_connect(client->mosq, cfg->mqtt_host, cfg->mqtt_port, cfg->mqtt_keepalive_s);
        if (rc == MOSQ_ERR_SUCCESS) break;

        fprintf(stderr, "[mqtt] mosquitto_connect(%s:%d) attempt %d/%d failed: %s\n",
                cfg->mqtt_host, cfg->mqtt_port, attempt, CONNECT_MAX_ATTEMPTS, mosquitto_strerror(rc));

        if (attempt < CONNECT_MAX_ATTEMPTS) {
            struct timespec ts = {CONNECT_RETRY_DELAY_S, 0};
            nanosleep(&ts, NULL);
        }
    }

    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqtt] giving up after %d attempts — caller can retry "
                        "mqtt_client_start() again later\n", CONNECT_MAX_ATTEMPTS);
        mosquitto_destroy(client->mosq);
        free(client);
        return NULL;
    }

    /* Background thread: handles the network loop (keepalive pings,
     * reconnect attempts, incoming callbacks) so main.c's poll loop
     * doesn't need to pump mosquitto_loop() itself. */
    rc = mosquitto_loop_start(client->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqtt] mosquitto_loop_start failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(client->mosq);
        free(client);
        return NULL;
    }

    return client;
}

void mqtt_client_announce_online(mqtt_client_t *client) {
    if (!client) return;
    mosquitto_publish(client->mosq, NULL, client->status_topic,
                       (int)strlen(STATUS_ONLINE), STATUS_ONLINE,
                       1 /* QoS 1 */, true /* retained — new subscribers see current state */);
}

void mqtt_client_publish_persons(mqtt_client_t *client, const notifier_config_t *cfg,
                                  int count, const char *timestamp) {
    if (!client) return;

    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "{\"count\": %d, \"timestamp\": \"%s\", \"student_id\": \"%s\"}",
        count, timestamp, cfg->student_id);

    int rc = mosquitto_publish(client->mosq, NULL, client->persons_topic,
                                len, payload, 1 /* QoS 1 */, false /* not retained */);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqtt] publish to %s failed: %s\n",
                client->persons_topic, mosquitto_strerror(rc));
    }
}

void mqtt_client_publish_telemetry(mqtt_client_t *client, const notifier_config_t *cfg,
                                    double cpu_temp_c, const char *timestamp) {
    if (!client) return;

    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "{\"cpu_temp_c\": %.1f, \"timestamp\": \"%s\", \"student_id\": \"%s\"}",
        cpu_temp_c, timestamp, cfg->student_id);

    int rc = mosquitto_publish(client->mosq, NULL, client->telemetry_topic,
                                len, payload, 1 /* QoS 1 */, false /* not retained */);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[mqtt] publish to %s failed: %s\n",
                client->telemetry_topic, mosquitto_strerror(rc));
    }
}

void mqtt_client_stop(mqtt_client_t *client) {
    if (!client) return;

    /* Distinguish "shutting down on purpose" from the LWT's "vanished
     * unexpectedly" — publish offline ourselves, give it a moment to
     * actually go out over the still-open connection, then disconnect
     * cleanly (which does NOT trigger our own LWT, since a clean
     * disconnect is exactly what the LWT mechanism is designed to
     * distinguish from). */
    mosquitto_publish(client->mosq, NULL, client->status_topic,
                       (int)strlen(STATUS_OFFLINE), STATUS_OFFLINE,
                       1, true);

    /* Best-effort flush: mosquitto_loop_start's background thread needs
     * a brief moment to actually push the queued publish out before we
     * tear the connection down underneath it. */
    struct timespec ts = {0, 200 * 1000 * 1000}; /* 200ms */
    nanosleep(&ts, NULL);

    mosquitto_disconnect(client->mosq);
    mosquitto_loop_stop(client->mosq, false);
    mosquitto_destroy(client->mosq);
    mosquitto_lib_cleanup();
    free(client);
}
