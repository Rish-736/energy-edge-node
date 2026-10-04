#include <unity.h>
#include <string.h>
#include "modbus_rtu.h"
#include "meter_regs.h"

/* ---------------------------------------------------------------------------
 * A master and a slave talking to each other through memory.
 *
 * `bus_exchange` IS the RS-485 cable: the master hands over a frame, the slave
 * hands one back. Because the protocol code knows nothing about UARTs, the
 * entire request/response cycle -- framing, CRC, exceptions, float packing,
 * block reads -- can be verified on a PC before any transceiver is wired up.
 * When the MAX485 modules arrive, only the transport underneath is new.
 * -------------------------------------------------------------------------*/

#define SLAVE_UNIT 1

static modbus_slave_t slave;
static meter_regs_t   regs;

static uint8_t req[MODBUS_MAX_FRAME];
static uint8_t resp[MODBUS_MAX_FRAME];

void setUp(void)
{
    meter_regs_init(&regs, 0x0100 /* v1.0 */, true /* simulated */);
    modbus_slave_init(&slave, SLAVE_UNIT,
                      meter_regs_read, meter_regs_write, &regs);
}
void tearDown(void) {}

static size_t bus_exchange(const uint8_t *tx, size_t tx_len,
                           uint8_t *rx, size_t rx_cap)
{
    return modbus_slave_handle(&slave, tx, tx_len, rx, rx_cap);
}

/* --- CRC ---------------------------------------------------------------- */

/* Standard property of CRC-16/MODBUS: running the checksum over a frame that
 * already has its CRC appended yields zero. That is how a receiver can verify
 * a frame in one pass, and it only holds if the algorithm and byte order are
 * both right. */
static void test_crc_of_frame_including_crc_is_zero(void)
{
    uint8_t f[8] = { 0x01, 0x03, 0x00, 0x10, 0x00, 0x08 };
    uint16_t crc = modbus_crc16(f, 6);
    f[6] = (uint8_t)(crc & 0xFF);     /* low byte first, per spec */
    f[7] = (uint8_t)(crc >> 8);

    TEST_ASSERT_EQUAL_UINT16(0, modbus_crc16(f, 8));
}

static void test_crc_detects_single_bit_flip(void)
{
    uint8_t a[6] = { 0x01, 0x03, 0x00, 0x10, 0x00, 0x08 };
    uint8_t b[6]; memcpy(b, a, 6);
    b[3] ^= 0x01;
    TEST_ASSERT_NOT_EQUAL(modbus_crc16(a, 6), modbus_crc16(b, 6));
}

/* --- float word order --------------------------------------------------- */

/* 1.0f is 0x3F800000. High word first means regs[0] == 0x3F80. If this test
 * ever fails, every float on the bus is garbage -- this is the single most
 * common Modbus integration bug, so it gets a known-answer test. */
static void test_float_word_order_is_high_word_first(void)
{
    uint16_t pair[2];
    mb_put_f32(pair, 1.0f);
    TEST_ASSERT_EQUAL_HEX16(0x3F80, pair[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0000, pair[1]);
}

static void test_float_round_trip(void)
{
    const float vals[] = { 0.0f, 1.0f, 12.345f, -3.5f, 68.17f, 1234.5f };
    for (unsigned k = 0; k < sizeof(vals) / sizeof(vals[0]); k++) {
        uint16_t pair[2];
        mb_put_f32(pair, vals[k]);
        TEST_ASSERT_EQUAL_FLOAT(vals[k], mb_get_f32(pair));
    }
}

static void test_u32_round_trip(void)
{
    uint16_t pair[2];
    mb_put_u32(pair, 0xDEADBEEFu);
    TEST_ASSERT_EQUAL_HEX16(0xDEAD, pair[0]);
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, mb_get_u32(pair));
}

/* --- identity ----------------------------------------------------------- */

