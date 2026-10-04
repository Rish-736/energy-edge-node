#pragma once
#ifdef ARDUINO

#include <Arduino.h>
#include <HardwareSerial.h>
#include "modbus_rtu.h"

/* ---------------------------------------------------------------------------
 * rs485.h -- the physical layer under Modbus-RTU.
 *
 * RS-485 is a two-wire, half-duplex bus: one pair carries traffic in both
 * directions, so exactly one device may drive the line at a time. A MAX485
 * transceiver has a DE (driver enable) pin for that -- high to talk, low to
 * listen. Leave DE high and you jam the entire bus.
 *
 * FRAMING. Modbus-RTU has no start or end byte. A frame is delimited purely
 * by SILENCE: 3.5 character times of idle line means "message over". So the
 * receiver is a timer, not a parser. The spec fixes this gap at 1.75 ms for
 * baud rates above 19200, and defines it as 3.5 actual character times below
 * that (a character being 11 bits: start + 8 data + parity/stop + stop).
 * -------------------------------------------------------------------------*/

typedef struct {
    HardwareSerial *port;
    int      de_pin;
    uint32_t t35_us;              /* inter-frame silence threshold */

    uint8_t  buf[MODBUS_MAX_FRAME];
    size_t   len;
    uint32_t last_byte_us;

    /* Diagnostics worth having on a real bus. */
    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t overflows;           /* bytes arrived with the buffer full */
} rs485_t;

void rs485_begin(rs485_t *b, HardwareSerial *port, uint32_t baud,
                 int rx_pin, int tx_pin, int de_pin);

/* Non-blocking: feed it from a loop. Returns the frame length once 3.5
 * character times of silence have followed the last byte, else 0. The frame
 * is in b->buf. Used by the slave, which must never block waiting. */
size_t rs485_poll_frame(rs485_t *b);

/* Blocking: wait up to timeout_ms for one complete frame. Returns its length,
 * or 0 on timeout. Used by the master, which has just asked a question and is
 * entitled to wait a bounded time for the answer. */
size_t rs485_recv_frame(rs485_t *b, uint32_t timeout_ms);

/* Drive DE high, send, wait for the last bit to physically leave, drop DE. */
void rs485_send(rs485_t *b, const uint8_t *frame, size_t len);

/* Throw away anything sitting in the receive path. Call before a request so a
 * late reply to the PREVIOUS question cannot be mistaken for this one's. */
void rs485_flush_input(rs485_t *b);

#endif /* ARDUINO */
