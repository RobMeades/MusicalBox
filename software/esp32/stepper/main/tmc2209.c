/*
 * Copyright 2026 Rob Meades
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** @file
 * @brief Implementation of the TMC2209 stepper motor driver API for the
 * ESP32-based stepper motor driver.  The code here is at least partly
 * inspired by the code here:
 *
 * https://github.com/janelia-arduino/TMC2209
 */

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "errno.h"
#include "driver/uart.h"
#include "string.h"

#include "tmc2209.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

// Logging prefix
#define TAG "tmc2209"

// UART buffer size
#define UART_RX_BUFFFER_SIZE 256

// The contents of the sync and reserved fields of a datagram
#define DATAGRAM_SYNC_AND_RESERVED 0x05

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// The UART we are using, -1 if not set.
static int32_t g_uart = -1;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Print out a datagram
static void print_datagram(const char *prefix, uint8_t *p, int32_t length)
{
    char buffer[(sizeof(uint64_t) * 2) + 1] = {0};

    for (int32_t x = 0; (x < length) && (x * 2 < sizeof(buffer)); x++) {
        sprintf(&(buffer[x * 2]), "%02x", *(p + x));
    }
    ESP_LOGI(TAG, "%s0x%s (%d byte(s)).", prefix, buffer, length);
}

 // Clean-up on error or completion
static void cleanup()
{
    if (g_uart >= 0) {
        uart_driver_delete((uart_port_t) g_uart);
        g_uart = -1;
    }
}

// Return true if this processor is little-endian, i.e. stores
// the least significant byte at the lowest memory address,
// so the value 0x12345678 read back as an array would
// be 0x12345678, as opposed to 0x78563412 for big-endian.
static bool is_little_endian()
{
    int32_t x = 1;

    return (*((char *) (&x)) == 1);
}

// Convert a buffer of data into a uint32_t for transmission.
static uint32_t endian_convert(uint32_t data)
{
    uint32_t output = data;

    // Transmission order is data bytes 3, 2, 1, 0,
    // so big-endian: reverse if we are little-endian
    if (is_little_endian()) {
        output = 0; 
        for (uint32_t x = 0; x < sizeof(data); x++) {
            output |= ((uint32_t) *(((uint8_t *) &data) + x)) << (24 - (8 * x));
        }
    }

    return output;
}

// Calculate the CRC on a datagram
static uint8_t calculate_crc(uint8_t *p, size_t length)
{
    uint8_t crc = 0;
    uint8_t byte;

    for (uint8_t x = 0; x < length; x++) {
        byte = *p;
        for (uint8_t y = 0; y < 8; y++) {
            if ((crc >> 7) ^ (byte & 0x01)) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc = (crc << 1);
            }
            byte = byte >> 1;
        }
        p++;
    }

    return crc;
}

// Write to a TMC2209.  data may be NULL, in which case
// a read request is sent.  See sections 4.1.1 and 4.1.2
// of the tMC2209n data sheet:
// https://www.analog.com/media/en/technical-documentation/data-sheets/TMC2209_datasheet_rev1.09.pdf
static esp_err_t write(int32_t address, int32_t reg, uint32_t *data)
{
    // Return the number of datagram bytes sent or negative error code from esp_err_t
    esp_err_t err = -ESP_ERR_INVALID_ARG;
    uint64_t datagram = 0;

    ESP_LOGI(TAG, "Sending %d byte(s) of data %sto address %d, register 0x%02x.",
             (data != NULL ? sizeof(*data) : 0),
             (data == NULL ? "(read access request) " : ""), address, reg);
    if ((g_uart >= 0) && (address < 4) && (reg < 128)) {
        uint8_t *p = (uint8_t *) &datagram;
        size_t index = 0;
        *(p + index) = DATAGRAM_SYNC_AND_RESERVED;
        index++;
        *(p + index) = address;
        index++;
        if (data != NULL) {
            *(p + index) = reg | 0x80; // Set top bit if there is data to write
        }
        index++;
        if (data != NULL) {
            uint32_t buffer = endian_convert(*data);
            memcpy ((p + index), &buffer, sizeof(buffer));
            index += sizeof(buffer);
        }
        *(p + index) = calculate_crc(p, index);
        index++;
        print_datagram("Send datagram will be ", p, index);
        // This function return the number of bytes sent or -1
        err = uart_write_bytes((uart_port_t) g_uart, p, index);
        if (err == index) {
            // Return the number of _data_ bytes sent
            err = (data != NULL) ? sizeof(*data) : 0;
            // Since the Tx and Rx pins are tied together, we will get
            // back what we sent: read it out now to get it out of
            // the way
            uint64_t read = 0;
            p = (uint8_t *) &read;
            int32_t read_count = uart_read_bytes((uart_port_t) g_uart, p,
                                                  index,
                                                  pdMS_TO_TICKS(100)); 
            if (read_count > 0) {
                if (memcmp(&read, &datagram, index != 0)) {
                    print_datagram("Expected to read back what we sent but"
                                   " instead read ", p, read_count);
                }
            } else {
                ESP_LOGW(TAG, "Expected to read back what we sent but"
                         " read returned %d.", err);
            }
        } else {
            if (err >= 0) {
                ESP_LOGE(TAG, "%d byte(s) (of %d) sent.", err, index);
            } else {
                ESP_LOGE(TAG, "UART write returned %d.", err);
            }
       }
    } else {
        ESP_LOGE(TAG, "Not initialised or address or register or buffer"
                 " length out of range.");
    }

    return err;
}