static void test_read_identity_block(void)
{
    int n = modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_HOLDING,
                                     MREG_IDENTITY_BASE, MREG_IDENTITY_COUNT,
                                     req, sizeof(req));
    TEST_ASSERT_EQUAL_INT(8, n);

    size_t rn = bus_exchange(req, (size_t)n, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_UINT32(5 + MREG_IDENTITY_COUNT * 2, rn);

    uint16_t out[MREG_IDENTITY_COUNT];
    mb_result_t r = modbus_master_parse_read(resp, rn, SLAVE_UNIT,
                                             MB_FC_READ_HOLDING,
                                             MREG_IDENTITY_COUNT, out, NULL);
    TEST_ASSERT_EQUAL_INT(MB_OK, r);
    TEST_ASSERT_EQUAL_HEX16(MREG_VENDOR_ID_VALUE,  out[0]);
    TEST_ASSERT_EQUAL_HEX16(MREG_PRODUCT_ID_VALUE, out[1]);
    TEST_ASSERT_EQUAL_HEX16(0x0100,                out[2]);
    TEST_ASSERT_EQUAL_HEX16(MREG_MAP_VERSION_VALUE, out[3]);
    TEST_ASSERT_TRUE(out[4] & MCAP_SIMULATED);
    TEST_ASSERT_TRUE(out[4] & MCAP_FLOAT32);
}

/* --- the real poll path -------------------------------------------------- */

/* One request, one reply, every measurement recovered. This is exactly what
 * the gateway will do ten times a second. */
static void test_full_poll_block_round_trip(void)
{
    regs.data.v            = 11.873f;
    regs.data.i            = 5.3104f;
    regs.data.p            = 63.042f;
    regs.data.wh           = 1.328401f;
    regs.data.status_flags = 0x80;
    regs.data.truth_mask   = 0x06;
    regs.data.sample_seq   = 123456;
    regs.data.uptime_s     = 4242;
    regs.data.overruns     = 7;

    int n = modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_INPUT,
                                     MREG_POLL_BASE, MREG_POLL_COUNT,
                                     req, sizeof(req));
    TEST_ASSERT_EQUAL_INT(8, n);

    size_t rn = bus_exchange(req, (size_t)n, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_UINT32(5 + MREG_POLL_COUNT * 2, rn);

    uint16_t block[MREG_POLL_COUNT];
    mb_result_t r = modbus_master_parse_read(resp, rn, SLAVE_UNIT,
                                             MB_FC_READ_INPUT,
                                             MREG_POLL_COUNT, block, NULL);
    TEST_ASSERT_EQUAL_INT(MB_OK, r);

    meter_poll_t p;
    meter_regs_decode_poll(block, &p);

    TEST_ASSERT_EQUAL_FLOAT(11.873f,    p.v);
    TEST_ASSERT_EQUAL_FLOAT(5.3104f,    p.i);
    TEST_ASSERT_EQUAL_FLOAT(63.042f,    p.p);
    TEST_ASSERT_EQUAL_FLOAT(1.328401f,  p.wh);
    TEST_ASSERT_EQUAL_HEX16(0x80,       p.status_flags);
    TEST_ASSERT_EQUAL_HEX16(0x06,       p.truth_mask);
    TEST_ASSERT_EQUAL_UINT32(123456,    p.sample_seq);
    TEST_ASSERT_EQUAL_UINT32(4242,      p.uptime_s);
    TEST_ASSERT_EQUAL_UINT16(7,         p.overruns);
}

static void test_scaled_integer_block(void)
{
    regs.data.v  = 12.34f;
    regs.data.i  = -0.250f;
    regs.data.p  = 63.0f;
    regs.data.wh = 1.5f;

    int n = modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_INPUT,
                                     MREG_VOLTAGE_X100, 5, req, sizeof(req));
    size_t rn = bus_exchange(req, (size_t)n, resp, sizeof(resp));

    uint16_t out[5];
    TEST_ASSERT_EQUAL_INT(MB_OK,
        modbus_master_parse_read(resp, rn, SLAVE_UNIT, MB_FC_READ_INPUT,
                                 5, out, NULL));

    TEST_ASSERT_EQUAL_UINT16(1234, out[0]);                 /* 12.34 V * 100 */
    TEST_ASSERT_EQUAL_INT16(-250, (int16_t)out[1]);         /* -0.25 A -> mA  */
    TEST_ASSERT_EQUAL_INT16(630,  (int16_t)out[2]);         /* 63.0 W * 10    */
    TEST_ASSERT_EQUAL_UINT32(1500, mb_get_u32(&out[3]));    /* 1.5 Wh -> mWh  */
}

