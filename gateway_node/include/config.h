#pragma once
/* ---------------------------------------------------------------------------
 * gateway_node configuration -- Floor 2, "Edge Control".
 *
 * Phase 1: Modbus-RTU master -- discover meters, poll them, report link health.
 * Phase 2: MQTT uplink with store-and-forward buffering, and real clock time.
 * Phase 3: the demand-response controller attaches to this same polled data.
 * -------------------------------------------------------------------------*/

/* Credentials live in secrets.h, which is gitignored. Compiling without it
 * is allowed -- the firmware runs and polls Modbus, it just cannot reach the
 * network. That keeps the repo clonable and buildable by anyone. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#ifndef WIFI_SSID
#  define WIFI_SSID  ""
#  define WIFI_PASS  ""
#  define MQTT_HOST  ""
#  define MQTT_PORT  1883
#  define MQTT_USER  ""
#  define MQTT_PASS  ""
#  warning "No secrets.h -- networking disabled. Copy secrets.example.h to secrets.h."
#endif

#define SERIAL_BAUD        115200

/* --- RS-485 / Modbus master -------------------------------------------- */
/* Must match the meter node exactly -- both ends of a bus have to agree on
 * baud rate and framing or nothing works at all. */
#define MODBUS_BAUD        19200
#define RS485_RX_PIN       16
#define RS485_TX_PIN       17
#define RS485_DE_PIN       4

/* A slave at 19200 baud needs ~40 ms to shift out the longest frame we ask
 * for, so 100 ms leaves generous headroom for a busy slave. */
#define MODBUS_TIMEOUT_MS  100
#define MODBUS_RETRIES     2

/* The protocol allows unit IDs 1..247, but sweeping all of them at 100 ms
 * each would take 25 s. 16 covers any realistic bench setup. */
#define MODBUS_SCAN_FIRST  1
#define MODBUS_SCAN_LAST   16

/* --- Polling and buffering --------------------------------------------- */
#define POLL_PERIOD_MS     100         /* 10 Hz, matching the meter */

/* Each device gets its own ring, so one meter's outage accounting never
 * mixes with another's. That costs RAM:
 *
 *   MAX_DEVICES x TELEMETRY_RING_SIZE x ~40 bytes
 *   2 x 512 x 40 = ~41 kB
 *
 * plus ~50 kB for the WiFi and MQTT stacks. Two devices and 512 samples
 * (= 51 s of outage coverage at 10 Hz) is the balance for a bench setup;
 * raising either means watching the heap.
 *
 * TELEMETRY_RING_SIZE is set in platformio.ini so the shared ring buffer
 * picks it up. */
#define MAX_DEVICES        2

/* --- MQTT --------------------------------------------------------------- */
#define MQTT_TOPIC_ROOT    "een/bench"
#define MQTT_CLIENT_PREFIX "een-gw-"
#define MQTT_KEEPALIVE_S   15
#define MQTT_BUFFER_BYTES  512         /* our JSON payload is ~220 bytes */

/* How many buffered samples may be published per uplink pass. Caps the
 * catch-up rate after an outage so draining a full buffer cannot monopolise
 * the uplink or starve the polling task. */
#define MQTT_BATCH         10
#define UPLINK_PERIOD_MS   20

/* A backlog above this many samples means we are catching up rather than
 * live, and the samples get tagged as backfill. Two poll periods' worth of
 * slack absorbs normal scheduling jitter without crying outage. */
#define BACKFILL_THRESHOLD 20

/* Reconnect backoff, milliseconds. Retrying a down broker every 20 ms just
 * burns CPU and fills the log. */
#define RECONNECT_MIN_MS   1000
#define RECONNECT_MAX_MS   15000

/* --- Time --------------------------------------------------------------- */
/* Timestamps in UTC. Local time is a presentation concern -- storing local
 * time means a log that is ambiguous for one hour of every year. */
#define NTP_SERVER_1       "pool.ntp.org"
#define NTP_SERVER_2       "time.google.com"
#define NTP_SYNC_TIMEOUT_MS 8000
#define NTP_RESYNC_PERIOD_MS 3600000UL /* re-sync hourly; the ESP32's RC
                                        * oscillator drifts noticeably */

/* --- Tariff and emissions ---------------------------------------------- */
/* Flat rate for now. Time-of-use pricing -- the thing that makes shedding
 * worth money at specific hours -- arrives with the Phase 3 controller.
 *
 * Both figures are assumptions, not measurements: set them to your own
 * tariff and your grid's published factor before quoting any result. */
#define TARIFF_INR_PER_KWH 8.0f
#define CO2_G_PER_KWH      710.0f      /* India grid average, approximate */
