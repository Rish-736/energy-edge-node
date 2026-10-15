#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

#include "config.h"
#include "modbus_rtu.h"
#include "meter_regs.h"
#include "rs485.h"
#include "telemetry.h"
#include "ring_buffer.h"
#include "forwarder.h"
#include "control.h"

/* ---------------------------------------------------------------------------
 * gateway_node -- Phases 1 and 2. The "Edge Control" floor.
 *
 * Two tasks, deliberately decoupled by a ring buffer per device:
 *
 *   poll   (prio 4, core 1)  Modbus -> ring.  Runs at a fixed 10 Hz and never
 *                            waits on the network. This is the part that must
 *                            not miss a beat.
 *   uplink (prio 3, core 0)  WiFi + SNTP + MQTT, ring -> broker.
 *
 * THE POINT OF THAT SPLIT: WiFi stalls. DNS lookups block. Brokers go away.
 * None of that is allowed to disturb metering. The poll task keeps filling the
 * ring regardless, and the uplink task drains it when it can -- which is
 * exactly what makes store-and-forward fall out of the design rather than
 * being bolted on.
 *
 * Nothing here decides anything about loads yet. The Phase 3 controller will
 * attach its own reader to these same rings, run on this node, and keep
 * working with the network unplugged.
 * -------------------------------------------------------------------------*/

/* --- per-device state ---------------------------------------------------- */
typedef struct {
    uint8_t  unit;
    bool     present;
    uint16_t fw_version;
    uint16_t capabilities;

    /* Link health, per device: on a real bus one bad cable run fails while
     * the others are fine, and an aggregate counter would hide exactly that. */
    uint32_t polls_ok;
    uint32_t timeouts;
    uint32_t crc_errors;
    uint32_t exceptions;
    uint32_t latency_us_last;
    uint32_t latency_us_max;
    uint64_t latency_us_sum;

    telemetry_ring_t ring;
    forwarder_t      fwd;

    meter_poll_t last;
    bool         last_valid;
    bool         mqtt_announced;
} device_t;

static device_t s_dev[MAX_DEVICES];
static int      s_dev_count = 0;

static rs485_t      s_bus;
static WiFiClient   s_net;
static PubSubClient s_mqtt(s_net);

static char s_client_id[32];

/* =========================================================================
 * Clock
 *
 * We take one SNTP reading and then track time as base + millis() delta,
 * rather than calling time() per sample. Two reasons: time() has 1-second
 * resolution, and reading the system clock repeatedly can step backwards when
 * SNTP corrects it. Monotonic timestamps matter more here than absolute
 * precision -- a log whose time goes backwards is very hard to reason about.
 * =======================================================================*/
static uint64_t s_epoch_base_ms = 0;
static uint32_t s_base_millis   = 0;
static bool     s_clock_ok      = false;