/* A sensor fault must not become a wild number on the bus. */
static void test_scaled_block_clamps_garbage(void)
{
    regs.data.v = 1.0e9f;
    regs.data.p = -1.0e9f;

    int n = modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_INPUT,
                                     MREG_VOLTAGE_X100, 3, req, sizeof(req));
    size_t rn = bus_exchange(req, (size_t)n, resp, sizeof(resp));

    uint16_t out[3];
    TEST_ASSERT_EQUAL_INT(MB_OK,
        modbus_master_parse_read(resp, rn, SLAVE_UNIT, MB_FC_READ_INPUT,
                                 3, out, NULL));
    TEST_ASSERT_EQUAL_UINT16(65535, out[0]);
    TEST_ASSERT_EQUAL_INT16(-32768, (int16_t)out[2]);
}

/* --- error handling ------------------------------------------------------ */

static void test_read_outside_map_raises_illegal_address(void)
{
    int n = modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_HOLDING,
                                     0x1000, 2, req, sizeof(req));
    size_t rn = bus_exchange(req, (size_t)n, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_UINT32(5, rn);

    uint16_t out[2];
    uint8_t ex = 0;
    mb_result_t r = modbus_master_parse_read(resp, rn, SLAVE_UNIT,
                                             MB_FC_READ_HOLDING, 2, out, &ex);
    TEST_ASSERT_EQUAL_INT(MB_ERR_EXCEPTION, r);
    TEST_ASSERT_EQUAL_HEX8(MB_EX_ILLEGAL_ADDRESS, ex);
    TEST_ASSERT_EQUAL_UINT32(1, slave.exceptions);
}

/* A corrupt frame gets no reply at all. Answering one would turn line noise
 * into bus traffic and make a bad bus worse. */
