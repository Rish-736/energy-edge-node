#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * modbus_rtu.h -- Modbus-RTU framing, slave dispatch and master helpers.
 *
 * Deliberately transport-agnostic: nothing in here knows about UARTs, RS-485
 * or ESP32. You hand it a received frame and a buffer for the reply. That is
 * what lets the whole protocol be unit-tested on a PC, with a master and a
 * slave talking to each other through memory, before any RS-485 hardware
 * exists -- and it is also how the same code will later serve Modbus-TCP,
 * which differs only in framing.
 *
 * Supported function codes:
 *   0x03  read holding registers
 *   0x04  read input registers
 *   0x06  write single register
 *   0x10  write multiple registers
 *
 * Frame layout (RTU):
 *   [unit_id][function][...payload...][crc_lo][crc_hi]
 *
 * Note the CRC byte order: Modbus sends the CRC low byte FIRST, which is the
 * opposite of every other 16-bit field in the protocol (those are big-endian).
 * That inconsistency is in the spec, not a mistake here.
 * -------------------------------------------------------------------------*/

/* Largest RTU frame on the wire, per the spec. */
#define MODBUS_MAX_FRAME   256

/* A single read response carries a 1-byte length field, so at most 125
 * registers (250 data bytes + unit + fc + len + 2 CRC = 255). */
#define MODBUS_MAX_READ_REGS 125
#define MODBUS_MAX_WRITE_REGS 123

/* Function codes */
#define MB_FC_READ_HOLDING   0x03
#define MB_FC_READ_INPUT     0x04
#define MB_FC_WRITE_SINGLE   0x06
#define MB_FC_WRITE_MULTIPLE 0x10

/* Exception codes (sent back with the function code's high bit set) */
#define MB_EX_ILLEGAL_FUNCTION 0x01
#define MB_EX_ILLEGAL_ADDRESS  0x02
#define MB_EX_ILLEGAL_VALUE    0x03
#define MB_EX_DEVICE_FAILURE   0x04

/* Master-side results. Negative values are failures. */
typedef enum {
    MB_OK            =  0,
    MB_ERR_SHORT     = -1,   /* frame too short to be valid              */
    MB_ERR_CRC       = -2,   /* checksum mismatch -- line noise          */
    MB_ERR_UNIT      = -3,   /* reply came from the wrong device         */
    MB_ERR_FC        = -4,   /* reply's function code did not match      */
    MB_ERR_LEN       = -5,   /* byte count inconsistent with the request */
    MB_ERR_EXCEPTION = -6,   /* slave returned a Modbus exception        */
    MB_ERR_ARG       = -7,   /* caller passed something impossible       */
} mb_result_t;

/* --- CRC ---------------------------------------------------------------- */
uint16_t modbus_crc16(const uint8_t *buf, size_t len);

/* --- big-endian register helpers ---------------------------------------- */
static inline uint16_t mb_get_u16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }

static inline void mb_put_u16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF); }

/* =========================================================================
 * Slave side
 * =======================================================================*/

/* Register access callbacks. Returning false means "no such register here",
 * which the dispatcher turns into an ILLEGAL_ADDRESS exception. */
typedef bool (*mb_read_reg_fn)(uint16_t addr, uint16_t *out, void *ctx);
typedef bool (*mb_write_reg_fn)(uint16_t addr, uint16_t val, void *ctx);

typedef struct {
    uint8_t         unit_id;
    mb_read_reg_fn  read_reg;
    mb_write_reg_fn write_reg;   /* NULL = device is read-only */
    void           *ctx;

    /* Diagnostics. Real meters expose counters like these, and they are what
     * you actually look at when a bus misbehaves. */
    uint32_t requests;      /* frames addressed to us and understood */
    uint32_t crc_errors;    /* frames dropped for a bad checksum     */
    uint32_t exceptions;    /* requests answered with an exception   */
    uint32_t ignored;       /* frames for some other device          */
} modbus_slave_t;

void modbus_slave_init(modbus_slave_t *s, uint8_t unit_id,
                       mb_read_reg_fn rd, mb_write_reg_fn wr, void *ctx);

/* Process one received frame.
 *
 * Returns the number of bytes written to `resp`, or 0 when nothing should be
 * sent. Staying silent is the correct Modbus behaviour for a bad CRC, for a
 * frame addressed to another device, and for a broadcast (unit 0) -- a slave
 * that answers a corrupt frame makes a noisy bus worse. */
size_t modbus_slave_handle(modbus_slave_t *s,
                           const uint8_t *req, size_t req_len,
                           uint8_t *resp, size_t resp_cap);

/* =========================================================================
 * Master side
 * =======================================================================*/

/* Build a read request (FC3 or FC4). Returns frame length, or MB_ERR_ARG. */
int modbus_master_build_read(uint8_t unit_id, uint8_t fc,
                             uint16_t addr, uint16_t qty,
                             uint8_t *out, size_t cap);

/* Parse a read reply into `regs` (must hold `qty` entries).
 * On MB_ERR_EXCEPTION, *exception_out (if given) receives the slave's code. */
mb_result_t modbus_master_parse_read(const uint8_t *resp, size_t len,
                                     uint8_t unit_id, uint8_t fc, uint16_t qty,
                                     uint16_t *regs, uint8_t *exception_out);

/* Build a write-single-register request (FC6). */
int modbus_master_build_write_single(uint8_t unit_id, uint16_t addr,
                                     uint16_t val, uint8_t *out, size_t cap);

/* Parse an FC6 reply, which echoes the request's address and value. */
mb_result_t modbus_master_parse_write_single(const uint8_t *resp, size_t len,
                                             uint8_t unit_id, uint16_t addr,
                                             uint16_t val,
                                             uint8_t *exception_out);

/* --- float32 across two registers --------------------------------------- */
/* Modbus has no float type and no agreed word order, so every vendor picks
 * one. We use HIGH WORD FIRST, matching Schneider PowerLogic meters. Getting
 * this backwards is the classic Modbus integration bug -- values come out as
 * absurd magnitudes -- so the convention is stated in the register map doc
 * and enforced by a unit test. */
void  mb_put_f32(uint16_t *regs, float v);
float mb_get_f32(const uint16_t *regs);

/* uint32 across two registers, same word order. */
void     mb_put_u32(uint16_t *regs, uint32_t v);
uint32_t mb_get_u32(const uint16_t *regs);

const char *mb_result_str(mb_result_t r);
