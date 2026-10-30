/**
 * @file gba_link_multiboot.c
 * @author tfmoe__ <https://github.com/marian-m12l>
 * @brief GBA Link Cable support for Multi Boot
 * @ingroup gba_link
 */

#include <stdio.h>
#include <string.h>

#include "asset.h"
#include "interrupt.h"
#include "gba_link.h"
#include "gba_link_multiboot.h"
#include "joybus_commands.h"
#include "joypad.h"
#include "timer.h"

/**
 * @addtogroup gba_link
 * @{
 */


/** @brief Delay between two attempts to detect GBA Link ready for Multi Boot */
#define IDENTIFY_RETRY_DELAY_US TIMER_TICKS(5000)

/** @brief Payload length must be rounded up to the nearest 8-byte boundary */
#define ROUND_UP_8(val) (((val) + 7) & ~7)

// Keys extracted from secret string " by Kawasedo"
/** @brief Key: " by " */
#define MAGIC_BY 0x20627920
/** @brief Key: "Kawa" */
#define MAGIC_KAWA 0x4b617761
/** @brief Key: "sedo" */
#define MAGIC_SEDO 0x7365646f

/** @brief Calculate key B from payload length and logo settings */
static uint32_t calc_key(uint32_t length, gba_link_multiboot_logo_speed_t speed, gba_link_multiboot_logo_color_t color) {
    // Compute payload length in 8-byte blocks, past the minimum 512 bytes
	length = (length - 0x200) >> 3;
	// Compute logo settings
    uint32_t logo = speed < 0
		? (color << 3 | (3 - speed))
		: (speed > 0
			? (color << 3 | (speed - 1))
			: (color | 0x38)
		);
    // Mangle payload length
    uint32_t mangled = (length & 0x4000) << 2 | (length & 0x3f80) << 1 | (length & 0x7f);
    // Combine mangled length with logo settings
    uint32_t key = (logo << 17) | mangled;
    // Checksum
    uint8_t checksum = ((key >> 16) + (key >> 8) + key) & 0x7f;
    // Combine key with checksum and fixed bits
    key = (checksum << 24) | 0x80808080 | key;
    // XOR with the correct key
    uint32_t xorkey = (key & 0x200) ? MAGIC_SEDO : MAGIC_KAWA;
    return __builtin_bswap32(key) ^ xorkey;
}

/** @brief Update the running CRC with latest payload word */
static uint32_t update_crc(uint32_t crc, uint32_t val) {
    for (int i = 0; i < 0x20; i++) {
        if ((crc ^ val) & 1) {
			crc >>= 1;
			crc ^= 0xa1c1;
        } else {
			crc >>= 1;
        }
		val >>= 1;
	}
	return crc;
}

/** @brief Cipher payload word and update session key */
static uint32_t cipher_data(uint32_t data, int i, uint32_t* sessionkey) {
    *sessionkey = (*sessionkey * __builtin_bswap32(MAGIC_KAWA)) + 1;
    data ^= *sessionkey;
    data ^= ((~(i + (0x20<<20))) + 1);
    data ^= __builtin_bswap32(MAGIC_BY);
    return data;
}


/** @brief Internal state for Multi Boot */
typedef enum {
    GBA_MULTIBOOT_IDLE = 0,
    GBA_MULTIBOOT_CLEARTEXT,
    GBA_MULTIBOOT_CIPHERTEXT,
    GBA_MULTIBOOT_CRC,
    GBA_MULTIBOOT_CHECK,
    GBA_MULTIBOOT_DONE,
} multiboot_state_t;

/** 
 * @brief Multi Boot context
 */
typedef struct
{
    FILE* file;
    int length;
    gba_link_multiboot_logo_color_t logo_color;
    gba_link_multiboot_logo_speed_t logo_speed;
    multiboot_state_t state;
    uint32_t crc;
    uint32_t dummy[8];
    uint32_t sessionkey;
    int sent;
    gba_link_multiboot_callback_t callback;
    void *ctx;
} multiboot_context_t;

/** @brief Per-port Multi Boot contexts */
static multiboot_context_t multiboot_contexts[JOYPAD_PORT_COUNT];

/** @brief Terminate Multi Boot, execute callback with status, close file and clear context */
static void terminate_multiboot(int port, gba_link_multiboot_status_t status, multiboot_context_t* context) {
    gba_link_multiboot_callback_t callback = context->callback;
    if (callback != NULL)
        callback(port, status, (void*)context->ctx);
    fclose(context->file);
    memset(context, 0, sizeof(multiboot_context_t));
}

