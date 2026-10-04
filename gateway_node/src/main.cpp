#include <Arduino.h>
#include "config.h"
#include "modbus_rtu.h"
#include "meter_regs.h"
#include "rs485.h"

/* ---------------------------------------------------------------------------
 * gateway_node -- Phase 1: Modbus-RTU master.
 *
 * This is the "Edge Control" floor. Its job is to ask the meters for their
 * numbers, on a schedule, and to be honest about what it got back.
 *
 * Two things here are easy to skip and shouldn't be:
 *
 *   DISCOVERY. Rather than hard-coding "there is a meter at unit 1", the
 *   gateway sweeps the bus at startup and asks every address "who are you?".
 *   Whoever answers with our vendor ID gets added. This is how real
 *   commissioning tools work, and it means adding a second meter requires no
 *   code change at all.
 *
 *   MEASURING THE LINK. Every transaction is timed and every failure is
 *   classified -- timeout, CRC error, or exception. "The bus works" is not an
 *   engineering claim; "poll round-trip is 11 ms worst case over 10,000
 *   transactions with 0 CRC errors" is.
 * -------------------------------------------------------------------------*/

static rs485_t s_bus;

typedef struct {
    uint8_t  unit;
    bool     present;
    uint16_t fw_version;
    uint16_t capabilities;

    /* Per-device link health. Kept per-device because on a real bus one bad
     * cable run fails while the others are fine, and an aggregate counter
     * would hide exactly that. */
    uint32_t polls_ok;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t exceptions;
    uint32_t latency_us_last;
    uint32_t latency_us_max;
    uint64_t latency_us_sum;

    meter_poll_t last;
    bool         last_valid;
} device_t;

static device_t s_dev[MAX_DEVICES];
static int      s_dev_count = 0;

/* --- one request/response transaction ----------------------------------- */
static mb_result_t txn_read(uint8_t unit, uint8_t fc, uint16_t addr,
                            uint16_t qty, uint16_t *out,
                            uint32_t *latency_us, uint8_t *exception_out)
{
    uint8_t req[MODBUS_MAX_FRAME];
    int n = modbus_master_build_read(unit, fc, addr, qty, req, sizeof(req));
    if (n < 0) return MB_ERR_ARG;

    /* Drop anything already in the receive path. Without this, a reply that
     * arrived late from the PREVIOUS question gets parsed as the answer to
     * this one -- which produces plausible-looking but wrong data, the worst
     * possible failure mode. */
    rs485_flush_input(&s_bus);

    uint32_t t0 = micros();
    rs485_send(&s_bus, req, (size_t)n);

    size_t rn = rs485_recv_frame(&s_bus, MODBUS_TIMEOUT_MS);
    uint32_t dt = micros() - t0;
    if (latency_us) *latency_us = dt;

    if (rn == 0) return MB_ERR_SHORT;   /* nothing came back: timeout */

    return modbus_master_parse_read(s_bus.buf, rn, unit, fc, qty,
                                    out, exception_out);
}

/* Retry wrapper. A single lost frame on an electrically noisy bus is normal;
 * treating it as a device failure would make the system flap. */
static mb_result_t txn_read_retry(device_t *d, uint8_t fc, uint16_t addr,
                                  uint16_t qty, uint16_t *out)
{
    mb_result_t r = MB_ERR_SHORT;

    for (int attempt = 0; attempt <= MODBUS_RETRIES; attempt++) {
        uint32_t lat = 0;
        uint8_t  ex  = 0;
        r = txn_read(d->unit, fc, addr, qty, out, &lat, &ex);

        if (r == MB_OK) {
            d->polls_ok++;
            d->latency_us_last = lat;
            d->latency_us_sum += lat;
            if (lat > d->latency_us_max) d->latency_us_max = lat;
            return MB_OK;
        }

        if (r == MB_ERR_CRC)            d->crc_errors++;
        else if (r == MB_ERR_EXCEPTION) { d->exceptions++; return r; }
        else                            d->timeouts++;

        /* Exceptions mean the slave understood us and said no. Retrying an
         * illegal address just gets the same answer, so we return above. */
    }
    return r;
}

