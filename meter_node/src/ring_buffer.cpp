#include "ring_buffer.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Locking.
 *
 * On the ESP32 the sampler and the consumers can genuinely run on different
 * cores, so copying a sample out is not atomic and needs a real critical
 * section. The sections here are a struct copy long -- a few hundred ns.
 *
 * Under `pio test -e native` there is no FreeRTOS, so these compile away and
 * the same ring code runs on the PC. That is what lets us unit-test the
 * trickiest logic in the project without any hardware plugged in.
 * -------------------------------------------------------------------------*/
#ifdef ARDUINO
  #include <Arduino.h>
  static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
  #define RING_LOCK()    portENTER_CRITICAL(&s_mux)
  #define RING_UNLOCK()  portEXIT_CRITICAL(&s_mux)
#else
  #define RING_LOCK()    ((void)0)
  #define RING_UNLOCK()  ((void)0)
#endif

/* TELEMETRY_RING_SIZE is a power of two, so the modulo is a mask. */
#define RING_MASK (TELEMETRY_RING_SIZE - 1)
static_assert((TELEMETRY_RING_SIZE & RING_MASK) == 0,
              "TELEMETRY_RING_SIZE must be a power of two");

void ring_init(telemetry_ring_t *r)
{
    memset(r, 0, sizeof(*r));
    r->write_seq = 0;
}

void ring_push(telemetry_ring_t *r, telemetry_sample_t *s)
{
    RING_LOCK();
    uint32_t w = r->write_seq;
    s->seq              = w;
    r->buf[w & RING_MASK] = *s;
    r->write_seq        = w + 1;   /* published last: a reader that sees the
                                    * new count is guaranteed the slot is
                                    * already filled */
    RING_UNLOCK();
}

void ring_reader_attach(telemetry_ring_t *r, telemetry_reader_t *rd,
                        const char *name)
{
    RING_LOCK();
    rd->name     = name;
    rd->next_seq = r->write_seq;   /* start live, not at the backlog */
    rd->overruns = 0;
    RING_UNLOCK();
}

bool ring_read(telemetry_ring_t *r, telemetry_reader_t *rd,
               telemetry_sample_t *out)
{
    bool got = false;

    RING_LOCK();
    uint32_t w = r->write_seq;

    if (rd->next_seq != w) {
        /* Oldest sample still physically present in the buffer. */
        uint32_t oldest = (w > TELEMETRY_RING_SIZE)
                        ? (w - TELEMETRY_RING_SIZE) : 0;

        if (rd->next_seq < oldest) {
            /* Lapped. Account for exactly what was lost, then resync. */
            rd->overruns += (oldest - rd->next_seq);
            rd->next_seq  = oldest;
        }

        *out = r->buf[rd->next_seq & RING_MASK];
        rd->next_seq++;
        got = true;
    }
    RING_UNLOCK();

    return got;
}

uint32_t ring_available(telemetry_ring_t *r, telemetry_reader_t *rd)
{
    RING_LOCK();
    uint32_t n = r->write_seq - rd->next_seq;
    RING_UNLOCK();
    return n;
}