static void gba_link_write_callback(int port, uint8_t gba_status, gba_link_io_status_t io_status, void *ctx);

static void gba_link_read_callback(int port, const uint8_t data[JOYBUS_GBA_LINK_CABLE_DATA_SIZE], uint8_t gba_status, gba_link_io_status_t io_status, void *ctx) {
    multiboot_context_t* context = (void*) ctx;
    // Handle I/O errors
    if (io_status != GBA_LINK_IO_STATUS_OK) {
        terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_IO, context);
        return;
    }
    switch (context->state) {
        case GBA_MULTIBOOT_IDLE: {
            // Check GBA status
            if (gba_status != (JOYBUS_IDENTIFY_STATUS_GBA_GPIO0 | JOYBUS_IDENTIFY_STATUS_GBA_SENDING)) {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_STATUS, context);
                return; 
            }
            // Compute session key from received data
            context->sessionkey = *(uint32_t*)data;
            context->sessionkey = context->sessionkey ^ MAGIC_SEDO;
            context->sessionkey = __builtin_bswap32(context->sessionkey);
            // Compute host key from payload length and palette configuration
            uint32_t calc = calc_key(ROUND_UP_8(context->length), context->logo_speed, context->logo_color);
            context->crc = 0x15a0;
            context->sent = 0;
            context->state = GBA_MULTIBOOT_CLEARTEXT;
            gba_link_write_async(port, (uint8_t*) &calc, gba_link_write_callback, ctx);
            break;
        }
        case GBA_MULTIBOOT_CLEARTEXT:
        case GBA_MULTIBOOT_CIPHERTEXT:
        case GBA_MULTIBOOT_CRC:
            break;
        case GBA_MULTIBOOT_CHECK: {
            uint32_t d = __builtin_bswap32(*(uint32_t*)data);
            if (d != context->dummy[4] && d != context->dummy[5] && d != context->dummy[6]) {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_DUMMY_BYTE, context);
                return;
            }
            context->state = GBA_MULTIBOOT_DONE;
            terminate_multiboot(port, GBA_LINK_MULTIBOOT_OK, context);
            break;
        }
        case GBA_MULTIBOOT_DONE: {
            break;
        }
    }
}

