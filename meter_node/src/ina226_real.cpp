#include "ina226_hal.h"
#ifndef USE_INA226_SIM

#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "telemetry.h"

/* INA226 register map (datasheet Table 2) */
#define REG_CONFIG      0x00
#define REG_SHUNT_V     0x01
#define REG_BUS_V       0x02
#define REG_POWER       0x03
#define REG_CURRENT     0x04
#define REG_CALIBRATION 0x05
#define REG_MANUF_ID    0xFE
#define INA226_MANUF_ID 0x5449   /* 'TI' */

static float s_current_lsb = 0.0f;

static bool reg_write(uint8_t reg, uint16_t val)
{
    Wire.beginTransmission(INA226_I2C_ADDR);
    Wire.write(reg);
    Wire.write((uint8_t)(val >> 8));
    Wire.write((uint8_t)(val & 0xFF));
    return Wire.endTransmission() == 0;
}

static bool reg_read(uint8_t reg, uint16_t *val)
{
    Wire.beginTransmission(INA226_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)INA226_I2C_ADDR, (uint8_t)2) != 2) return false;
    *val = ((uint16_t)Wire.read() << 8) | Wire.read();
    return true;
}

bool ina226_init(void)
{
    Wire.begin(INA226_SDA_PIN, INA226_SCL_PIN);
    Wire.setClock(400000);

    uint16_t id;
    if (!reg_read(REG_MANUF_ID, &id) || id != INA226_MANUF_ID) {
        Serial.printf("# INA226 not found (manuf id 0x%04X)\n", id);
        return false;
    }

    /* Averaging 16 samples, 1.1 ms conversion on both channels, continuous.
     * Averaging in the chip is free noise reduction and still leaves us an
     * update well under our 100 ms sample period. */
    if (!reg_write(REG_CONFIG, 0x4527)) return false;

    /* Calibration per datasheet 7.5:
     *   current_lsb = max_expected_current / 2^15
     *   cal         = 0.00512 / (current_lsb * r_shunt)          */
    s_current_lsb  = INA226_MAX_CURRENT_A / 32768.0f;
    uint16_t cal   = (uint16_t)(0.00512f / (s_current_lsb * INA226_SHUNT_OHMS));
    if (!reg_write(REG_CALIBRATION, cal)) return false;

    Serial.printf("# INA226 ready: current_lsb=%.6f A/bit cal=%u\n",
                  s_current_lsb, cal);
    return true;
}

bool ina226_read(ina226_reading_t *out)
{
    uint16_t raw_bus, raw_cur, raw_pwr;

    if (!reg_read(REG_BUS_V,  &raw_bus)) return false;
    if (!reg_read(REG_CURRENT,&raw_cur)) return false;
    if (!reg_read(REG_POWER,  &raw_pwr)) return false;

    out->bus_voltage_v = raw_bus * 1.25e-3f;                  /* 1.25 mV/bit */
    out->current_a     = (int16_t)raw_cur * s_current_lsb;    /* signed      */
    out->power_w       = raw_pwr * (s_current_lsb * 25.0f);   /* 25 * I_LSB  */
    return true;
}

bool    ina226_is_simulated(void) { return false; }
uint8_t ina226_truth_mask(void)   { return LOAD_NONE; }

#endif /* !USE_INA226_SIM */
