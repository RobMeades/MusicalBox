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
#include "driver/gpio.h"
#include "string.h"

#include "tmc2209.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

// Logging prefix
#define TAG "tmc2209"

// UART buffer size
#define UART_RX_BUFFFER_SIZE 256

// The maximum number of TMC2209's that can be addressed by this API
#define MAX_NUM_TMC2209 4

// The contents of the sync and reserved fields of a datagram
#define DATAGRAM_SYNC_AND_RESERVED 0x05

// From the table in section 14 of the TMC2209 datasheet,
// the number to multiply VACTUAL by to get a step frequency
// in milliHertz
#define VACTUAL_TO_MILLIHERTZ 715

// The maximum value of IRUN or IHOLD, which are 5-bit values.
#define IRUN_OR_IHOLD_MAX 31

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// The UART we are using, -1 tmc2209_init() has not been called.
static int32_t g_uart = -1;

// The GPIOs that are wired to the enable pins of the TMC2209's;
// may be -1
static gpio_num_t g_pin_motor_enable[MAX_NUM_TMC2209];

// Table of permitted microstep values: order is important, the index
// into the array corresponds to the MRES value in the CHOPCONF
// register i.e. 256 is an MRES value of 0, 1 is an MRES value of 8.
static const int32_t g_microstep_table[] = {256, 128, 64, 32, 16, 8, 4, 2, 1};

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
    for (size_t x = 0; x < sizeof(g_pin_motor_enable) / sizeof(g_pin_motor_enable[0]); x++) {
        if (g_pin_motor_enable[x] >= 0) {
            gpio_set_level(g_pin_motor_enable[x], 1);
            g_pin_motor_enable[x] = -1;
        }
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
        *(p + index) = reg;
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
                err = -ESP_ERR_INVALID_CRC;
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

// Do a generic register write.
static esp_err_t write_reg(int32_t address, int32_t reg,
                           uint32_t data)
{
    esp_err_t err = write(address, reg, &data); 

    if (err == sizeof(data)) {
        err = ESP_OK;
    } else {
        if (err >= 0) {
            err = -ESP_ERR_INVALID_RESPONSE;
        }
    }

    return err;
}

// Do a generic register read.
static esp_err_t read_reg(int32_t address, int32_t reg)
{
    uint32_t data = 0;

    esp_err_t err = read(address, reg, &data);
    if (err == sizeof(data)) {
        err = (esp_err_t) data;
    } else {
        if (err >= 0) {
            err = -ESP_ERR_INVALID_RESPONSE;
        }
    }

    return err;
}

// Set the stallguard registers.
static esp_err_t set_stallguard(int32_t address,
                                int32_t tcoolthrs,
                                uint8_t sgthrs)
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Configuring StallGuard, TCOOLTHRS %d, SGTHRS %d.",
             tcoolthrs, sgthrs);
    if (tcoolthrs < 0) {
        // Read the TSTEP register so that we
        // can use it as TCOOLTHRS
        err = read_reg(address, 0x12);
        if (err >= 0) {
            tcoolthrs = err;
            err = ESP_OK;
            ESP_LOGI(TAG, "TCOOLTHRS will be TSTEP which is %d.", tcoolthrs);
        }
    }
    if (err == ESP_OK) {
        // Write to the TCOOLTHRS register (0x14) 
        err = write_reg(address, 0x14, tcoolthrs); 
    }
    if (err == ESP_OK) {
        // Write to the SGTHRS register (0x40) 
        err = write_reg(address, 0x40, sgthrs); 
    }

    return err;
}

// Get the value of VFS in millivolts that we get
// for a given setting of the VSENSE bit of the
// CHOP CONF register.
static uint32_t v_full_scale_millivolts(uint32_t v_sense)
{
    // See the bottom of the "Sense resistor voltage levels"
    // table in the datasheet, the full scale voltage is
    // halved if vsense is 1
    return v_sense > 0 ? 180 : 320;
}

