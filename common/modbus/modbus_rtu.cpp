#include "modbus_rtu.h"
#include <string.h>

/* --- CRC ---------------------------------------------------------------- */
/* Modbus CRC-16: polynomial 0xA001 (reversed 0x8005), initial value 0xFFFF,
 * processed least-significant-bit first. Computed bitwise rather than from a
 * lookup table: at 9600-115200 baud this is nowhere near a bottleneck, and it
 * keeps 512 bytes of flash free. */
uint16_t modbus_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* CRC goes on the wire low byte first. */
static void crc_append(uint8_t *frame, size_t len_without_crc)
{
    uint16_t crc = modbus_crc16(frame, len_without_crc);
    frame[len_without_crc]     = (uint8_t)(crc & 0xFF);
    frame[len_without_crc + 1] = (uint8_t)(crc >> 8);
}

static bool crc_ok(const uint8_t *frame, size_t len)
{
    if (len < 3) return false;
    uint16_t want = (uint16_t)frame[len - 2] | (uint16_t)((uint16_t)frame[len - 1] << 8);
    return modbus_crc16(frame, len - 2) == want;
}

/* --- float / u32 packing ------------------------------------------------- */
void mb_put_f32(uint16_t *regs, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);          /* memcpy, not a pointer cast: type
                                    * punning through a cast is undefined
                                    * behaviour and GCC does optimise on it */
    regs[0] = (uint16_t)(bits >> 16);   /* high word first */
    regs[1] = (uint16_t)(bits & 0xFFFF);
}

float mb_get_f32(const uint16_t *regs)
{
    uint32_t bits = ((uint32_t)regs[0] << 16) | (uint32_t)regs[1];
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

void mb_put_u32(uint16_t *regs, uint32_t v)
{
    regs[0] = (uint16_t)(v >> 16);
    regs[1] = (uint16_t)(v & 0xFFFF);
}

uint32_t mb_get_u32(const uint16_t *regs)
{
    return ((uint32_t)regs[0] << 16) | (uint32_t)regs[1];
}

/* =========================================================================
 * Slave
 * =======================================================================*/

void modbus_slave_init(modbus_slave_t *s, uint8_t unit_id,
                       mb_read_reg_fn rd, mb_write_reg_fn wr, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->unit_id   = unit_id;
    s->read_reg  = rd;
    s->write_reg = wr;
    s->ctx       = ctx;
}

static size_t make_exception(modbus_slave_t *s, uint8_t fc, uint8_t code,
                             uint8_t *resp, size_t cap)
{
    if (cap < 5) return 0;
    s->exceptions++;
    resp[0] = s->unit_id;
    resp[1] = (uint8_t)(fc | 0x80);   /* high bit marks an exception reply */
    resp[2] = code;
    crc_append(resp, 3);
    return 5;
}

size_t modbus_slave_handle(modbus_slave_t *s,
                           const uint8_t *req, size_t req_len,
                           uint8_t *resp, size_t resp_cap)
{
    /* Shortest legal request is unit + fc + 2 CRC. */
    if (req_len < 4) return 0;

    if (!crc_ok(req, req_len)) {
        s->crc_errors++;
        return 0;               /* silence is correct: the frame may not even
                                 * have been addressed to us */
    }

    uint8_t unit = req[0];
    uint8_t fc   = req[1];

    if (unit != s->unit_id && unit != 0) {
        s->ignored++;
        return 0;
    }
    bool broadcast = (unit == 0);   /* broadcast is never answered */

    s->requests++;

    switch (fc) {

    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT: {
        if (req_len != 8)
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_VALUE, resp, resp_cap);

        uint16_t addr = mb_get_u16(req + 2);
        uint16_t qty  = mb_get_u16(req + 4);

        if (qty < 1 || qty > MODBUS_MAX_READ_REGS)
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_VALUE, resp, resp_cap);

        size_t need = 3 + (size_t)qty * 2 + 2;
        if (resp_cap < need) return 0;

        /* Read everything before emitting anything: a partially written
         * response to a range that turns out to be invalid would be worse
         * than a clean exception. */
        uint16_t vals[MODBUS_MAX_READ_REGS];
        for (uint16_t k = 0; k < qty; k++) {
            if (!s->read_reg((uint16_t)(addr + k), &vals[k], s->ctx))
                return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_cap);
        }
        if (broadcast) return 0;

        resp[0] = s->unit_id;
        resp[1] = fc;
        resp[2] = (uint8_t)(qty * 2);
        for (uint16_t k = 0; k < qty; k++)
            mb_put_u16(resp + 3 + k * 2, vals[k]);
        crc_append(resp, 3 + (size_t)qty * 2);
        return need;
    }

    case MB_FC_WRITE_SINGLE: {
        if (req_len != 8)
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_VALUE, resp, resp_cap);
        if (!s->write_reg)
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_cap);

        uint16_t addr = mb_get_u16(req + 2);
        uint16_t val  = mb_get_u16(req + 4);

        if (!s->write_reg(addr, val, s->ctx))
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_cap);
        if (broadcast) return 0;

        /* FC6 replies by echoing the request verbatim. */
        if (resp_cap < 8) return 0;
        memcpy(resp, req, 6);
        resp[0] = s->unit_id;
        crc_append(resp, 6);
        return 8;
    }

    case MB_FC_WRITE_MULTIPLE: {
        if (req_len < 11)
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_VALUE, resp, resp_cap);
        if (!s->write_reg)
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_cap);

        uint16_t addr  = mb_get_u16(req + 2);
        uint16_t qty   = mb_get_u16(req + 4);
        uint8_t  nbyte = req[6];

        /* Three fields have to agree, and a frame where they disagree is
         * either corrupt or hostile. Check all of them. */
        if (qty < 1 || qty > MODBUS_MAX_WRITE_REGS ||
            nbyte != qty * 2 || req_len != (size_t)(9 + nbyte))
            return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_VALUE, resp, resp_cap);

        for (uint16_t k = 0; k < qty; k++) {
            uint16_t v = mb_get_u16(req + 7 + k * 2);
            if (!s->write_reg((uint16_t)(addr + k), v, s->ctx))
                return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_cap);
        }
        if (broadcast) return 0;

        if (resp_cap < 8) return 0;
        resp[0] = s->unit_id;
        resp[1] = fc;
        mb_put_u16(resp + 2, addr);
        mb_put_u16(resp + 4, qty);
        crc_append(resp, 6);
        return 8;
    }

    default:
        return broadcast ? 0 : make_exception(s, fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_cap);
    }
}

