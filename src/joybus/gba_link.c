/**
 * @file gba_link.c
 * @author tfmoe__ <https://github.com/marian-m12l>
 * @brief GBA Link Cable Subsystem
 * @ingroup gba_link
 */

#include <stdlib.h>
#include <string.h>

#include "gba_link.h"
#include "joybus_commands.h"
#include "joypad.h"

/**
 * @addtogroup gba_link
 * @{
 */


bool gba_link_is_connected(int port)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    return joybus_get_identifier(port, NULL) == JOYBUS_IDENTIFIER_GBA_LINK_CABLE;
}

uint8_t gba_link_get_status(int port)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    uint8_t status;
    if (joybus_get_identifier(port, &status) != JOYBUS_IDENTIFIER_GBA_LINK_CABLE) {
        return 0;
    }
    return status;
}

/** 
 * @brief Async read command context
 */
typedef struct
{
    int port;
    gba_link_read_callback_t callback;
    void *ctx;
} gba_link_cable_read_context_t;

static void gba_link_read_callback(uint64_t *out_dwords, void *ctx)
{
    const uint8_t *out_bytes = (void *)out_dwords;
    gba_link_cable_read_context_t* context = (void*)ctx;
    joypad_port_t port = context->port;

    const joybus_cmd_gba_link_cable_read_port_t *cmdr =
        (void *)&out_bytes[port + JOYBUS_COMMAND_METADATA_SIZE];

    gba_link_io_status_t io_status = GBA_LINK_IO_STATUS_OK;
    // Check for joybus error (no device)
    if ((out_bytes[port+1] & 0x80) != 0) {
        io_status = GBA_LINK_IO_STATUS_ABSENT;
    }
    // Check GBA status
    if ((cmdr->recv.status & JOYBUS_IDENTIFY_STATUS_GBA_SENDING) == 0) {
        io_status = GBA_LINK_IO_STATUS_DATA_NOT_READY;
    }

    gba_link_read_callback_t callback = context->callback;
    if (callback != NULL) {
        // Command data may be misaligned, so we copy it to an aligned word
        uint32_t copy;
        memcpy(&copy, cmdr->recv.data, sizeof(cmdr->recv.data));
        callback(port, (uint8_t*)&copy, cmdr->recv.status, io_status, (void*)context->ctx);
    }
    free(context);
}

void gba_link_read_async(
    int port,
    gba_link_read_callback_t callback,
    void *ctx
)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    uint8_t input[JOYBUS_BLOCK_SIZE] = {0};
    joybus_cmd_gba_link_cable_read_port_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_GBA_LINK_CABLE_READ,
    } };
    size_t i = port;
    // Set the command metadata
    input[i++] = sizeof(cmd.send);
    input[i++] = sizeof(cmd.recv);
    // Micro-optimization: Minimize copy length
    const size_t recv_offset = offsetof(typeof(cmd), recv);
    memcpy(&input[i], &cmd.send, recv_offset);
    i += sizeof(cmd);
    // Close out the Joybus operation block
    input[i] = 0xFE;
    input[sizeof(input) - 1] = 0x01;
    // Execute the Joybus operation asynchronously
    gba_link_cable_read_context_t* gba_link_read_context = calloc(1, sizeof(gba_link_cable_read_context_t));
    gba_link_read_context->port = port;
    gba_link_read_context->callback = callback;
    gba_link_read_context->ctx = ctx;
    joybus_exec_async(input, gba_link_read_callback, (void*)gba_link_read_context);
}

/** 
 * @brief Async write command context
 */
typedef struct
{
    int port;
    gba_link_write_callback_t callback;
    void *ctx;
} gba_link_cable_write_context_t;

static void gba_link_write_callback(uint64_t *out_dwords, void *ctx)
{
    const uint8_t *out_bytes = (void *)out_dwords;
    gba_link_cable_write_context_t* context = (void*)ctx;
    joypad_port_t port = context->port;

    const joybus_cmd_gba_link_cable_write_port_t *cmdw =
        (void *)&out_bytes[port + JOYBUS_COMMAND_METADATA_SIZE];

    gba_link_io_status_t io_status = GBA_LINK_IO_STATUS_OK;
    // Check for joybus error (no device)
    if ((out_bytes[port+1] & 0x80) != 0) {
        io_status = GBA_LINK_IO_STATUS_ABSENT;
    }
    // Check GBA status
    if ((cmdw->recv.status & JOYBUS_IDENTIFY_STATUS_GBA_RECEIVING) == 0) {
        io_status = GBA_LINK_IO_STATUS_UNEXPECTED_STATUS;
    }

    gba_link_write_callback_t callback = context->callback;
    if (callback != NULL)
        callback(port, cmdw->recv.status, io_status, (void*)context->ctx);
    free(context);
}

void gba_link_write_async(
    int port,
    const uint8_t *data,
    gba_link_write_callback_t callback,
    void *ctx
)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    uint8_t input[JOYBUS_BLOCK_SIZE] = {0};
    joybus_cmd_gba_link_cable_write_port_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_GBA_LINK_CABLE_WRITE,
    } };
    size_t i = port;
    // Set the command metadata
    input[i++] = sizeof(cmd.send);
    input[i++] = sizeof(cmd.recv);
    // Micro-optimization: Minimize copy length
    const size_t data_offset = offsetof(typeof(cmd.send), data);
    memcpy(&input[i], &cmd.send, data_offset);
    memcpy(&input[i + data_offset], data, sizeof(cmd.send.data));
    i += sizeof(cmd);
    // Close out the Joybus operation block
    input[i] = 0xFE;
    input[sizeof(input) - 1] = 0x01;
    // Execute the Joybus operation asynchronously
    gba_link_cable_write_context_t* gba_link_write_context = calloc(1, sizeof(gba_link_cable_write_context_t));
    gba_link_write_context->port = port;
    gba_link_write_context->callback = callback;
    gba_link_write_context->ctx = ctx;
    joybus_exec_async(input, gba_link_write_callback, (void*)gba_link_write_context);
}

gba_link_io_status_t gba_link_read(int port, uint8_t *data, uint8_t* gba_status)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    joybus_cmd_gba_link_cable_read_port_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_GBA_LINK_CABLE_READ,
    } };
    // Execute the command
    joybus_exec_cmd_struct(port, cmd);
    // Copy the data from the command receive buffer
    memcpy(data, cmd.recv.data, sizeof(cmd.recv.data));
    *gba_status = cmd.recv.status;
    if ((*gba_status & JOYBUS_IDENTIFY_STATUS_GBA_SENDING) == 0) {
        return GBA_LINK_IO_STATUS_DATA_NOT_READY;
    }
    return GBA_LINK_IO_STATUS_OK;
}

gba_link_io_status_t gba_link_write(int port, const uint8_t *data, uint8_t* gba_status)
{
    assert((port >= 0) && (port < JOYPAD_PORT_COUNT));
    joybus_cmd_gba_link_cable_write_port_t cmd = { .send = {
        .command = JOYBUS_COMMAND_ID_GBA_LINK_CABLE_WRITE,
    } };
    // Copy the data to the command send buffer
    memcpy(cmd.send.data, data, sizeof(cmd.send.data));
    // Execute the command
    joybus_exec_cmd_struct(port, cmd);
    *gba_status = cmd.recv.status;
    if ((*gba_status & JOYBUS_IDENTIFY_STATUS_GBA_RECEIVING) == 0) {
        return GBA_LINK_IO_STATUS_UNEXPECTED_STATUS;
    }
    return GBA_LINK_IO_STATUS_OK;
}

/** @} */ /* gba_link */