static bool clock_sync(void)
{
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);   /* 0,0 = UTC, no DST */

    uint32_t start = millis();
    while (millis() - start < NTP_SYNC_TIMEOUT_MS) {
        time_t now = time(nullptr);
        /* Before sync the ESP32 clock sits near the epoch, so any plausible
         * recent date means SNTP has answered. */
        if (now > 1700000000) {
            s_epoch_base_ms = (uint64_t)now * 1000ULL;
            s_base_millis   = millis();
            s_clock_ok      = true;

            struct tm tm_utc;
            gmtime_r(&now, &tm_utc);
            Serial.printf("# clock synced: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                          tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                          tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    Serial.println("# clock sync FAILED -- timestamps will be 0");
    return false;
}

/* Wall-clock milliseconds, or 0 when we have never synced. Zero is an honest
 * "unknown" that downstream can test for, which is better than inventing a
 * plausible-looking wrong time. */
static uint64_t now_unix_ms(void)
{
    if (!s_clock_ok) return 0;
    return s_epoch_base_ms + (uint64_t)(millis() - s_base_millis);
}

/* Lets the controller attach its own reader to each device's ring without
 * either file needing the other's struct layout. */
static telemetry_ring_t *device_ring(int index)
{
    if (index < 0 || index >= s_dev_count) return NULL;
    return &s_dev[index].ring;
}

/* =========================================================================
 * Modbus master
 * =======================================================================*/
static mb_result_t txn_read(uint8_t unit, uint8_t fc, uint16_t addr,
                            uint16_t qty, uint16_t *out,
                            uint32_t *latency_us, uint8_t *exception_out)
{
    uint8_t req[MODBUS_MAX_FRAME];
    int n = modbus_master_build_read(unit, fc, addr, qty, req, sizeof(req));
    if (n < 0) return MB_ERR_ARG;

    /* Drop anything already in the receive path. Without this, a reply that
     * arrived late from the PREVIOUS question gets parsed as the answer to
     * this one -- plausible-looking but wrong data, the worst failure mode. */
    rs485_flush_input(&s_bus);

    uint32_t t0 = micros();
    rs485_send(&s_bus, req, (size_t)n);

    size_t rn = rs485_recv_frame(&s_bus, MODBUS_TIMEOUT_MS);
    uint32_t dt = micros() - t0;
    if (latency_us) *latency_us = dt;

    if (rn == 0) return MB_ERR_SHORT;       /* timeout */

    return modbus_master_parse_read(s_bus.buf, rn, unit, fc, qty,
                                    out, exception_out);
}

/* A single lost frame on an electrically noisy bus is normal; treating it as
 * a device failure would make the system flap. */
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
        /* An exception means the slave understood and said no. Retrying an
         * illegal address gets the same answer, so stop. */
        else if (r == MB_ERR_EXCEPTION) { d->exceptions++; return r; }
        else                            d->timeouts++;
    }
    return r;
}

static void discover_bus(void)
{
    Serial.printf("# scanning units %d..%d\n", MODBUS_SCAN_FIRST, MODBUS_SCAN_LAST);

    for (uint8_t unit = MODBUS_SCAN_FIRST;
         unit <= MODBUS_SCAN_LAST && s_dev_count < MAX_DEVICES; unit++) {

        uint16_t id[MREG_IDENTITY_COUNT];
        uint32_t lat = 0;
        uint8_t  ex  = 0;

        if (txn_read(unit, MB_FC_READ_HOLDING, MREG_IDENTITY_BASE,
                     MREG_IDENTITY_COUNT, id, &lat, &ex) != MB_OK)
            continue;                       /* silence just means nobody home */

        if (id[0] != MREG_VENDOR_ID_VALUE) {
            Serial.printf("# unit %d answered but is not ours (vendor 0x%04X)\n",
                          unit, id[0]);
            continue;
        }

        device_t *d = &s_dev[s_dev_count++];
        memset(d, 0, sizeof(*d));
        d->unit         = unit;
        d->present      = true;
        d->fw_version   = id[2];
        d->capabilities = id[4];

        ring_init(&d->ring);
        forwarder_init(&d->fwd, &d->ring, BACKFILL_THRESHOLD);

        Serial.printf("# found unit %d: product=0x%04X fw=v%d.%d map=v%d "
                      "caps=0x%04X%s (%lu us)\n",
                      unit, id[1], id[2] >> 8, id[2] & 0xFF, id[3], id[4],
                      (id[4] & MCAP_SIMULATED) ? " [SIMULATED]" : "",
                      (unsigned long)lat);
    }

    if (!s_dev_count)
        Serial.println("# no devices found -- check wiring, baud rate, unit id, "
                       "and that A/B are not swapped");
}

/* =========================================================================
 * Poll task: Modbus -> ring
 * =======================================================================*/
