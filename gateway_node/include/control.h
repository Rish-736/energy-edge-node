#pragma once
#include <stdint.h>
#include "ring_buffer.h"

/* The controller does not own the device table -- main.cpp does. It is handed
 * a function that maps a device index to that device's ring, so it can attach
 * its own reader without the two files sharing a struct definition. */
typedef telemetry_ring_t *(*device_ring_accessor_t)(int index);

typedef struct {
    float    target_w;
    float    window_avg_w;
    float    peak_avg_w;
    uint8_t  mask;
    uint32_t sheds, restores, forced, refusals;
    uint32_t deadline_misses;
    float    charger_slack_s;
    float    charger_delivered_wh;
    float    charger_required_wh;
} control_state_t;

void control_task_start(device_ring_accessor_t accessor, int device_count);

/* Declare a charging job: deliver `wh` by `deadline_ms` (millis() clock). */
void control_set_charger_job(float wh, uint32_t deadline_ms);

void control_get_state(control_state_t *out);
