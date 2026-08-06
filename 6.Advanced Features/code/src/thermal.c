/*
 * thermal.c — see thermal.h
 */

#include "thermal.h"

#include <stdio.h>

static void write_control_file(const char *control_path, double target_fps,
                                double processing_scale, int throttled) {
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", control_path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        fprintf(stderr, "[thermal] could not write %s\n", tmp_path);
        return;
    }
    fprintf(f,
        "{\"target_fps\": %.1f, \"processing_scale\": %.2f, \"throttled\": %s}",
        target_fps, processing_scale, throttled ? "true" : "false");
    fclose(f);

    /* Atomic rename — same reasoning as person_detector.py's own
     * SharedOutput._atomic_write_bytes: the reader (person_detector.py's
     * control-file poll) should never see a half-written file. */
    if (rename(tmp_path, control_path) != 0) {
        fprintf(stderr, "[thermal] could not rename %s -> %s\n", tmp_path, control_path);
    }
}

void thermal_init(thermal_state_t *state) {
    state->throttled = 0;
    state->initialized = 0;
}

thermal_transition_t thermal_check(thermal_state_t *state, const thermal_config_t *cfg,
                                    double current_temp_c, const char *control_path) {
    thermal_transition_t transition = THERMAL_NO_CHANGE;

    if (!state->throttled && current_temp_c >= cfg->trigger_threshold_c) {
        state->throttled = 1;
        transition = THERMAL_JUST_THROTTLED;
    } else if (state->throttled && current_temp_c <= cfg->recover_threshold_c) {
        state->throttled = 0;
        transition = THERMAL_JUST_RECOVERED;
    }

    if (transition != THERMAL_NO_CHANGE || !state->initialized) {
        double fps = state->throttled ? cfg->throttled_fps : cfg->normal_fps;
        double scale = state->throttled ? cfg->throttled_scale : cfg->normal_scale;
        write_control_file(control_path, fps, scale, state->throttled);
        state->initialized = 1;
    }

    return transition;
}