static void poll_device(device_t *d)
{
    uint16_t block[MREG_POLL_COUNT];

    /* FC4 (read input registers) is semantically correct for read-only
     * measured values. One transaction fetches measurements and status. */
    if (txn_read_retry(d, MB_FC_READ_INPUT,
                       MREG_POLL_BASE, MREG_POLL_COUNT, block) != MB_OK) {
        d->last_valid = false;
        return;
    }

    meter_regs_decode_poll(block, &d->last);
    d->last_valid = true;

    /* Into the ring it goes, network or no network. */
    telemetry_sample_t s;
    memset(&s, 0, sizeof(s));
    s.t_ms      = millis();
    s.t_unix_ms = now_unix_ms();
    s.v         = d->last.v;
    s.i         = d->last.i;
    s.p         = d->last.p;
    s.wh        = d->last.wh;
    s.flags     = (uint8_t)d->last.status_flags;
    s.truth     = (uint8_t)d->last.truth_mask;

    ring_push(&d->ring, &s);
}

static void task_poll(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        for (int k = 0; k < s_dev_count; k++)
            poll_device(&s_dev[k]);

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

/* =========================================================================
 * Uplink task: WiFi + SNTP + MQTT, ring -> broker
 * =======================================================================*/
static void topic(char *buf, size_t cap, const char *fmt, ...)
{
    int n = snprintf(buf, cap, "%s/", MQTT_TOPIC_ROOT);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, cap - (size_t)n, fmt, ap);
    va_end(ap);
}

static bool net_up(void)
{
    return WiFi.status() == WL_CONNECTED;
}

static void wifi_begin(void)
{
    if (!strlen(WIFI_SSID)) return;        /* no secrets.h: stay offline */
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                   /* modem sleep adds latency and
                                             * drops packets we would rather
                                             * not have to re-send */
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("# wifi: connecting to \"%s\"\n", WIFI_SSID);
}

static bool mqtt_connect(void)
{
    char will[64];
    topic(will, sizeof(will), "gateway/status");

    /* Last Will and Testament: if this node drops off without saying goodbye,
     * the broker publishes "offline" on its behalf. Without it a dead gateway
     * looks identical to an idle one, and the dashboard would keep showing
     * the last reading as if it were current. */
    bool ok = s_mqtt.connect(s_client_id,
                             strlen(MQTT_USER) ? MQTT_USER : nullptr,
                             strlen(MQTT_PASS) ? MQTT_PASS : nullptr,
                             will, 0, true, "offline", true);
    if (!ok) {
        Serial.printf("# mqtt: connect failed, state=%d\n", s_mqtt.state());
        return false;
    }

    s_mqtt.publish(will, "online", true);
    Serial.printf("# mqtt: connected to %s:%d as %s\n",
                  MQTT_HOST, MQTT_PORT, s_client_id);

    /* Re-announce each meter, retained, so a dashboard opened later knows
     * what exists without waiting for a poll. */
    for (int k = 0; k < s_dev_count; k++) {
        device_t *d = &s_dev[k];
        char t[64], payload[160];
        topic(t, sizeof(t), "meter/%u/status", d->unit);
        snprintf(payload, sizeof(payload),
                 "{\"online\":1,\"fw\":\"%d.%d\",\"caps\":%u,\"simulated\":%d}",
                 d->fw_version >> 8, d->fw_version & 0xFF,
                 d->capabilities,
                 (d->capabilities & MCAP_SIMULATED) ? 1 : 0);
        s_mqtt.publish(t, payload, true);
        d->mqtt_announced = true;
    }
    return true;
}

