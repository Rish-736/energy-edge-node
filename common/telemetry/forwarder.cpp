#include "forwarder.h"
#include <string.h>

void forwarder_init(forwarder_t *f, telemetry_ring_t *ring,
                    uint32_t backfill_threshold)
{
    memset(f, 0, sizeof(*f));
    f->ring               = ring;
    f->backfill_threshold = backfill_threshold;
    f->link_up            = false;   /* assume down until told otherwise:
                                      * optimism here would publish into a
                                      * void and lose the first samples */
    ring_reader_attach(ring, &f->reader, "mqtt");
}

void forwarder_set_link(forwarder_t *f, bool up)
{
    if (f->link_up && !up) f->outages++;
    f->link_up = up;

    /* Note what is NOT here: nothing touches the reader cursor. The backlog
     * surviving a reconnect is the whole mechanism -- resetting the cursor on
     * either transition would throw away exactly the data we buffered. */
}

uint32_t forwarder_backlog(forwarder_t *f)
{
    return ring_available(f->ring, &f->reader);
}

uint32_t forwarder_dropped(const forwarder_t *f)
{
    /* The ring counts this for us: a reader that gets lapped records how many
     * samples passed it by. */
    return f->reader.overruns;
}

size_t forwarder_take(forwarder_t *f, telemetry_sample_t *out, size_t budget)
{
    if (!f->link_up || budget == 0) return 0;

    uint32_t backlog = forwarder_backlog(f);
    if (backlog > f->max_backlog) f->max_backlog = backlog;

    /* Decide once, before draining: if we are behind, everything in this
     * batch is history arriving late. Re-evaluating per sample would flip the
     * flag mid-batch as the backlog shrinks past the threshold. */
    bool catching_up = (backlog > f->backfill_threshold);

    size_t n = 0;
    while (n < budget && ring_read(f->ring, &f->reader, &out[n])) {
        if (catching_up) {
            out[n].flags |= TLM_FLAG_BACKFILL;
            f->backfilled++;
        }
        n++;
    }

    f->published += (uint32_t)n;
    return n;
}
