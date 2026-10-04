#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * meter_regs.h -- the meter node's Modbus register map.
 *
 * This header is the single source of truth for the map, shared by BOTH the
 * slave (which serves the registers) and the master (which decodes them), so
 * the two can never drift apart. docs/MODBUS_REGISTERS.md documents the same
 * addresses for anyone integrating from outside, and docs/device_model.json
 * carries them in machine-readable form.
 *
 * ADDRESSING: all addresses here are PROTOCOL addresses, i.e. 0-based, which
 * is what actually travels on the wire. Vendor documentation often prints
 * 1-based "register numbers" instead (register 40001 = holding address 0).
 * Mixing the two conventions is the most common Modbus integration mistake
 * after float word order, so this file commits to one and says so.
 *
 * Measurements are exposed through BOTH function codes -- 0x04 (read input
 * registers) is semantically correct for read-only measured values, but a lot
 * of masters only ever implement 0x03, so real meters answer both. We do too.
 * -------------------------------------------------------------------------*/

/* --- identity: 0x0000 .. 0x0004 ----------------------------------------- */
#define MREG_VENDOR_ID        0x0000  /* u16  0x4545 ("EE")               */
#define MREG_PRODUCT_ID       0x0001  /* u16  0x0001 = meter node         */
#define MREG_FW_VERSION       0x0002  /* u16  major<<8 | minor            */
#define MREG_MAP_VERSION      0x0003  /* u16  this map's version          */
#define MREG_CAPABILITIES     0x0004  /* u16  MCAP_* bits                 */

#define MREG_VENDOR_ID_VALUE  0x4545
#define MREG_PRODUCT_ID_VALUE 0x0001
#define MREG_MAP_VERSION_VALUE 1

#define MCAP_SIMULATED        0x0001  /* readings come from the simulator */
#define MCAP_FLOAT32          0x0002  /* float block present              */
#define MCAP_SCALED           0x0004  /* scaled-integer block present     */
#define MCAP_WRITABLE         0x0008  /* accepts FC6 / FC16               */

/* --- float32 measurements: 0x0010 .. 0x0017 ------------------------------ */
/* Two registers each, HIGH WORD FIRST (PowerLogic convention). */
#define MREG_VOLTAGE_F32      0x0010  /* volts      */
#define MREG_CURRENT_F32      0x0012  /* amps       */
#define MREG_POWER_F32        0x0014  /* watts      */
#define MREG_ENERGY_F32       0x0016  /* watt-hours */

/* --- scaled integers: 0x0020 .. 0x0024 ---------------------------------- */
/* For masters without float support -- plenty of PLCs and cheap HMIs. */
#define MREG_VOLTAGE_X100     0x0020  /* u16  volts * 100       */
#define MREG_CURRENT_MA       0x0021  /* i16  milliamps         */
#define MREG_POWER_X10        0x0022  /* i16  watts * 10        */
#define MREG_ENERGY_MWH       0x0023  /* u32  milliwatt-hours   */

/* --- status: 0x0030 .. 0x0036 ------------------------------------------- */
#define MREG_STATUS_FLAGS     0x0030  /* u16  TLM_FLAG_* bits                */
#define MREG_TRUTH_MASK       0x0031  /* u16  LOAD_* -- simulator ground
                                       *      truth, 0 on real hardware     */
#define MREG_SAMPLE_SEQ       0x0032  /* u32  monotonic sample counter       */
#define MREG_UPTIME_S         0x0034  /* u32  seconds since boot             */
#define MREG_OVERRUNS         0x0036  /* u16  samples the Modbus reader lost */

/* --- control: 0x0040 (writable, FC6/FC16) ------------------------------- */
#define MREG_CMD              0x0040
#define MREG_CMD_RESET_ENERGY 0x0001

/* --- block reads --------------------------------------------------------
 * Addresses inside the mapped span but not listed above read back as 0
 * (reserved), so a master can fetch measurements and status in ONE
 * transaction instead of four. That is how you keep poll latency low on a
 * real bus. Anything outside the span raises ILLEGAL_ADDRESS.             */
#define MREG_SPAN_FIRST       0x0000
#define MREG_SPAN_LAST        0x0040

#define MREG_POLL_BASE        MREG_VOLTAGE_F32
#define MREG_POLL_COUNT       39      /* 0x0010 .. 0x0036 inclusive */

#define MREG_IDENTITY_BASE    MREG_VENDOR_ID
#define MREG_IDENTITY_COUNT   5

/* =========================================================================
 * Slave-side binding
 *
 * Plain scalars rather than a telemetry_sample_t, so this file -- and the
 * gateway that includes it -- stays independent of the meter's internals.
 * =======================================================================*/
typedef struct {
    float    v, i, p, wh;
    uint16_t status_flags;
    uint16_t truth_mask;
    uint32_t sample_seq;
    uint32_t uptime_s;
    uint16_t overruns;
    uint16_t fw_version;
    bool     simulated;
} meter_data_t;

typedef struct {
    meter_data_t data;
    bool energy_reset_requested;   /* set by a write to MREG_CMD; the
                                    * sampling task clears it after acting */
} meter_regs_t;

void meter_regs_init(meter_regs_t *r, uint16_t fw_version, bool simulated);

/* Callbacks matching mb_read_reg_fn / mb_write_reg_fn, with `ctx` being a
 * meter_regs_t*. */
bool meter_regs_read(uint16_t addr, uint16_t *out, void *ctx);
bool meter_regs_write(uint16_t addr, uint16_t val, void *ctx);

/* =========================================================================
 * Master-side decoding -- what the gateway gets back from one poll.
 * =======================================================================*/
typedef struct {
    float    v, i, p, wh;
    uint16_t status_flags;
    uint16_t truth_mask;
    uint32_t sample_seq;
    uint32_t uptime_s;
    uint16_t overruns;
} meter_poll_t;

/* Decode a MREG_POLL_COUNT-register block read from MREG_POLL_BASE. */
void meter_regs_decode_poll(const uint16_t *regs, meter_poll_t *out);