static void publish_batch(device_t *d)
{
    telemetry_sample_t batch[MQTT_BATCH];
    size_t n = forwarder_take(&d->fwd, batch, MQTT_BATCH);
    if (!n) return;

    char t[64];
    topic(t, sizeof(t), "meter/%u/telemetry", d->unit);

    for (size_t k = 0; k < n; k++) {
        const telemetry_sample_t *s = &batch[k];

        float kwh  = s->wh / 1000.0f;
        float cost = kwh * TARIFF_INR_PER_KWH;
        float co2  = kwh * CO2_G_PER_KWH;

        /* Hand-rolled JSON rather than a library: the shape is fixed, this is
         * ~40 lines lighter, and there is no allocation on the hot path.
         * Short keys keep each packet near 220 bytes. */
        char payload[MQTT_BUFFER_BYTES];
        int len = snprintf(payload, sizeof(payload),
            "{\"unit\":%u,\"seq\":%lu,\"t\":%llu,\"up\":%lu,"
            "\"v\":%.3f,\"i\":%.4f,\"p\":%.3f,\"wh\":%.6f,"
            "\"cost\":%.4f,\"co2\":%.2f,"
            "\"flags\":%u,\"truth\":%u,\"bf\":%d}",
            d->unit,
            (unsigned long)s->seq,
            (unsigned long long)s->t_unix_ms,
            (unsigned long)s->t_ms,
            s->v, s->i, s->p, s->wh,
            cost, co2,
            s->flags, s->truth,
            (s->flags & TLM_FLAG_BACKFILL) ? 1 : 0);

        if (len > 0 && (size_t)len < sizeof(payload))
            s_mqtt.publish(t, payload);
    }
}

static void publish_stats(void)
{
    char t[64], payload[320];
    topic(t, sizeof(t), "gateway/stats");

    /* One aggregate line for the dashboard's health strip. Per-device detail
     * goes to the serial console. */
    uint32_t backlog = 0, dropped = 0, outages = 0, published = 0;
    for (int k = 0; k < s_dev_count; k++) {
        backlog   += forwarder_backlog(&s_dev[k].fwd);
        dropped   += forwarder_dropped(&s_dev[k].fwd);
        outages   += s_dev[k].fwd.outages;
        published += s_dev[k].fwd.published;
    }

    snprintf(payload, sizeof(payload),
        "{\"devices\":%d,\"published\":%lu,\"backlog\":%lu,\"dropped\":%lu,"
        "\"outages\":%lu,\"rssi\":%d,\"heap\":%lu,\"clock\":%d}",
        s_dev_count,
        (unsigned long)published, (unsigned long)backlog,
        (unsigned long)dropped, (unsigned long)outages,
        net_up() ? WiFi.RSSI() : 0,
        (unsigned long)ESP.getFreeHeap(),
        s_clock_ok ? 1 : 0);

    s_mqtt.publish(t, payload, true);
}