static void test_bad_crc_is_silently_dropped(void)
{
    int n = modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_HOLDING,
                                     MREG_POLL_BASE, 2, req, sizeof(req));
    req[3] ^= 0xFF;                       /* corrupt, leave CRC stale */

    TEST_ASSERT_EQUAL_UINT32(0, bus_exchange(req, (size_t)n, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT32(1, slave.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0, slave.requests);
}

static void test_frame_for_another_unit_is_ignored(void)
{
    int n = modbus_master_build_read(SLAVE_UNIT + 5, MB_FC_READ_HOLDING,
                                     MREG_POLL_BASE, 2, req, sizeof(req));
    TEST_ASSERT_EQUAL_UINT32(0, bus_exchange(req, (size_t)n, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT32(1, slave.ignored);
    TEST_ASSERT_EQUAL_UINT32(0, slave.requests);
}

static void test_unsupported_function_raises_illegal_function(void)
{
    req[0] = SLAVE_UNIT;
    req[1] = 0x2B;                        /* read device identification */
    uint16_t crc = modbus_crc16(req, 2);
    req[2] = (uint8_t)(crc & 0xFF);
    req[3] = (uint8_t)(crc >> 8);

    size_t rn = bus_exchange(req, 4, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_UINT32(5, rn);
    TEST_ASSERT_EQUAL_HEX8(0x2B | 0x80, resp[1]);
    TEST_ASSERT_EQUAL_HEX8(MB_EX_ILLEGAL_FUNCTION, resp[2]);
}

static void test_zero_and_oversize_quantity_rejected(void)
{
    /* Build by hand: the master helper refuses these, which is itself correct,
     * so we have to forge them to test the slave's own validation. */
    for (uint16_t qty = 0; qty <= MODBUS_MAX_READ_REGS + 1;
         qty += (uint16_t)(MODBUS_MAX_READ_REGS + 1)) {
        req[0] = SLAVE_UNIT;
        req[1] = MB_FC_READ_HOLDING;
        mb_put_u16(req + 2, MREG_POLL_BASE);
        mb_put_u16(req + 4, qty);
        uint16_t crc = modbus_crc16(req, 6);
        req[6] = (uint8_t)(crc & 0xFF);
        req[7] = (uint8_t)(crc >> 8);

        size_t rn = bus_exchange(req, 8, resp, sizeof(resp));
        TEST_ASSERT_EQUAL_UINT32(5, rn);
        TEST_ASSERT_EQUAL_HEX8(MB_EX_ILLEGAL_VALUE, resp[2]);
    }
}

static void test_master_helper_refuses_impossible_request(void)
{
    TEST_ASSERT_EQUAL_INT(MB_ERR_ARG,
        modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_HOLDING,
                                 0, 0, req, sizeof(req)));
    TEST_ASSERT_EQUAL_INT(MB_ERR_ARG,
        modbus_master_build_read(SLAVE_UNIT, MB_FC_READ_HOLDING,
                                 0, MODBUS_MAX_READ_REGS + 1, req, sizeof(req)));
}

/* --- writes ------------------------------------------------------------- */

static void test_write_command_register(void)
{
    int n = modbus_master_build_write_single(SLAVE_UNIT, MREG_CMD,
                                             MREG_CMD_RESET_ENERGY,
                                             req, sizeof(req));
    TEST_ASSERT_EQUAL_INT(8, n);

    size_t rn = bus_exchange(req, (size_t)n, resp, sizeof(resp));
    TEST_ASSERT_EQUAL_UINT32(8, rn);

    TEST_ASSERT_EQUAL_INT(MB_OK,
        modbus_master_parse_write_single(resp, rn, SLAVE_UNIT, MREG_CMD,
                                         MREG_CMD_RESET_ENERGY, NULL));
    TEST_ASSERT_TRUE(regs.energy_reset_requested);
}

/* Measurements are physical facts; a master must not be able to overwrite
 * them and then read back a lie. */
static void test_write_to_measurement_register_is_rejected(void)
{
    int n = modbus_master_build_write_single(SLAVE_UNIT, MREG_POWER_F32,
                                             0x1234, req, sizeof(req));
    size_t rn = bus_exchange(req, (size_t)n, resp, sizeof(resp));

    uint8_t ex = 0;
    TEST_ASSERT_EQUAL_INT(MB_ERR_EXCEPTION,
        modbus_master_parse_write_single(resp, rn, SLAVE_UNIT, MREG_POWER_F32,
                                         0x1234, &ex));
    TEST_ASSERT_EQUAL_HEX8(MB_EX_ILLEGAL_ADDRESS, ex);
}

/* Broadcast: the write takes effect, but nothing is sent back. Two slaves
 * replying at once would collide on a half-duplex bus. */
static void test_broadcast_write_applies_but_is_not_answered(void)
{
    int n = modbus_master_build_write_single(0, MREG_CMD,
                                             MREG_CMD_RESET_ENERGY,
                                             req, sizeof(req));
    TEST_ASSERT_EQUAL_UINT32(0, bus_exchange(req, (size_t)n, resp, sizeof(resp)));
    TEST_ASSERT_TRUE(regs.energy_reset_requested);
}

int main(int, char **)
{
    UNITY_BEGIN();

    RUN_TEST(test_crc_of_frame_including_crc_is_zero);
    RUN_TEST(test_crc_detects_single_bit_flip);

    RUN_TEST(test_float_word_order_is_high_word_first);
    RUN_TEST(test_float_round_trip);
    RUN_TEST(test_u32_round_trip);

    RUN_TEST(test_read_identity_block);
    RUN_TEST(test_full_poll_block_round_trip);
    RUN_TEST(test_scaled_integer_block);
    RUN_TEST(test_scaled_block_clamps_garbage);

    RUN_TEST(test_read_outside_map_raises_illegal_address);
    RUN_TEST(test_bad_crc_is_silently_dropped);
    RUN_TEST(test_frame_for_another_unit_is_ignored);
    RUN_TEST(test_unsupported_function_raises_illegal_function);
    RUN_TEST(test_zero_and_oversize_quantity_rejected);
    RUN_TEST(test_master_helper_refuses_impossible_request);

    RUN_TEST(test_write_command_register);
    RUN_TEST(test_write_to_measurement_register_is_rejected);
    RUN_TEST(test_broadcast_write_applies_but_is_not_answered);

    return UNITY_END();
}