// Return the RMS current in milliAmps for a given i_run, VSENSE
// bit and r_sense_mohm.
static uint32_t rms_current_milliamps(uint32_t i_run,
                                      uint32_t v_sense_bit,
                                      uint32_t r_sense_mohm)
{
    // From section 9 of the data sheet, the RMS current
    // delivered is:
    //
    // ((IRUN + 1)  / 32) * (v_full_scale_millivolts / (r_sense_mohm + 20)) *  (1 / root-2)
    //
    // ...where max_microvolts is VSRTL (320 mV) if the
    // VSENSE bit is set to 0 or VSRTH (180 mV) if the VSENSE
    // bit is set to 1, the 20 mOhms is for PCB track resistance
    // and the root-2 is to give the RMS current

    // Do this in uint64_t just in case
    uint64_t numerator = (uint64_t) (i_run + 1) * v_full_scale_millivolts(v_sense_bit) * 1000000;
    uint64_t denominator = (uint64_t) (32 * 1414 * (r_sense_mohm + 20));

    return (uint32_t) (numerator / denominator);
}

// Set the enable pin of a motor.
static esp_err_t setMotorEnablePin(int32_t address, bool enableNotDisable)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (address < (sizeof(g_pin_motor_enable) / sizeof(g_pin_motor_enable[0]))) {
        err = ESP_ERR_NOT_FOUND;
        if (g_pin_motor_enable[address] >= 0) {
            // Pin is low for enable (true), high for disable (false)
            err = gpio_set_level(g_pin_motor_enable[address], !enableNotDisable);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "MOTOR %s for address %d, pin %d.",
                        enableNotDisable ? "ENABLED": "DISABLED", address,
                        g_pin_motor_enable[address]);
            }
        } else {
                ESP_LOGE(TAG, "Not %sing motor, no motor enable GPIO pin was set.",
                         enableNotDisable ? "enabl": "disabl");
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
    esp_err_t err = ESP_OK;

    if (g_uart < 0) {
        // UART configuration
        uart_config_t uart_config = {
            // baud_rate is populated below
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
    
        // Assume no enable pins
        for (size_t x = 0; x < sizeof(g_pin_motor_enable) / sizeof(g_pin_motor_enable[0]); x++) {
            g_pin_motor_enable[x] = -1;
        }

        ESP_LOGI(TAG, "Installing TMC2209 driver on UART %d, TXD pin %d,"
                " RXD pin %d, baud rate %d.", uart, pin_txd, pin_rxd, baud);
        // Configure the UART that talks to the stepper motor driver
        err = uart_driver_install(uart, UART_RX_BUFFFER_SIZE * 2, 0, 0, NULL, 0);
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
    } else {
        ESP_LOGW(TAG, "tmc2209_init() called when already enabled");
    }

    // Returns ESP_OK or negative error code from esp_err_t
    return -err;
}

// Start communications with a TMC2209.
esp_err_t tmc2209_start(int32_t address, int32_t pin_motor_enable)
{
    esp_err_t err = ESP_ERR_INVALID_STATE;

    if (g_uart >= 0) {
        err = ESP_OK;
        char buffer[32] = "no enable pin";
        if (pin_motor_enable) {
            snprintf(buffer, sizeof(buffer), "motor enable on pin %d", (int) pin_motor_enable);
        }
        if (pin_motor_enable >= 0) {
            esp_err_t err = ESP_ERR_INVALID_ARG;
            if (address < sizeof(g_pin_motor_enable) / sizeof(g_pin_motor_enable[0])) {
                err = gpio_set_level(pin_motor_enable, 1);
                if (err == ESP_OK) {
                    err = gpio_set_direction(pin_motor_enable, GPIO_MODE_OUTPUT);
                    if (err == ESP_OK) {
                        // Save the motor enable pin for tmc2209_motor_enable()
                        g_pin_motor_enable[address] = (gpio_num_t) pin_motor_enable;
                    }
                }
            }
        }
        ESP_LOGI(TAG, "Starting TMC2209 %d%, s.", address, buffer);
        if (err == ESP_OK) {
            err = write_reg(address, 0, TMC2209_REG_GCONF_DEFAULTS);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start TMC2209.");
            }
        }
    }

    return err;
}
 
