#pragma once
/* ---------------------------------------------------------------------------
 * gateway_node configuration -- Floor 2, "Edge Control".
 *
 * Phase 1 scope: be a Modbus-RTU master. Discover meters on the bus, poll
 * them, report what came back. MQTT (Phase 2) and the demand-response
 * controller (Phase 3) attach to this same polled data.
 * -------------------------------------------------------------------------*/

#define SERIAL_BAUD        115200

/* --- RS-485 / Modbus master -------------------------------------------- */
/* Must match the meter node's settings exactly -- both ends of a bus have to
 * agree on baud rate and framing or nothing works at all. */
#define MODBUS_BAUD        19200
#define RS485_RX_PIN       16
#define RS485_TX_PIN       17
#define RS485_DE_PIN       4

/* How long to wait for a reply before giving up on it.
 * A slave at 19200 baud needs ~40 ms to shift out the longest frame we ask
 * for, so 100 ms leaves generous headroom for a busy slave. */
#define MODBUS_TIMEOUT_MS  100
#define MODBUS_RETRIES     2

/* Bus scan range. The protocol allows unit IDs 1..247, but sweeping all of
 * them at 100 ms each would take 25 s. 16 covers any realistic bench setup
 * and keeps startup quick. */
#define MODBUS_SCAN_FIRST  1
#define MODBUS_SCAN_LAST   16
#define MAX_DEVICES        8

/* --- Polling ----------------------------------------------------------- */
#define POLL_PERIOD_MS     100
