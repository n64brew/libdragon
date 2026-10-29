#include "../src/joybus/joybus_commands.h"

// Raw EEPROM block access through Joybus, bypassing the RAM cache of the
// eeprom module, so that tests can seed and check the actual hardware contents.

#define EEPROM_TEST_PORT 4

static void eeprom_test_wait_ready(void)
{
    joybus_cmd_identify_port_t cmd;
    do {
        cmd = (joybus_cmd_identify_port_t){ .send = {
            .command = JOYBUS_COMMAND_ID_IDENTIFY,
        } };
        joybus_exec_cmd_struct(EEPROM_TEST_PORT, cmd);
    } while (cmd.recv.status & JOYBUS_IDENTIFY_STATUS_EEPROM_BUSY);
}

static void eeprom_test_raw_read(uint8_t block, uint8_t *dest)
{
    eeprom_wait_idle();
    eeprom_test_wait_ready();
    joybus_cmd_eeprom_read_block_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_EEPROM_READ_BLOCK,
        .block = block,
    } };
    joybus_exec_cmd_struct(EEPROM_TEST_PORT, cmd);
    memcpy(dest, cmd.recv.data, EEPROM_BLOCK_SIZE);
}

static void eeprom_test_raw_write(uint8_t block, const uint8_t *src)
{
    eeprom_wait_idle();
    eeprom_test_wait_ready();
    joybus_cmd_eeprom_write_block_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_EEPROM_WRITE_BLOCK,
        .block = block,
    } };
    memcpy(cmd.send.data, src, EEPROM_BLOCK_SIZE);
    joybus_exec_cmd_struct(EEPROM_TEST_PORT, cmd);
    eeprom_test_wait_ready();
}

// Known non-zero contents for a block, different for each block.
static void eeprom_test_pattern(uint8_t block, uint8_t *buf)
{
    for (int i = 0; i < EEPROM_BLOCK_SIZE; i++)
        buf[i] = 0x80 | ((block & 0xF) << 3) | i;
}

// Byte spans to test, as offset and length from the start of a block. Each
// case uses its own blocks, not touched before by any test, so that they are
// not in the eeprom module cache yet.
static const struct {
    uint8_t offset; // offset of the span within its first block
    uint8_t len;    // length of the span in bytes
} eeprom_test_spans[] = {
    { 5, 1 },   // starts and ends inside one block
    { 1, 3 },   // starts and ends inside one block
    { 0, 3 },   // starts at a block boundary, ends inside the same block
    { 3, 5 },   // starts inside a block, ends at its end
    { 6, 4 },   // crosses a block boundary, partial on both sides
    { 0, 11 },  // starts at a block boundary, partial last block
    { 0, 8 },   // exactly one block
};

void test_eeprom_read_bytes(TestContext *ctx) {
    if (eeprom_present() == EEPROM_NONE) {
        SKIP("EEPROM not found; skipping eeprom tests");
    }

    for (int t = 0; t < sizeof(eeprom_test_spans) / sizeof(eeprom_test_spans[0]); t++) {
        // Two blocks per case, from block 40 (4 Kbit EEPROMs have 64)
        const uint8_t block = 40 + t * 2;
        const size_t start = block * EEPROM_BLOCK_SIZE + eeprom_test_spans[t].offset;
        const size_t span_len = eeprom_test_spans[t].len;
        LOG("read_bytes(start=%d, len=%d)\n", (int)start, (int)span_len);

        // Seed the hardware with known contents
        uint8_t expected[EEPROM_BLOCK_SIZE * 2];
        eeprom_test_pattern(block, &expected[0]);
        eeprom_test_pattern(block + 1, &expected[EEPROM_BLOCK_SIZE]);
        eeprom_test_raw_write(block, &expected[0]);
        eeprom_test_raw_write(block + 1, &expected[EEPROM_BLOCK_SIZE]);

        // Read into the middle of a guarded buffer
        uint8_t buf[32];
        memset(buf, 0xEE, sizeof(buf));
        eeprom_read_bytes(&buf[8], start, span_len);

        uint8_t guard[8];
        memset(guard, 0xEE, sizeof(guard));
        ASSERT_EQUAL_MEM(&buf[8], &expected[eeprom_test_spans[t].offset], span_len,
            "read_bytes(%d, %d) returned wrong data", (int)start, (int)span_len);
        ASSERT_EQUAL_MEM(&buf[0], guard, 8,
            "read_bytes(%d, %d) wrote before the destination", (int)start, (int)span_len);
        ASSERT_EQUAL_MEM(&buf[8 + span_len], guard, 8,
            "read_bytes(%d, %d) wrote past the destination", (int)start, (int)span_len);
    }
}

void test_eeprom_write_bytes(TestContext *ctx) {
    if (eeprom_present() == EEPROM_NONE) {
        SKIP("EEPROM not found; skipping eeprom tests");
    }

    for (int t = 0; t < sizeof(eeprom_test_spans) / sizeof(eeprom_test_spans[0]); t++) {
        // Four blocks per case (the span's two and one on each side), from
        // block 8: different blocks than the read test, which cached its own
        const uint8_t block = 8 + t * 4 + 1;
        const size_t start = block * EEPROM_BLOCK_SIZE + eeprom_test_spans[t].offset;
        const size_t span_len = eeprom_test_spans[t].len;
        LOG("write_bytes(start=%d, len=%d)\n", (int)start, (int)span_len);

        // Seed the hardware with known contents (and one block on each side)
        uint8_t expected[EEPROM_BLOCK_SIZE * 4];
        for (int b = 0; b < 4; b++) {
            eeprom_test_pattern(block - 1 + b, &expected[b * EEPROM_BLOCK_SIZE]);
            eeprom_test_raw_write(block - 1 + b, &expected[b * EEPROM_BLOCK_SIZE]);
        }

        // Write the span, and wait for it to reach the hardware
        uint8_t src[16];
        for (int i = 0; i < sizeof(src); i++)
            src[i] = 0x10 + i;
        eeprom_write_bytes(src, start, span_len);
        eeprom_wait_idle();
        memcpy(&expected[EEPROM_BLOCK_SIZE + eeprom_test_spans[t].offset], src, span_len);

        // Only the bytes of the span must have changed in the hardware
        uint8_t actual[EEPROM_BLOCK_SIZE * 4];
        for (int b = 0; b < 4; b++)
            eeprom_test_raw_read(block - 1 + b, &actual[b * EEPROM_BLOCK_SIZE]);
        ASSERT_EQUAL_MEM(actual, expected, sizeof(expected),
            "write_bytes(%d, %d) changed the wrong bytes", (int)start, (int)span_len);

        // And the cache must agree with the hardware
        uint8_t cached[EEPROM_BLOCK_SIZE * 4];
        eeprom_read_bytes(cached, (block - 1) * EEPROM_BLOCK_SIZE, sizeof(cached));
        ASSERT_EQUAL_MEM(cached, expected, sizeof(expected),
            "write_bytes(%d, %d) left a stale cache", (int)start, (int)span_len);
    }
}
