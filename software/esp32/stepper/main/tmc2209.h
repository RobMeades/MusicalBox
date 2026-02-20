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

 #ifndef _TMC2209_H_
 #define _TMC2209_H_
 
 /** @file
  * @brief The TMC2209 stepper motor driver API for the stepper motor
  * application.
  */
 
 #ifdef __cplusplus
 extern "C" {
 #endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

/** The value that will be written to the TMC2209 GCONF register
 * (register 0, 10 bits wide) by tmc2209_start().  The important
 * bits are:
 *
 * bit 0: i_scale_analog = 0 to use internal voltage reference.
 * bit 6: pdn_disable = 1 so that the PDN function is not on the
 *        UART pin.
 * bit 7: mstep_reg_select = so that microstep resolution is set by
 *        the MRES register.
 * bit 8: multistep_filt = 1 just to keep it at its power-on defaults.
 *
 * The rest will be set to 0.
 */
#define TMC2209_REG_GCONF_DEFAULTS 0x000001c0


/** Masks for the line states of each pin of a TMC2209, 
 * use on the return value of tmc2209_read_lines().
 */
#define TMC2209_LINE_MASK_ENN       0x0001
#define TMC2209_LINE_MASK_MS1       0x0004
#define TMC2209_LINE_MASK_MS2       0x0008
#define TMC2209_LINE_MASK_DIAG      0x0010
#define TMC2209_LINE_MASK_PDN_UART  0x0040
#define TMC2209_LINE_MASK_STEP      0x0080
#define TMC2209_LINE_MASK_SPREAD_EN 0x0100
#define TMC2209_LINE_MASK_DIR       0x0200

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */
  
/* ----------------------------------------------------------------
 * FUNCTIONS
 * -------------------------------------------------------------- */

/** Initialise the interface to the TMC2209.  Note that tmc2209_start()
 * still needs to be called before the TMC2209 will respond to
 * read or write requests.
 *
 * @param uart     the UART number to use (e.g. UART_NUM_1).
 * @param pin_txd  the GPIO number for the transmit data pin, e.g. 21.
 * @param pin_rxd  the GPIO number for the receive data pin, e.g. 10.
 * @param baud     the baud rate to use, e.g. 115200.
 * @return         ESP_OK on success, else a negative value from esp_err_t.
 */
esp_err_t tmc2209_init(int32_t uart, int32_t pin_txd, int32_t pin_rxd,
                       int32_t baud);

/** Start communications with a particular TMC2209.
 *
 * @param address the address of the device, range 0 to 3.
 * @return        ESP_OK on success, else a negative value from esp_err_t.
 */
esp_err_t tmc2209_start(int32_t address);

 /** Deinitialise the interface to the TMC2209.
 */
void tmc2209_deinit();

/** Write a buffer of data to the given register of the TMC2209 at
 * the given address.
 *
 * @param address the address of the device, range 0 to 3.
 * @param reg     the register to write to, range 0 to 127.
 * @param data    the data to send.
 * @return        the number of data bytes sent or negative error
 *                code from esp_err_t.
 */
esp_err_t tmc2209_write(int32_t address, int32_t reg, uint32_t data);

/** Read a buffer of data from the given register of the TMC2209 at
 * the given address.
 *
 * @param address the address of the device, range 0 to 3.
 * @param reg     the register to write to, range 0 to 127.
 * @param data    a pointer to a uint32_t into which the received
 *                data will be written; may be NULL.
 * @return        the number of bytes written to data or
 *                negative error code from esp_err_t.
 */
esp_err_t tmc2209_read(int32_t address, int32_t reg, uint32_t *data);

/** Read the state of all of a TMC2209's lines.  The return
 * value, if non-negative, may be masked with one or more
 * TMC2209_LINE_MASK_* values (see above) to get the state
 * of a given line.  The top byte of the response contains the
 * version number of the IC (normally 0x21).
 *
 * @param address the address of the device, range 0 to 3.
 * @return        the lines state as a bit-map, else negative
 *                error code from esp_err_t.
 */
esp_err_t tmc2209_read_lines(int32_t address);

/** Read the microstep counter of a TMC2209.
 *
 * @param address the address of the device, range 0 to 3.
 * @return        the count, else negative error code
 *                from esp_err_t.
 */
 esp_err_t tmc2209_get_position(int32_t address);

/** Set the microstep resolution of a TMC2209.
 *
 * @param address    the address of the device, range 0 to 3.
 * @param resolution the power of 2 value that the microstep
 *                   resolution should be set, from 1
 *                   to 256; if not a power of two
 *                   the value is rounded down.
 * @return           the power of two set, else negative
 *                   error code from esp_err_t.
 */
 esp_err_t tmc2209_set_microstep_resolution(int32_t address, int32_t resolution);

/** Get the microstep resolution of the stepper motor
 * attached to a TMC2209 device.
 *
 * @param address               the address of the device,
 *                              range 0 to 3.
 * @return                      the microstep resolution,
 *                              else negative error code
 *                              from esp_err_t.
 */
esp_err_t tmc2209_get_microstep_resolution(int32_t address);


/** Set the velocity of the stepper motor attached to a
 * TMC2209 device.
 *
 * IMPORTANT: this will drive the steps of the stepper
 * motor from its own internal step generator, i.e. it
 * WILL START MOVING IMMEDIATELY.
 *
 * @param address               the address of the device,
 *                              range 0 to 3.
 * @param microsteps_per_second the velocity to set in
 *                              microsteps per second.
 * @return                      the velociy set, else
 *                              negative error code
 *                              from esp_err_t.
 */
esp_err_t tmc2209_set_velocity(int32_t address,
                               int32_t microsteps_per_second);

#ifdef __cplusplus
}
#endif

/** @}*/

#endif // _TMC2209_H_

// End of file