static void gba_link_write_callback(int port, uint8_t gba_status, gba_link_io_status_t io_status, void *ctx) {
    multiboot_context_t* context = (void*) ctx;
    // Handle I/O errors
    if (io_status != GBA_LINK_IO_STATUS_OK) {
        terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_IO, context);
        return;
    }
    switch (context->state) {
        case GBA_MULTIBOOT_IDLE:
            break;
        case GBA_MULTIBOOT_CLEARTEXT: {
            // Check GBA status
            if (context->sent > 0 && !(gba_status & JOYBUS_IDENTIFY_STATUS_GBA_GPIO1)) {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_STATUS, context);
                return; 
            }
            if ((gba_status & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0) != (~(context->sent<<2) & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0)) {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_STATUS, context);
                return; 
            }
            uint8_t buf[JOYBUS_GBA_LINK_CABLE_DATA_SIZE];
            size_t nread;
            if ((nread = fread(buf, sizeof(buf[0]), sizeof(buf), context->file)) != 0) {
                gba_link_write_async(port, buf, gba_link_write_callback, ctx);
                context->sent += 4;
                if (context->sent == 0xc0) {
                    context->state = GBA_MULTIBOOT_CIPHERTEXT;
                }
            } else {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_FILE, context);
                return;
            }
            break;
        }
        case GBA_MULTIBOOT_CIPHERTEXT: {
            if ((gba_status & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0) != (~(context->sent<<2) & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0)) {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_STATUS, context);
                return; 
            }
            // ROM data must be ciphered for the remaining of the payload
            // CRC is updated for each packet sent
            uint32_t d = 0; // If rounded-length is larger than file length, pad with zeroes
            if (context->sent < context->length) {
                uint8_t buf[JOYBUS_GBA_LINK_CABLE_DATA_SIZE];
                size_t nread;
                if ((nread = fread(buf, sizeof(buf[0]), sizeof(buf), context->file)) != 0) {
                    d = __builtin_bswap32(*(uint32_t*)buf);
                } else {
                    terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_FILE, context);
                    return;
                }
            }
            uint32_t cipherText = __builtin_bswap32(cipher_data(d, context->sent, &context->sessionkey));
            context->crc = update_crc(context->crc, d);
            // Collect dummy bytes to check against at the end of Multi Boot
            if (context->sent == 0x1f8) {
                context->dummy[0] = d;
                context->dummy[2] = __builtin_bswap32(cipherText);
            } else if (context->sent == 0x1fc) {
                context->dummy[1] = d;
                context->dummy[3] = __builtin_bswap32(cipherText);
                context->dummy[4] = context->dummy[2] * context->dummy[3];
                context->dummy[5] = context->dummy[0] * context->dummy[1];
                context->dummy[6] = context->dummy[0] * context->dummy[3];
            }
            gba_link_write_async(port, (uint8_t*) &cipherText, gba_link_write_callback, ctx);
            context->sent += 4;
            if (context->sent == ROUND_UP_8(context->length)) {
                context->state = GBA_MULTIBOOT_CRC;
            }
            break;
        }
        case GBA_MULTIBOOT_CRC: {
            if ((gba_status & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0) != (~(context->sent<<2) & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0)) {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_STATUS, context);
                return; 
            }
            // Finalize and send CRC
            context->crc |= (ROUND_UP_8(context->length) << 16);
            context->crc = __builtin_bswap32(cipher_data(context->crc, context->sent, &context->sessionkey));
            gba_link_write_async(port, (uint8_t*) &context->crc, gba_link_write_callback, ctx);
            context->state = GBA_MULTIBOOT_CHECK;
            break;
        }
        case GBA_MULTIBOOT_CHECK: {
            if ((gba_status & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0) != (~((context->sent+4)<<2) & JOYBUS_IDENTIFY_STATUS_GBA_GPIO0)) {
                terminate_multiboot(port, GBA_LINK_MULTIBOOT_BAD_STATUS, context);
                return; 
            }
            gba_link_read_async(port, gba_link_read_callback, ctx);
            break;
        }
        case GBA_MULTIBOOT_DONE:
            break;
    }
}

void gba_link_multiboot_start(
    int port,
    const char *file_name,
    gba_link_multiboot_logo_color_t logo_color,
    gba_link_multiboot_logo_speed_t logo_speed,
    gba_link_multiboot_callback_t callback,
    void *ctx
)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    memset(&multiboot_contexts[port], 0, sizeof(multiboot_context_t));
    multiboot_contexts[port].callback = callback;
    multiboot_contexts[port].ctx = ctx;
    multiboot_contexts[port].file = asset_fopen(file_name, &multiboot_contexts[port].length);
    multiboot_contexts[port].logo_color = logo_color;
    multiboot_contexts[port].logo_speed = logo_speed;
    gba_link_read_async(port, gba_link_read_callback, (void*)&multiboot_contexts[port]);
}


/** 
 * @brief Async detection context
 */
typedef struct
{
    int port;
    bool pending;
    uint8_t progress;
    gba_link_multiboot_detect_callback_t callback;
    timer_link_t *retry_timer;
    timer_link_t *abort_timer;
    void *ctx;
} gba_link_multiboot_detect_context_t;

/** @brief Per-port detection contexts */
static gba_link_multiboot_detect_context_t gba_link_multiboot_detect_contexts[JOYPAD_PORT_COUNT];

static void gba_link_multiboot_identify_callback(uint64_t *out_dwords, void *ctx);

static void queue_joybus_input_reset_async(gba_link_multiboot_detect_context_t* context) {
    joybus_cmd_reset_port_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_RESET,
    } };
    // Allocate the Joybus operation block input and output buffers
    uint8_t input[JOYBUS_BLOCK_SIZE] = {0};
    // Skip commands on ports before the desired port offset
    size_t i = context->port;
    // Set the command metadata
    input[i++] = sizeof(cmd.send);
    input[i++] = sizeof(cmd.recv);
    // Copy the send_data into the input buffer
    memcpy( &input[i], (void *)&cmd.send, sizeof(cmd.send) );
    i += sizeof(cmd.send) + sizeof(cmd.recv);
    // Close out the Joybus operation block
    input[i] = 0xFE;
    input[sizeof(input) - 1] = 0x01;
    joybus_exec_async(input, NULL, NULL);
}