/* =========================================================================
 * Master
 * =======================================================================*/

int modbus_master_build_read(uint8_t unit_id, uint8_t fc,
                             uint16_t addr, uint16_t qty,
                             uint8_t *out, size_t cap)
{
    if (cap < 8) return MB_ERR_ARG;
    if (qty < 1 || qty > MODBUS_MAX_READ_REGS) return MB_ERR_ARG;
    if (fc != MB_FC_READ_HOLDING && fc != MB_FC_READ_INPUT) return MB_ERR_ARG;

    out[0] = unit_id;
    out[1] = fc;
    mb_put_u16(out + 2, addr);
    mb_put_u16(out + 4, qty);
    crc_append(out, 6);
    return 8;
}

mb_result_t modbus_master_parse_read(const uint8_t *resp, size_t len,
                                     uint8_t unit_id, uint8_t fc, uint16_t qty,
                                     uint16_t *regs, uint8_t *exception_out)
{
    if (len < 5)              return MB_ERR_SHORT;
    if (!crc_ok(resp, len))   return MB_ERR_CRC;
    if (resp[0] != unit_id)   return MB_ERR_UNIT;

    if (resp[1] == (uint8_t)(fc | 0x80)) {
        if (exception_out) *exception_out = resp[2];
        return MB_ERR_EXCEPTION;
    }
    if (resp[1] != fc)        return MB_ERR_FC;

    if (resp[2] != qty * 2)   return MB_ERR_LEN;
    if (len != (size_t)(5 + qty * 2)) return MB_ERR_LEN;

    for (uint16_t k = 0; k < qty; k++)
        regs[k] = mb_get_u16(resp + 3 + k * 2);

    return MB_OK;
}

int modbus_master_build_write_single(uint8_t unit_id, uint16_t addr,
                                     uint16_t val, uint8_t *out, size_t cap)
{
    if (cap < 8) return MB_ERR_ARG;
    out[0] = unit_id;
    out[1] = MB_FC_WRITE_SINGLE;
    mb_put_u16(out + 2, addr);
    mb_put_u16(out + 4, val);
    crc_append(out, 6);
    return 8;
}

mb_result_t modbus_master_parse_write_single(const uint8_t *resp, size_t len,
                                             uint8_t unit_id, uint16_t addr,
                                             uint16_t val,
                                             uint8_t *exception_out)
{
    if (len < 5)            return MB_ERR_SHORT;
    if (!crc_ok(resp, len)) return MB_ERR_CRC;
    if (resp[0] != unit_id) return MB_ERR_UNIT;

    if (resp[1] == (uint8_t)(MB_FC_WRITE_SINGLE | 0x80)) {
        if (exception_out) *exception_out = resp[2];
        return MB_ERR_EXCEPTION;
    }
    if (resp[1] != MB_FC_WRITE_SINGLE) return MB_ERR_FC;
    if (len != 8)                      return MB_ERR_LEN;

    /* The echo must match what we asked for, or the slave and we disagree
     * about what just happened. */
    if (mb_get_u16(resp + 2) != addr || mb_get_u16(resp + 4) != val)
        return MB_ERR_LEN;

    return MB_OK;
}

const char *mb_result_str(mb_result_t r)
{
    switch (r) {
    case MB_OK:            return "ok";
    case MB_ERR_SHORT:     return "frame too short";
    case MB_ERR_CRC:       return "crc error";
    case MB_ERR_UNIT:      return "wrong unit id";
    case MB_ERR_FC:        return "function code mismatch";
    case MB_ERR_LEN:       return "length mismatch";
    case MB_ERR_EXCEPTION: return "modbus exception";
    case MB_ERR_ARG:       return "bad argument";
    }
    return "unknown";
}