// Set the motor enable pin low to enable motors.
esp_err_t  tmc2209_motor_enable(int32_t address)
{
    return setMotorEnablePin(address, true);
}

// Set the motor enable pin high to disable motors.
esp_err_t  tmc2209_motor_disable(int32_t address)
{
    return setMotorEnablePin(address, false);
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

// Read the state of all of a TMC2209's lines.
esp_err_t tmc2209_read_lines(int32_t address)
{
    // Read the IOIN register (0x06)
    return read_reg(address, 0x06);
}

// Read the microstep counter of a TMC2209.
esp_err_t tmc2209_get_position(int32_t address)
{
    // Read the MSCNT register (0x6a)
    return read_reg(address, 0x6a);
}

// Set the microstep resolution of a TMC2209.
 esp_err_t tmc2209_set_microstep_resolution(int32_t address, int32_t resolution)
 {
    esp_err_t err = ESP_ERR_INVALID_ARG;
    int32_t index;

    // Find the entry in g_microstep_table which is less than or
    // equal to microsteps
    for (index = 0;
         (index < sizeof(g_microstep_table) / sizeof(g_microstep_table[0])) &&
         (g_microstep_table[index] > resolution);
         index++) {
    }

    if (index < sizeof(g_microstep_table) / sizeof(g_microstep_table[0])) {
        // index is now the value to write to the MRES value in the
        // CHOPCONF register (0x6c).  Since there are other things in
        // there we need to do a read-modify-write
        err = read_reg(address, 0x6c);
        if (err >= 0) {
            uint32_t data = err;
            // The MRES value is in bits 24 to 27
            data &= 0xf0ffffff;
            data |= ((uint32_t) index) << 24;
            // Write it back again
            err = write_reg(address, 0x6c, data); 
            if (err == ESP_OK) {
                // Return the value set
                err = g_microstep_table[index];
            }
        }
    }

    return err;
}

// Get the microstep resolution of a TMC2209.
esp_err_t tmc2209_get_microstep_resolution(int32_t address)
{
    // Read the CHOPCONF register (0x6c).
    esp_err_t err = read_reg(address, 0x6c);
    if (err >= 0) {
        uint32_t data = err;
        err = ESP_ERR_INVALID_RESPONSE;
        // The MRES value is in bits 24 to 27
        data = (data >> 24) & 0x0f;
        // Use this as an index into g_microstep_table
        // to get the power-of-two value 
        if (data < sizeof(g_microstep_table) / sizeof(g_microstep_table[0])) {
            err = g_microstep_table[data];
        }
    }

    return err;
}

// Set the current supplied to the stepper motor.
esp_err_t tmc2209_set_current(int32_t address,
                              uint32_t r_sense_mohm, 
                              uint32_t run_current_ma,
                              uint32_t hold_current_percent)
{
    esp_err_t err = -ESP_ERR_INVALID_ARG;

    if (hold_current_percent <= 100) {
        // Can we achieve the desired current with VSENSE = 1
        // which halves the power dissipated through the sense
        // resistors?  Try it with the maximum value of IRUN
        uint32_t v_sense = 1;
        uint32_t max_current = rms_current_milliamps(IRUN_OR_IHOLD_MAX,
                                                     v_sense, r_sense_mohm);
        if (max_current < run_current_ma) {
            // Nope
            v_sense = 0;
            max_current = rms_current_milliamps(IRUN_OR_IHOLD_MAX,
                                                v_sense, r_sense_mohm);
        }

        // -1 because the values in the register are from 0
        uint32_t i_run = (run_current_ma * IRUN_OR_IHOLD_MAX / max_current) - 1;
        uint32_t i_hold = i_run * hold_current_percent / 100;

        ESP_LOGI(TAG, "VSENSE will be %u, IRUN %d, IHOLD %d.",
                 v_sense, i_run, i_hold);

        uint32_t data;
        // Set VSENSE: read the chopper configuration register (0x6c)
        err = read_reg(address, 0x6c);
        if (err >= 0) {
            // VSENSE choice is in bit 17
            data = (uint32_t) err;
            data = (data & 0xfffdffff) | (v_sense << 17);
            err = write_reg(address, 0x6c, data);
        }

        if (err >= 0) {
            // Set IRUN and IHOLD
            err = read_reg(address, 0x10);
            // IHOLD is the first 5 bits, IRUN bit 8 to 12
            if (err >= 0) {
                data = (uint32_t) err;
                data = (data & 0xffffe0e0) | (i_hold & 0x1f) | ((i_run & 0x1f) << 8);
                err = write_reg(address, 0x10, data);
            }
        }

        if (err >= 0) {
            // Finally, set i_scale_analog in the global configuration
            // register to 0 to use the values
            err = read_reg(address, 0);
            if (err >= 0) {
                // Set bit 0, I_scale_analog, to 0
                err =  write_reg(address, 0, err & 0xfffffffe); 
            }
        }

        if (err >= 0) {
            // Re-calculate the current to return
            err = (int32_t) rms_current_milliamps(i_run, v_sense, r_sense_mohm);
        }
    }

    return err;
}

// Return to using VRef to determine the drive current.
esp_err_t tmc2209_unset_current(int32_t address)
{
   // Read the general configuration register
    esp_err_t err = read_reg(address, 0);
    if (err >= 0) {
        // Set bit 0, I_scale_analog, to 1
        err =  write_reg(address, 0, err | 0x01); 
    }

    return err;
}

// Set the velocity of the stepper motor attached to a
// TMC2209 device.
esp_err_t tmc2209_set_velocity(int32_t address,
                               int32_t milliHertz)
{
    // Write to the VACTUAL register (0x22)
    milliHertz /= VACTUAL_TO_MILLIHERTZ;
    return write_reg(address, 0x22, milliHertz); 
}

// Get the value of TSTEP from a TMC2209.
esp_err_t tmc2209_get_tstep(int32_t address)
{
    // Read the TSTEP register (0x12)
    return read_reg(address, 0x12);
}

// Get the value of SG_RESULT from a TMC2209.
esp_err_t tmc2209_get_sg_result(int32_t address)
{
    // Read the SG_RESULT register (0x41)
    return read_reg(address, 0x41);
}

// Set the operation of stall-guard in a TMC2209.
esp_err_t tmc2209_init_stallguard(int32_t address,
                                  int32_t tcoolthrs,
                                  uint8_t sgthrs,
                                  int32_t pin,
                                  gpio_isr_t handler,
                                  void *handler_arg)
{
    esp_err_t err = -ESP_ERR_INVALID_ARG;

    if ((pin < 0) || (handler != NULL)) {
        err = set_stallguard(address, tcoolthrs, sgthrs);
        if ((err == ESP_OK) && (pin >= 0)) {
            ESP_LOGI(TAG, "Configuring interrupt pin %d.", pin);
            gpio_config_t cfg = {
                .intr_type = GPIO_INTR_POSEDGE,
                .mode = GPIO_MODE_INPUT,
                .pin_bit_mask = (1ULL << pin),
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .pull_up_en = GPIO_PULLUP_ENABLE
            };
            err = gpio_config(&cfg);
            if (err == ESP_OK) {
                err = gpio_install_isr_service(0);
                // ESP_ERR_INVALID_STATE means the ISR service was already installed
                if ((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE)) {
                    err = gpio_isr_handler_add((gpio_num_t) pin,
                                                handler, handler_arg);
                }
            }
        }
    }
    
    return err;
}

// Set the stall-guard registers in a TMC2209.
esp_err_t tmc2209_set_stallguard(int32_t address,
                                 int32_t tcoolthrs,
                                 uint8_t sgthrs)
{
    return set_stallguard(address, tcoolthrs, sgthrs);;
}

// Remove interrupt handling that was set up by
// tmc2209_init_stallguard().
void tmc2209_deinit_stallguard(int32_t pin)
{
    gpio_isr_handler_remove((gpio_num_t) pin);
}

// End of file

