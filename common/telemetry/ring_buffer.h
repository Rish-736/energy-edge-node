#pragma once
#include <stdbool.h>
#include "telemetry.h"

/* ---------------------------------------------------------------------------
 * ring_buffer.h -- single-writer, multi-reader telemetry history.
 *
 * WHY THIS EXISTS
 *   One sampling task writes. Several consumers read, each at its own pace and
 *   on its own schedule. A slow consumer (MQTT blocked on WiFi) must never
 *   stall a fast one (NILM), and must never stall the sampler.
 *
 * HOW IT WORKS
 *   The writer owns a monotonic counter `write_seq` -- the total number of
 *   samples ever written -- and drops each sample at index (seq % SIZE),
 *   overwriting whatever is oldest. It never blocks and never fails.
 *
 *   Each reader holds its OWN cursor (`next_seq`). Readers are completely
 *   independent: registering a new one is a local change that the writer and
 *   the other readers never see.
 *
 *   If a reader falls more than SIZE samples behind, the writer has lapped it.
 *   We do not pretend otherwise: the read fast-forwards to the oldest sample
 *   still present and counts the loss in `overruns`. Silent data loss is the
 *   bug that makes energy logs untrustworthy, so we surface it instead.
 * -------------------------------------------------------------------------*/

/* Overridable per node via build_flags: the meter needs ~25 s of history for
 * its consumers, while the gateway sizes this for how long an MQTT outage it
 * should survive without losing readings. Must be a power of two. */
#ifndef TELEMETRY_RING_SIZE
#define TELEMETRY_RING_SIZE 256
#endif

typedef struct {
    telemetry_sample_t buf[TELEMETRY_RING_SIZE];
    volatile uint32_t  write_seq;   /* total samples ever written */
} telemetry_ring_t;

typedef struct {
    const char *name;        /* for diagnostics: "logger", "modbus", ... */
    uint32_t    next_seq;    /* the next sample this reader wants        */
    uint32_t    overruns;    /* samples lost to being lapped             */
} telemetry_reader_t;

void ring_init(telemetry_ring_t *r);

/* Writer side. Never blocks, never fails. Stamps the sample's seq field. */
void ring_push(telemetry_ring_t *r, telemetry_sample_t *s);

/* Attach a reader. Starts at the newest sample -- a consumer that comes up
 * late wants live data, not a backlog it will never catch up on. */
void ring_reader_attach(telemetry_ring_t *r, telemetry_reader_t *rd,
                        const char *name);

/* Pop the next sample for this reader into *out.
 * Returns true if a sample was produced, false if the reader is caught up. */
bool ring_read(telemetry_ring_t *r, telemetry_reader_t *rd,
               telemetry_sample_t *out);

/* How many samples are waiting for this reader. */
uint32_t ring_available(telemetry_ring_t *r, telemetry_reader_t *rd);
