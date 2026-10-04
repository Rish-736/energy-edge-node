#include "meter_regs.h"
#include "modbus_rtu.h"
#include <string.h>
#include <math.h>

void meter_regs_init(meter_regs_t *r, uint16_t fw_version, bool simulated)
{
    memset(r, 0, sizeof(*r));
    r->data.fw_version = fw_version;
    r->data.simulated  = simulated;
}

/* Clamp before narrowing to an integer. Without this a sensor fault or a NaN
 * turns into a wild value in the scaled block, and a master that trusts the
 * scaled registers would act on nonsense. */
static uint16_t clamp_u16(float v)
{
    if (!isfinite(v) || v <= 0.0f) return 0;
    if (v >= 65535.0f) return 65535;
    return (uint16_t)(v + 0.5f);
}

static int16_t clamp_i16(float v)
{
    if (!isfinite(v)) return 0;
    if (v >=  32767.0f) return  32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static uint32_t clamp_u32(float v)
{
    if (!isfinite(v) || v <= 0.0f) return 0;
    if (v >= 4294967040.0f) return 0xFFFFFFFFu;
    return (uint32_t)v;
}

bool meter_regs_read(uint16_t addr, uint16_t *out, void *ctx)
{
    const meter_regs_t *r = (const meter_regs_t *)ctx;
    const meter_data_t *d = &r->data;

    /* The span starts at 0 and addr is unsigned, so only the upper bound
     * needs testing. */
    if (addr > MREG_SPAN_LAST)
        return false;                       /* -> ILLEGAL_ADDRESS */

    uint16_t pair[2];

    switch (addr) {

    /* identity */
    case MREG_VENDOR_ID:   *out = MREG_VENDOR_ID_VALUE;    return true;
    case MREG_PRODUCT_ID:  *out = MREG_PRODUCT_ID_VALUE;   return true;
    case MREG_FW_VERSION:  *out = d->fw_version;           return true;
    case MREG_MAP_VERSION: *out = MREG_MAP_VERSION_VALUE;  return true;
    case MREG_CAPABILITIES:
        *out = MCAP_FLOAT32 | MCAP_SCALED | MCAP_WRITABLE
             | (d->simulated ? MCAP_SIMULATED : 0);
        return true;

    /* float32 measurements -- each occupies this address and the next */
    case MREG_VOLTAGE_F32:     mb_put_f32(pair, d->v);  *out = pair[0]; return true;
    case MREG_VOLTAGE_F32 + 1: mb_put_f32(pair, d->v);  *out = pair[1]; return true;
    case MREG_CURRENT_F32:     mb_put_f32(pair, d->i);  *out = pair[0]; return true;
    case MREG_CURRENT_F32 + 1: mb_put_f32(pair, d->i);  *out = pair[1]; return true;
    case MREG_POWER_F32:       mb_put_f32(pair, d->p);  *out = pair[0]; return true;
    case MREG_POWER_F32 + 1:   mb_put_f32(pair, d->p);  *out = pair[1]; return true;
    case MREG_ENERGY_F32:      mb_put_f32(pair, d->wh); *out = pair[0]; return true;
    case MREG_ENERGY_F32 + 1:  mb_put_f32(pair, d->wh); *out = pair[1]; return true;

    /* scaled integers */
    case MREG_VOLTAGE_X100: *out = clamp_u16(d->v * 100.0f);          return true;
    case MREG_CURRENT_MA:   *out = (uint16_t)clamp_i16(d->i * 1000.0f); return true;
    case MREG_POWER_X10:    *out = (uint16_t)clamp_i16(d->p * 10.0f);   return true;
    case MREG_ENERGY_MWH:
        mb_put_u32(pair, clamp_u32(d->wh * 1000.0f)); *out = pair[0]; return true;
    case MREG_ENERGY_MWH + 1:
        mb_put_u32(pair, clamp_u32(d->wh * 1000.0f)); *out = pair[1]; return true;

    /* status */
    case MREG_STATUS_FLAGS: *out = d->status_flags; return true;
    case MREG_TRUTH_MASK:   *out = d->truth_mask;   return true;
    case MREG_SAMPLE_SEQ:     mb_put_u32(pair, d->sample_seq); *out = pair[0]; return true;
    case MREG_SAMPLE_SEQ + 1: mb_put_u32(pair, d->sample_seq); *out = pair[1]; return true;
    case MREG_UPTIME_S:       mb_put_u32(pair, d->uptime_s);   *out = pair[0]; return true;
    case MREG_UPTIME_S + 1:   mb_put_u32(pair, d->uptime_s);   *out = pair[1]; return true;
    case MREG_OVERRUNS:     *out = d->overruns;     return true;

    /* control -- readable so a master can confirm the command cleared */
    case MREG_CMD: *out = r->energy_reset_requested ? MREG_CMD_RESET_ENERGY : 0;
                   return true;

    default:
        /* Reserved but inside the mapped span: read as zero so a master can
         * fetch one contiguous block instead of several. */
        *out = 0;
        return true;
    }
}

bool meter_regs_write(uint16_t addr, uint16_t val, void *ctx)
{
    meter_regs_t *r = (meter_regs_t *)ctx;

    /* Only the command register is writable. Measurements are physical facts
     * and a master has no business overwriting them. */
    if (addr != MREG_CMD) return false;

    if (val == MREG_CMD_RESET_ENERGY) {
        r->energy_reset_requested = true;
        return true;
    }
    if (val == 0) {                 /* explicit clear */
        r->energy_reset_requested = false;
        return true;
    }
    return false;                   /* -> ILLEGAL_ADDRESS (unknown command) */
}

void meter_regs_decode_poll(const uint16_t *regs, meter_poll_t *out)
{
    /* regs[0] corresponds to MREG_POLL_BASE. */
    #define OFF(a) ((a) - MREG_POLL_BASE)

    out->v  = mb_get_f32(&regs[OFF(MREG_VOLTAGE_F32)]);
    out->i  = mb_get_f32(&regs[OFF(MREG_CURRENT_F32)]);
    out->p  = mb_get_f32(&regs[OFF(MREG_POWER_F32)]);
    out->wh = mb_get_f32(&regs[OFF(MREG_ENERGY_F32)]);

    out->status_flags = regs[OFF(MREG_STATUS_FLAGS)];
    out->truth_mask   = regs[OFF(MREG_TRUTH_MASK)];
    out->sample_seq   = mb_get_u32(&regs[OFF(MREG_SAMPLE_SEQ)]);
    out->uptime_s     = mb_get_u32(&regs[OFF(MREG_UPTIME_S)]);
    out->overruns     = regs[OFF(MREG_OVERRUNS)];

    #undef OFF
}
