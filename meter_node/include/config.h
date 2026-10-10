#pragma once
/* ---------------------------------------------------------------------------
 * config.h -- single place for every tunable in the meter node.
 * -------------------------------------------------------------------------*/

/* --- Sampling ---------------------------------------------------------- */
/* 10 Hz is plenty for metering/Modbus, and fast enough to catch the switch-on
 * transients that Phase 5 (NILM) needs to tell appliances apart. Raising this
 * costs nothing but CPU -- the ring buffer and log format do not care. */
#define SAMPLE_RATE_HZ        10
#define SAMPLE_PERIOD_MS      (1000 / SAMPLE_RATE_HZ)

/* --- Telemetry ring ---------------------------------------------------- */
/* Must be a power of two. 256 samples @ 10 Hz = ~25 s of history, so a reader
 * can stall for 25 s before it starts losing samples. */
#define TELEMETRY_RING_SIZE   256

/* --- INA226 ------------------------------------------------------------ */
#define INA226_I2C_ADDR       0x40
#define INA226_SDA_PIN        21
#define INA226_SCL_PIN        22
/* Shunt fitted on the breakout board, and the largest current we expect. */
#define INA226_SHUNT_OHMS     0.1f
#define INA226_MAX_CURRENT_A  0.8f

/* --- Serial ------------------------------------------------------------ */
#define SERIAL_BAUD           115200

/* --- Firmware identity (exposed over Modbus) --------------------------- */
#define FW_VERSION            0x0100   /* major<<8 | minor -> v1.0 */

/* --- Modbus-RTU slave over RS-485 -------------------------------------- */
/* 19200 is the usual industrial default and keeps the 3.5-character frame
 * gap at a comfortable ~2 ms. Faster is possible; it buys nothing here and
 * makes framing fussier over long cable runs. */
#define MODBUS_UNIT_ID        1
#define MODBUS_BAUD           19200
#define RS485_RX_PIN          16
#define RS485_TX_PIN          17
#define RS485_DE_PIN          4        /* MAX485 DE+RE tied together */