/* --- discovery ---------------------------------------------------------- */
static void discover_bus(void)
{
    Serial.printf("# scanning units %d..%d\n", MODBUS_SCAN_FIRST, MODBUS_SCAN_LAST);

    for (uint8_t unit = MODBUS_SCAN_FIRST;
         unit <= MODBUS_SCAN_LAST && s_dev_count < MAX_DEVICES; unit++) {

        uint16_t id[MREG_IDENTITY_COUNT];
        uint32_t lat = 0;
        uint8_t  ex  = 0;

        mb_result_t r = txn_read(unit, MB_FC_READ_HOLDING,
                                 MREG_IDENTITY_BASE, MREG_IDENTITY_COUNT,
                                 id, &lat, &ex);
        if (r != MB_OK) continue;       /* silence just means nobody home */

        if (id[0] != MREG_VENDOR_ID_VALUE) {
            Serial.printf("# unit %d answered but is not ours (vendor 0x%04X)\n",
                          unit, id[0]);
            continue;
        }

        device_t *d      = &s_dev[s_dev_count++];
        memset(d, 0, sizeof(*d));
        d->unit          = unit;
        d->present       = true;
        d->fw_version    = id[2];
        d->capabilities  = id[4];

        Serial.printf("# found unit %d: product=0x%04X fw=v%d.%d map=v%d "
                      "caps=0x%04X%s (%lu us)\n",
                      unit, id[1], id[2] >> 8, id[2] & 0xFF, id[3], id[4],
                      (id[4] & MCAP_SIMULATED) ? " [SIMULATED]" : "",
                      (unsigned long)lat);
    }

    if (!s_dev_count)
        Serial.println("# no devices found -- check wiring, baud rate, "
                       "unit id, and that A/B are not swapped");
}

/* --- polling ------------------------------------------------------------ */
static void poll_device(device_t *d)
{
    uint16_t block[MREG_POLL_COUNT];

    /* FC4 (read input registers) is the semantically correct function for
     * read-only measured values. One transaction fetches the whole
     * measurement and status block. */
    mb_result_t r = txn_read_retry(d, MB_FC_READ_INPUT,
                                   MREG_POLL_BASE, MREG_POLL_COUNT, block);
    if (r != MB_OK) {
        d->last_valid = false;
        Serial.printf("# unit %d poll failed: %s\n", d->unit, mb_result_str(r));
        return;
    }

    meter_regs_decode_poll(block, &d->last);
    d->last_valid = true;

    /* Same 'T,' prefixed format the meter logs locally, with the unit id
     * added. One parser (tools/replay.py) therefore reads captures from
     * either floor of the architecture. */
    Serial.printf("T,%u,%lu,%lu,%.3f,%.4f,%.3f,%.6f,%u,%u,%lu\n",
                  d->unit,
                  (unsigned long)d->last.sample_seq,
                  (unsigned long)millis(),
                  d->last.v, d->last.i, d->last.p, d->last.wh,
                  d->last.status_flags, d->last.truth_mask,
                  (unsigned long)d->latency_us_last);
}

static void print_stats(void)
{
    for (int k = 0; k < s_dev_count; k++) {
        device_t *d = &s_dev[k];
        uint32_t avg = d->polls_ok ? (uint32_t)(d->latency_us_sum / d->polls_ok) : 0;

        Serial.printf("# unit %d ok=%lu timeout=%lu crc=%lu exc=%lu "
                      "latency avg=%lu us max=%lu us slave_overrun=%u\n",
                      d->unit,
                      (unsigned long)d->polls_ok,
                      (unsigned long)d->timeouts,
                      (unsigned long)d->crc_errors,
                      (unsigned long)d->exceptions,
                      (unsigned long)avg,
                      (unsigned long)d->latency_us_max,
                      d->last_valid ? d->last.overruns : 0);
    }
}

/* --- bring-up ----------------------------------------------------------- */
void setup(void)
{
    Serial.begin(SERIAL_BAUD);
    delay(300);

    rs485_begin(&s_bus, &Serial2, MODBUS_BAUD,
                RS485_RX_PIN, RS485_TX_PIN, RS485_DE_PIN);

    Serial.println("# energy-edge-node gateway v1 (Modbus-RTU master)");
    Serial.printf("# baud=%lu t3.5=%lu us timeout=%d ms retries=%d\n",
                  (unsigned long)MODBUS_BAUD, (unsigned long)s_bus.t35_us,
                  MODBUS_TIMEOUT_MS, MODBUS_RETRIES);
    Serial.println("# fields: unit,seq,t_ms,v_v,i_a,p_w,wh,flags,truth,latency_us");

    discover_bus();
}

void loop(void)
{
    static uint32_t last_poll_ms  = 0;
    static uint32_t last_stats_ms = 0;

    uint32_t now = millis();

    if (now - last_poll_ms >= POLL_PERIOD_MS) {
        last_poll_ms = now;
        for (int k = 0; k < s_dev_count; k++)
            poll_device(&s_dev[k]);
    }

    if (now - last_stats_ms >= 10000) {
        last_stats_ms = now;
        print_stats();
    }

    delay(1);
}