static void task_uplink(void *arg)
{
    (void)arg;

    uint32_t backoff_ms      = RECONNECT_MIN_MS;
    uint32_t next_attempt_ms = 0;
    uint32_t last_stats_ms   = 0;
    uint32_t last_sync_ms    = 0;
    bool     was_linked      = false;

    wifi_begin();

    for (;;) {
        uint32_t now = millis();
        bool linked  = net_up() && s_mqtt.connected();

        /* One place decides whether the uplink is usable. The forwarders
         * buffer or drain purely off this. */
        if (linked != was_linked) {
            for (int k = 0; k < s_dev_count; k++)
                forwarder_set_link(&s_dev[k].fwd, linked);

            Serial.printf("# uplink %s\n", linked ? "UP" : "DOWN");
            if (!linked) {
                Serial.println("#   buffering readings -- they will be "
                               "published when the link returns");
            } else {
                uint32_t b = 0;
                for (int k = 0; k < s_dev_count; k++)
                    b += forwarder_backlog(&s_dev[k].fwd);
                if (b) Serial.printf("#   backfilling %lu buffered samples\n",
                                     (unsigned long)b);
            }
            was_linked = linked;
        }

        if (!linked) {
            if (now >= next_attempt_ms && strlen(MQTT_HOST)) {
                if (!net_up()) {
                    if (WiFi.status() == WL_DISCONNECTED) WiFi.reconnect();
                } else {
                    if (!s_clock_ok || now - last_sync_ms > NTP_RESYNC_PERIOD_MS) {
                        if (clock_sync()) last_sync_ms = now;
                    }
                    mqtt_connect();
                }
                /* Exponential backoff, capped. Hammering a down broker every
                 * 20 ms burns CPU and floods the log. */
                next_attempt_ms = now + backoff_ms;
                backoff_ms = (backoff_ms * 2 > RECONNECT_MAX_MS)
                           ? RECONNECT_MAX_MS : backoff_ms * 2;
            }
        } else {
            backoff_ms = RECONNECT_MIN_MS;
            s_mqtt.loop();

            for (int k = 0; k < s_dev_count; k++)
                publish_batch(&s_dev[k]);

            if (now - last_stats_ms >= 5000) {
                last_stats_ms = now;
                publish_stats();
            }

            if (now - last_sync_ms > NTP_RESYNC_PERIOD_MS) {
                if (clock_sync()) last_sync_ms = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(UPLINK_PERIOD_MS));
    }
}

/* =========================================================================
 * Bring-up
 * =======================================================================*/
void setup(void)
{
    Serial.begin(SERIAL_BAUD);
    delay(300);

    uint64_t mac = ESP.getEfuseMac();
    snprintf(s_client_id, sizeof(s_client_id), "%s%04X",
             MQTT_CLIENT_PREFIX, (unsigned)(mac & 0xFFFF));

    rs485_begin(&s_bus, &Serial2, MODBUS_BAUD,
                RS485_RX_PIN, RS485_TX_PIN, RS485_DE_PIN);

    Serial.println("# energy-edge-node gateway v2 (Modbus master + MQTT)");
    Serial.printf("# baud=%lu t3.5=%lu us timeout=%d ms retries=%d\n",
                  (unsigned long)MODBUS_BAUD, (unsigned long)s_bus.t35_us,
                  MODBUS_TIMEOUT_MS, MODBUS_RETRIES);
    Serial.printf("# ring=%d samples (%.0f s of outage coverage at %d Hz)\n",
                  TELEMETRY_RING_SIZE,
                  (float)TELEMETRY_RING_SIZE / (1000.0f / POLL_PERIOD_MS),
                  1000 / POLL_PERIOD_MS);
    Serial.printf("# mqtt=%s:%d root=%s client=%s\n",
                  strlen(MQTT_HOST) ? MQTT_HOST : "(disabled)",
                  MQTT_PORT, MQTT_TOPIC_ROOT, s_client_id);

    discover_bus();

    s_mqtt.setServer(MQTT_HOST, MQTT_PORT);
    s_mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
    s_mqtt.setBufferSize(MQTT_BUFFER_BYTES);

    xTaskCreatePinnedToCore(task_poll,   "poll",   4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(task_uplink, "uplink", 8192, NULL, 3, NULL, 0);

    /* Started after discovery, so it knows how many rings to read from. */
    control_task_start(device_ring, s_dev_count);

    /* A demo job so a bench run has something to schedule. On a real site
     * this arrives from the charge point when a vehicle plugs in. */
    control_set_charger_job(1.5f, millis() + 480000u);
}

void loop(void)
{
    /* Periodic console report. Everything functional runs in the two tasks. */
    for (int k = 0; k < s_dev_count; k++) {
        device_t *d = &s_dev[k];
        uint32_t avg = d->polls_ok ? (uint32_t)(d->latency_us_sum / d->polls_ok) : 0;

        Serial.printf("# unit %d ok=%lu timeout=%lu crc=%lu exc=%lu "
                      "lat avg=%lu max=%lu us | pub=%lu backlog=%lu "
                      "dropped=%lu peak_backlog=%lu outages=%lu\n",
                      d->unit,
                      (unsigned long)d->polls_ok, (unsigned long)d->timeouts,
                      (unsigned long)d->crc_errors, (unsigned long)d->exceptions,
                      (unsigned long)avg, (unsigned long)d->latency_us_max,
                      (unsigned long)d->fwd.published,
                      (unsigned long)forwarder_backlog(&d->fwd),
                      (unsigned long)forwarder_dropped(&d->fwd),
                      (unsigned long)d->fwd.max_backlog,
                      (unsigned long)d->fwd.outages);
    }
    delay(10000);
}