static void queue_joybus_input_identify_async(gba_link_multiboot_detect_context_t* context) {
    joybus_cmd_identify_port_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_IDENTIFY,
    } };
    // Allocate the Joybus operation block input and output buffers
    uint8_t input[JOYBUS_BLOCK_SIZE] = {0};
    // Skip commands on ports before the desired port offset
    size_t i = context->port;
    // Set the command metadata
    input[i++] = sizeof(cmd.send);
    input[i++] = sizeof(cmd.recv);
    // Copy the send_data into the input buffer
    memcpy( &input[i], (void *)&cmd.send, sizeof(cmd.send) );
    i += sizeof(cmd.send) + sizeof(cmd.recv);
    // Close out the Joybus operation block
    input[i] = 0xFE;
    input[sizeof(input) - 1] = 0x01;
    joybus_exec_async(input, gba_link_multiboot_identify_callback, (void*)context);
}

static void retry_identify(int ovfl, void *ctx) {
    gba_link_multiboot_detect_context_t* context = (void*)ctx;
    delete_timer(context->retry_timer);
    timer_close();
    context->retry_timer = NULL;
    queue_joybus_input_reset_async(context);
    queue_joybus_input_identify_async(context);
}

static void abort_identify(int ovfl, void *ctx) {
    gba_link_multiboot_detect_context_t* context = (void*)ctx;
    delete_timer(context->abort_timer);
    timer_close();
    context->abort_timer = NULL;
    context->pending = false;
}

static void gba_link_multiboot_identify_callback(uint64_t *out_dwords, void *ctx) {
    const uint8_t *out_bytes = (void *)out_dwords;
    gba_link_multiboot_detect_context_t* context = (void*)ctx;
    size_t i = context->port;
    const joybus_cmd_identify_port_t *cmd = (void *)&out_bytes[i + JOYBUS_COMMAND_METADATA_SIZE];
    bool multiboot_ready = false;
    if ((out_bytes[i+1] & 0x80) == 0
        && cmd->recv.identifier == JOYBUS_IDENTIFIER_GBA_LINK_CABLE
        && cmd->recv.status == (JOYBUS_IDENTIFY_STATUS_GBA_GPIO0 | JOYBUS_IDENTIFY_STATUS_GBA_SENDING)
    ) {
        multiboot_ready = true;
        context->pending = false;
    }

    if (!context->pending) {
        if (context->abort_timer != NULL) {
            delete_timer(context->abort_timer);
            timer_close();
        }
        gba_link_multiboot_detect_callback_t callback = context->callback;
        if (callback != NULL)
            callback(context->port, multiboot_ready, (void*)context->ctx);
        memset(context, 0, sizeof(gba_link_multiboot_detect_context_t));
    } else {
        timer_init();
        context->retry_timer = new_timer_context(IDENTIFY_RETRY_DELAY_US, TF_ONE_SHOT, retry_identify, context);
    }
}

void gba_link_multiboot_detect(int port, int timeout, gba_link_multiboot_detect_callback_t callback, void *ctx)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    // Disabled during reset
    if( exception_reset_time() > 0 ) { return; }
    // Already in-progress
    if (gba_link_multiboot_detect_contexts[port].pending) { return; }

    memset(&gba_link_multiboot_detect_contexts[port], 0, sizeof(gba_link_multiboot_detect_context_t));
    gba_link_multiboot_detect_contexts[port].port = port;
    gba_link_multiboot_detect_contexts[port].pending = true;
    gba_link_multiboot_detect_contexts[port].progress = 0;
    gba_link_multiboot_detect_contexts[port].callback = callback;
    gba_link_multiboot_detect_contexts[port].retry_timer = NULL;
    gba_link_multiboot_detect_contexts[port].ctx = ctx;
    // Set abort timer
    timer_init();
    gba_link_multiboot_detect_contexts[port].abort_timer = new_timer_context(TIMER_TICKS(timeout * 1000), TF_ONE_SHOT, abort_identify, (void*)&gba_link_multiboot_detect_contexts[port]);
    queue_joybus_input_reset_async(&gba_link_multiboot_detect_contexts[port]);
    queue_joybus_input_identify_async(&gba_link_multiboot_detect_contexts[port]);
}

uint8_t gba_link_multiboot_progress(int port)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    if (multiboot_contexts[port].state == GBA_MULTIBOOT_IDLE) {
        return 0;
    }
    return (100 * multiboot_contexts[port].sent) / multiboot_contexts[port].length;
}


/** @} */ /* gba_link */
