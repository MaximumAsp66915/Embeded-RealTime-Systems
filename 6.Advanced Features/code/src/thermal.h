/*
 * thermal.h — Smart Surveillance System, Step 6 (Advanced Features)
 *
 * Monitors CPU temp (via sysinfo_read_cpu_temp_c(), same direct
 * /sys/class/thermal read used everywhere else in this project — no
 * shelling out) and, when it crosses a threshold, writes a throttle
 * signal file that person_detector.py polls each frame to reduce its
 * own FPS and processing resolution scale. ALL the thermal decision-
 * making (when to throttle, by how much, when to recover) happens
 * here, in C — person_detector.py just mechanically applies whatever
 * numbers this file tells it to use, no thermal logic of its own.
 *
 * Hysteresis: recover_threshold_c is deliberately LOWER than
 * trigger_threshold_c (e.g. trigger at 70C, recover at 65C) — without
 * that gap, a temperature sitting right at the boundary would flip
 * throttled/normal every single poll cycle. The gap means it has to
 * cool down meaningfully before normal settings resume, not just dip
 * 0.1C below the trigger point.
 */

#ifndef THERMAL_H
#define THERMAL_H

#include <stddef.h>

typedef struct {
    int throttled; /* current state, so callers can detect the transition edges */
    int initialized; /* has thermal_check written the control file at least once yet? */
} thermal_state_t;

typedef struct {
    double trigger_threshold_c;
    double recover_threshold_c;
    double normal_fps;
    double normal_scale;
    double throttled_fps;
    double throttled_scale;
} thermal_config_t;

typedef enum {
    THERMAL_NO_CHANGE,
    THERMAL_JUST_THROTTLED,  /* crossed trigger_threshold_c this cycle */
    THERMAL_JUST_RECOVERED,  /* dropped below recover_threshold_c this cycle */
} thermal_transition_t;

void thermal_init(thermal_state_t *state);

/* Call every poll cycle with the current CPU temp. Writes control_path
 * (a small JSON file: {"target_fps": ..., "processing_scale": ...,
 * "throttled": bool}) whenever the throttle state changes OR on the
 * very first call (so the file always reflects reality even if this is
 * the first cycle after startup). Returns which transition (if any)
 * happened this cycle, so the caller can decide whether to log/email
 * about it. */
thermal_transition_t thermal_check(thermal_state_t *state, const thermal_config_t *cfg,
                                    double current_temp_c, const char *control_path);

#endif /* THERMAL_H */