// Read from a TMC2209.  See section 4.1.2 of the tMC2209n data sheet:
// https://www.analog.com/media/en/technical-documentation/data-sheets/TMC2209_datasheet_rev1.09.pdf
static esp_err_t read(int32_t address, int32_t reg, uint32_t *data)
{
    uint64_t datagram = 0;

    // Send a read request, which is a write with no data
    esp_err_t err = write(address, reg, NULL);
    if (err == 0) {
        uint8_t *p = (uint8_t *) &datagram;
        err = uart_read_bytes((uart_port_t) g_uart, p, sizeof(datagram),
                              pdMS_TO_TICKS(1000));
        if (err == sizeof(datagram)) {
            print_datagram("Read datagram was ", p, err);
            // Check CRC
            uint8_t crc = calculate_crc(p, sizeof(datagram) - 1);
            if (crc == *(p + sizeof(datagram) - 1)) {
                // Return the number of data bytes received
                err = sizeof(uint32_t);
                if (data) {
                    uint32_t output = 0;
                    // Data starts in the fourth byte
                    memcpy(&output, (p + 3), sizeof(output));
                    *data = endian_convert(output);
                }
            } else {
                err = ESP_ERR_INVALID_CRC;
                ESP_LOGE(TAG, "CRC failure: expected 0x%02x, got 0x%02x).",
                         crc, *(p + sizeof(datagram) - 1));
            }
        } else {
            if (err > 0) {
                print_datagram("Expected 8 bytes, got ", p, err);
            } else {
                ESP_LOGE(TAG, "UART read returned %d.", err);
            }
        }
    }

    return err;
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialise the interface to the TMC2209.
esp_err_t tmc2209_init(int32_t uart, int32_t pin_txd, int32_t pin_rxd,
                       int32_t baud)
{
    // UART configuration
    uart_config_t uart_config = {
        // baud_rate is populated below
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
 
    ESP_LOGI(TAG, "Installing TMC2209 driver on UART %d, TXD pin %d,"
             " RXD pin %d, baud rate %d.", uart, pin_txd, pin_rxd, baud);
    // Configure the UART that talks to the stepper motor driver
    esp_err_t err = uart_driver_install(uart, UART_RX_BUFFFER_SIZE * 2, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        g_uart = uart;
        uart_config.baud_rate = baud;
        err = uart_param_config(uart, &uart_config);
        if (err == ESP_OK) {
            err = uart_set_pin(uart, pin_txd, pin_rxd, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "uart_set_pin() failed (%s).", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "uart_param_config() failed (%s).", esp_err_to_name(err));
        }
        if (err != ESP_OK) {
            cleanup();
        }
    } else {
        ESP_LOGE(TAG, "uart_driver_install() failed (%s).", esp_err_to_name(err));
    }

    // Returns ESP_OK or negative error code from esp_err_t
    return -err;
}

// Start communications with a TMC2209.
esp_err_t tmc2209_start(int32_t address)
{
    esp_err_t err = ESP_ERR_INVALID_STATE;

    if (g_uart >= 0) {
        ESP_LOGI(TAG, "Starting TMC2209 %d.", address);
        uint32_t data = TMC2209_REG_GCONF_DEFAULTS;
        err = write(address, 0, &data);
        if (err != sizeof(data)) {
            ESP_LOGE(TAG, "Failed to start TMC2209.");
        }
    }

    return err;
}
 
 // Deinitialise the TMC2209 driver.
void tmc2209_deinit()
{
    cleanup();
}

// Write a buffer of data to the given register of the TMC2209 at
// the given address.
 esp_err_t tmc2209_write(int32_t address, int32_t reg, uint32_t data)
 {
    return write(address, reg, &data);
 }

 // Read a buffer of data from the given register of the TMC2209
 // at the given address
 esp_err_t tmc2209_read(int32_t address, int32_t reg, uint32_t *data)
 {
    return read(address, reg, data);
 }

// End of file

