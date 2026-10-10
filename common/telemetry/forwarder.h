#pragma once
#include <stddef.h>
#include "ring_buffer.h"

/* ---------------------------------------------------------------------------
 * forwarder.h -- store-and-forward across an unreliable uplink.
 *
 * THE PROBLEM. The gateway polls meters at a fixed rate, but its link to the
 * broker is WiFi: it drops. A naive publisher throws away every reading taken
 * while the link was down, which means the energy record has holes in it
 * precisely when something interesting may have happened.
 *
 * THE FIX. Readings accumulate in the ring buffer whether or not the link is
 * up. The forwarder is just a reader that refuses to advance its cursor while
 * disconnected. When the link returns it drains the backlog -- oldest first --
 * and the missing stretch of history arrives late rather than never.
 *
 * This is how an industrial gateway behaves: Schneider's Panel Server uses its
 * logging memory as exactly this kind of buffer, publishing what it held once
 * the cloud connection is restored.
 *
 * WHAT IT CANNOT DO. The ring is finite. An outage longer than the buffer
 * means genuine loss, and the forwarder reports exactly how many readings went
 * missing rather than quietly papering over the gap -- an energy log you
 * cannot trust is worth nothing.
 *
 * Draining is rate-limited by a caller-supplied budget, so catching up after
 * a long outage cannot monopolise the uplink or starve the polling task.
 * -------------------------------------------------------------------------*/

typedef struct {
    telemetry_ring_t   *ring;
    telemetry_reader_t  reader;
    bool                link_up;

    /* Backlog above this many samples means we are catching up, so the
     * samples handed out get tagged TLM_FLAG_BACKFILL. Consumers can then
     * distinguish "this is live" from "this is history arriving late". */
    uint32_t backfill_threshold;

    /* Accounting. published + dropped is every sample the ring ever saw. */
    uint32_t published;
    uint32_t backfilled;
    uint32_t max_backlog;     /* worst backlog observed -- sizes the ring */
    uint32_t outages;
} forwarder_t;

void forwarder_init(forwarder_t *f, telemetry_ring_t *ring,
                    uint32_t backfill_threshold);

/* Tell the forwarder whether the uplink is usable. Going down is what makes
 * it start accumulating; coming back up is what makes it start draining. */
void forwarder_set_link(forwarder_t *f, bool up);

/* Take up to `budget` samples to publish, oldest first.
 *
 * Returns the number written to out[]. Returns 0 while the link is down --
 * the samples stay in the ring, which is the entire point.
 *
 * Samples taken while the backlog exceeds backfill_threshold are tagged
 * TLM_FLAG_BACKFILL. */
size_t forwarder_take(forwarder_t *f, telemetry_sample_t *out, size_t budget);

/* How many samples are waiting. */
uint32_t forwarder_backlog(forwarder_t *f);

/* Samples lost because an outage outlasted the buffer. */
uint32_t forwarder_dropped(const forwarder_t *f);
