#include "rs485.h"
#ifdef ARDUINO

void rs485_begin(rs485_t *b, HardwareSerial *port, uint32_t baud,
                 int rx_pin, int tx_pin, int de_pin)
{
    b->port         = port;
    b->de_pin       = de_pin;
    b->len          = 0;
    b->last_byte_us = 0;
    b->frames_rx    = 0;
    b->frames_tx    = 0;
    b->overflows    = 0;

    /* 3.5 character times, a character being 11 bits on the wire. Above
     * 19200 baud the spec stops scaling it and fixes the gap at 1.75 ms,
     * because the arithmetic value gets too small to measure reliably. */
    if (baud > 19200) {
        b->t35_us = 1750;
    } else {
        b->t35_us = (uint32_t)((11UL * 1000000UL * 7UL) / (baud * 2UL));
    }

    pinMode(de_pin, OUTPUT);
    digitalWrite(de_pin, LOW);          /* listen by default -- never idle
                                         * holding the bus */
    port->begin(baud, SERIAL_8N1, rx_pin, tx_pin);
}

void rs485_flush_input(rs485_t *b)
{
    while (b->port->available()) b->port->read();
    b->len = 0;
}

size_t rs485_poll_frame(rs485_t *b)
{
    while (b->port->available()) {
        int c = b->port->read();
        if (c < 0) break;

        if (b->len < sizeof(b->buf)) {
            b->buf[b->len++] = (uint8_t)c;
        } else {
            b->overflows++;             /* frame longer than legal: it will
                                         * fail CRC and be dropped */
        }
        b->last_byte_us = micros();
    }

    if (b->len == 0) return 0;

    /* Silence long enough? Then the frame is complete. */
    if ((uint32_t)(micros() - b->last_byte_us) < b->t35_us) return 0;

    size_t n = b->len;
    b->len   = 0;
    b->frames_rx++;
    return n;
}

size_t rs485_recv_frame(rs485_t *b, uint32_t timeout_ms)
{
    uint32_t start = millis();

    for (;;) {
        size_t n = rs485_poll_frame(b);
        if (n) return n;

        if ((uint32_t)(millis() - start) >= timeout_ms) {
            b->len = 0;                 /* discard a partial frame */
            return 0;
        }
        /* Yield: on an ESP32 a tight poll loop starves other tasks and the
         * watchdog eventually complains. */
        delayMicroseconds(200);
    }
}

void rs485_send(rs485_t *b, const uint8_t *frame, size_t len)
{
    digitalWrite(b->de_pin, HIGH);      /* take the bus */

    b->port->write(frame, len);

    /* flush() blocks until the UART has physically shifted out the last stop
     * bit. Dropping DE before that truncates the end of the frame -- which
     * shows up as a CRC error at the far end and is one of the most common
     * RS-485 bring-up faults. */
    b->port->flush();

    digitalWrite(b->de_pin, LOW);       /* release it immediately */
    b->frames_tx++;
}

#endif /* ARDUINO */
