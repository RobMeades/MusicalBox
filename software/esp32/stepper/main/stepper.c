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
 * @brief Implementation of an OTA-updated ESP32-based stepper motor driver.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "errno.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "ota.h"
#include "network.h"
#include "tmc2209.h"
#include "ping.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

 // Logging prefix
 #define TAG "stepper"

// UART buffer
#define UART_RX_BUFFFER_SIZE 256

// The addresss of the TMC2209 device we are going to us
#define TMC2209_ADDRESS 0

// The sense resisitor wired to the BRA and BRB pins of the TMC2209,
// the BigTreeTech board uses 100 milliOhms
#define TMC2209_RSENSE_MOHM 110

// The desired stepper motor run current in milliamps
#if (defined (CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)) || \
    (defined(CONFIG_STEPPER_TEST_ROTATION) && (CONFIG_STEPPER_TEST_ROTATION > 0)) 
    // Door motors are only little, and this is a safe current for any motor
#    define STEPPER_MOTOR_CURRENT_MA 150
#else
#    define STEPPER_MOTOR_CURRENT_MA 1000
#endif

// The percentage of the run current to apply during hold;
// don't need a lot, let it cool down
#define STEPPER_MOTOR_HOLD_CURRENT_PERCENT 20

// The desired stepper motor velocity
#if (defined(CONFIG_STEPPER_TEST_ROTATION) && (CONFIG_STEPPER_TEST_ROTATION > 0)) 
    // The velocity for testing any motor
#    define VELOCITY_MILLIHERTZ (1000 * 64 * 8)
#elif (defined (CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0))
    // The velocity for door operation
#    define VELOCITY_MILLIHERTZ (1000 * 64)
#else
    // The velocity for lift operation
#    define VELOCITY_MILLIHERTZ (1000 * 64 * 10)
#endif

// Standard short duration for an LED lash
#define DEBUG_LED_SHORT_MS 50

// Standard short duration for a long LED lash
#define DEBUG_LED_LONG_MS 1000

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// A place to remember the handle of the stall task if created.
static TaskHandle_t g_stall_task_handle = NULL;

// A semaphore to let diag_interrupt_handler signal the stall task.
static SemaphoreHandle_t g_diag_semaphore = NULL;

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Flash the debug LED
static void flash_debug_led(int32_t duration_ms)
{
#if defined (CONFIG_STEPPER_DEBUG_LED_PIN) && (CONFIG_STEPPER_DEBUG_LED_PIN >= 0)
    gpio_set_level(CONFIG_STEPPER_DEBUG_LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(CONFIG_STEPPER_DEBUG_LED_PIN, 1);
#endif
}

#if defined CONFIG_STEPPER_DIAG_PIN && (CONFIG_STEPPER_DIAG_PIN >= 0)

// DIAG interrupt handler,
static void diag_interrupt_handler(void *handler_arg)
{
    (void) handler_arg;
    BaseType_t higherPriorityTaskWoken;

    if (g_stall_task_handle && g_diag_semaphore) {
        xSemaphoreGiveFromISR(g_diag_semaphore, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
    }
}

#endif

// Task to handle stall indications.
static void stall_task(void *arg)
{
    while (1) {
        xSemaphoreTake(g_diag_semaphore, portMAX_DELAY);
        ESP_LOGI(TAG, "STALL");
    }
}

#if defined (CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0)
// Return true if the lift is at a limit, else false.
static bool at_limit()
{
    return !gpio_get_level(CONFIG_STEPPER_LIFT_LIMIT_PIN);
}
#endif

#if defined (CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
// Return true if the lift is fully down.
static bool is_down()
{
    return !gpio_get_level(CONFIG_STEPPER_LIFT_DOWN_PIN);
}
#endif

#if defined (CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)
// Return true if a door is in the open position.
static bool is_open()
{
    return !gpio_get_level(CONFIG_STEPPER_DOOR_OPEN_PIN);
}
#endif

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Entry point
void app_main(void)
{
#if defined(CONFIG_STEPPER_ENABLE_PIN) && (CONFIG_STEPPER_ENABLE_PIN >= 0)
    // Stop the motor moving too much before it is configured,
    if (gpio_set_level(CONFIG_STEPPER_ENABLE_PIN, 1) == ESP_OK) {
        gpio_set_direction(CONFIG_STEPPER_ENABLE_PIN, GPIO_MODE_OUTPUT);
    }
#endif

    ESP_LOGI(TAG, "Stepper app_main start");

    // Create the default event loop, for everyone's use
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s.", esp_err_to_name(err));
    }

#if defined (CONFIG_STEPPER_DEBUG_LED_PIN) && (CONFIG_STEPPER_DEBUG_LED_PIN >= 0)
    // Configure our debug LED
    if (err == ESP_OK) {
        err = gpio_set_level(CONFIG_STEPPER_DEBUG_LED_PIN, 1);
        if (err == ESP_OK) {
            err = gpio_set_direction(CONFIG_STEPPER_DEBUG_LED_PIN, GPIO_MODE_OUTPUT);
            if (err == ESP_OK) {
                // Flash it so that we know it can be active
                flash_debug_led(DEBUG_LED_SHORT_MS);
            }
        }
    }
#endif

#if defined (CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0)
    // Configure the lift limit pin
    if (err == ESP_OK) {
        err = gpio_set_direction(CONFIG_STEPPER_LIFT_LIMIT_PIN, GPIO_MODE_INPUT);
    }
#endif

#if defined (CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
    // Configure the lift down pin
    if (err == ESP_OK) {
        err = gpio_set_direction(CONFIG_STEPPER_LIFT_DOWN_PIN, GPIO_MODE_INPUT);
    }
#endif

#if defined (CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)
    // Configure the door open pin
    if (err == ESP_OK) {
        err = gpio_set_direction(CONFIG_STEPPER_DOOR_OPEN_PIN, GPIO_MODE_INPUT);
    }
#endif

#if !defined(CONFIG_STEPPER_NO_WIFI) || (CONFIG_STEPPER_NO_WIFI <= 0) 
    // Initialise OTA
    if (err == ESP_OK) {
        err = ota_init();
    }

    // Initialize networking
    if (err == ESP_OK) {
        err = network_init(CONFIG_STEPPER_WIFI_SSID, CONFIG_STEPPER_WIFI_PASSWORD, WIFI_AUTH_OPEN);
    }

    // Check for an OTA update, which may restart the system
    if (err == ESP_OK) {
        err = ota_update(CONFIG_STEPPER_FIRMWARE_UPG_URL, CONFIG_STEPPER_OTA_RECV_TIMEOUT_MS);
    }
#else
    ESP_LOGW(TAG, "CONFIG_STEPPER_NO_WIFI is greater than zero, not connecting to WiFi.");
#endif

    // Initialize the TMC2209 stepper motor driver interface
    if (err == ESP_OK) {
        err = tmc2209_init(CONFIG_STEPPER_UART_NUM, CONFIG_STEPPER_UART_TXD_PIN,
                           CONFIG_STEPPER_UART_RXD_PIN, CONFIG_STEPPER_UART_BAUD_RATE);
    }

    // Create the RTOS stuff needed for stall handling
    if (err == ESP_OK) {
        vSemaphoreCreateBinary(g_diag_semaphore);
        if (!g_diag_semaphore ||
            (xTaskCreate(&stall_task, "stall_task", 1024 * 4, NULL, 5, &g_stall_task_handle) != pdPASS)) {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "Unable to create stall_task or semaphore.");
        }
#if defined CONFIG_STEPPER_DIAG_PIN && (CONFIG_STEPPER_DIAG_PIN >= 0)
        // Initial setup of stall detection with threshold value that means
        // a stall should never be detected
        if (err == ESP_OK) {
            tmc2209_init_stallguard(TMC2209_ADDRESS, -1, 100, CONFIG_STEPPER_DIAG_PIN,
                                    diag_interrupt_handler, NULL);
        }
#endif
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initialization complete.");

        err = tmc2209_start(TMC2209_ADDRESS, CONFIG_STEPPER_ENABLE_PIN);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Setting TMC2209 %d to full step.", TMC2209_ADDRESS);
            err = tmc2209_set_microstep_resolution(TMC2209_ADDRESS, 1);
            if (err >= 0) {
                ESP_LOGI(TAG, "Microstep resolution is now %d.", err);
                err = ESP_OK;
            }
        }

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Setting motor current to %d mA.", STEPPER_MOTOR_CURRENT_MA);
            err = tmc2209_set_current(TMC2209_ADDRESS, TMC2209_RSENSE_MOHM,
                                      STEPPER_MOTOR_CURRENT_MA,
                                      STEPPER_MOTOR_HOLD_CURRENT_PERCENT);
            if (err >= 0) {
                ESP_LOGI(TAG, "Current set to %d mA.", err);
                err = ESP_OK;
            }
        }

#if defined CONFIG_STEPPER_DIAG_PIN && (CONFIG_STEPPER_DIAG_PIN >= 0)
        if (err == ESP_OK) {
            tmc2209_set_stallguard(TMC2209_ADDRESS, -1, 0);
        }
#endif

        // Allow us to feed the watchdog
        esp_task_wdt_add(NULL);

        if (err == ESP_OK) {

#if defined(CONFIG_STEPPER_TEST_ROTATION) && (CONFIG_STEPPER_TEST_ROTATION > 0) 
            // Test mode only
            ESP_LOGI(TAG, "TEST MODE");
            ESP_LOGI(TAG, "Setting velocity.");
            tmc2209_set_velocity(TMC2209_ADDRESS, VELOCITY_MILLIHERTZ);
            size_t run_time_seconds = 3;
            ESP_LOGI(TAG, "Running the motor for %d second(s)...", run_time_seconds);
            tmc2209_motor_enable(TMC2209_ADDRESS);
            vTaskDelay(pdMS_TO_TICKS(run_time_seconds * 1000));
            ESP_LOGI(TAG, "Stopping.");
#elif defined (CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0) && \
      defined (CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
            // We're operating the lift
            ESP_LOGI(TAG, "LIFT MODE");
            size_t repeats = 2;
            for (size_t y = 0; y < repeats; y++) {

                int32_t velocity_sign = 1;
                ESP_LOGI(TAG, "Setting velocity.");
                if (is_down()) {
                    ESP_LOGI("INFO", "Down indicator returns true, hence velocity positive (go up).");
                } else {
                    ESP_LOGI("INFO", "Down indicator returns false, hence velocity negative (go down).");
                    velocity_sign = -1;
                }

                tmc2209_set_velocity(TMC2209_ADDRESS, velocity_sign * VELOCITY_MILLIHERTZ);
                ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
                ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

                tmc2209_motor_enable(TMC2209_ADDRESS);
                size_t guard_time_seconds = 27;
                ESP_LOGI(TAG, "Running for up to %d seconds...", guard_time_seconds);
                size_t loops_per_second = 10;
                size_t hysteresis_count = 0;
                if (at_limit()) {
                    // Wait three seconds after determining we are at a limit before
                    // checking again
                    hysteresis_count = loops_per_second * 3;
                }
                bool stop = false;
                for (size_t x = 0; (x < loops_per_second * guard_time_seconds) && !stop; x++) {
                    vTaskDelay(pdMS_TO_TICKS(1000 / loops_per_second));
                    ESP_LOGI("INFO", "at limit %s, down %s.", at_limit() ? "true" : "false",
                            is_down() ? "true" : "false");
                    // Stop when the appropriate limit is hit
                    if ((hysteresis_count == 0)) {
                        if (at_limit()) {
                            // Stop if we're going up or if is_down() is true
                            if (velocity_sign > 0 || is_down()) {
                                ESP_LOGI("INFO", "At limit, stopping.");
                                stop = true;
                            }
                        }
                    }
                    if (hysteresis_count > 0) {
                        hysteresis_count--;
                    }
                    esp_task_wdt_reset();
                }

                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Stopping.");
                    tmc2209_set_velocity(TMC2209_ADDRESS, 0);
                    err = tmc2209_get_position(TMC2209_ADDRESS);
                    if (err >= 0) {
                        ESP_LOGI(TAG, "Microsteps reading is now %d.", err);
                        err = ESP_OK;
                    }
                }
                tmc2209_motor_disable(TMC2209_ADDRESS);
                ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
                ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

                ESP_LOGI(TAG, "Run %d of %d completed.", y + 1, repeats);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

#elif defined (CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)
            // We're operating a door
            ESP_LOGI(TAG, "DOOR MODE");
            // Switch off StallGuard and CoolStep as they don't work well at low speeds
            ESP_LOGI(TAG, "Switching off StallGuard and CoolStep.");
            err = tmc2209_set_stealth_chop_threshold(TMC2209_ADDRESS, UINT32_MAX);
            if (err == ESP_OK) {
                // Quieten things down: these values derived using the Trinamic
                // spreadsheet:
                //
                // https://www.analog.com/media/en/engineering-tools/design-tools/tmc2209_calculations.xlsx
                //
                // ..."chopper parameters" tab, with input values for the
                // 24BYJ48-034 stepper motor:
                //
                // 30 Ohms, 30 mH (estimate), 183 mA, 12 V, TBL = 3, TOFF = 3:
                //
                //                ***** HSTRT = 6, HEND = 0 *****
                //
                // These reduce the cricket buzz to a rapid ticking 
                err = tmc2209_stop_that_bloody_racket(TMC2209_ADDRESS, 3, 3, 6, 0);
            }
            if (err == ESP_OK) {
                size_t repeats = 11;
                bool stop = false;
                for (size_t y = 0; (y < repeats) || stop; y++) { // "stop" here to always end closed
                    int32_t velocity_sign = 1;
                    ESP_LOGI(TAG, "Setting velocity.");
                    if (is_open()) {
                        ESP_LOGI("INFO", "Open indicator returns true, hence velocity positive (close).");
                    } else {
                        ESP_LOGI("INFO", "Open indicator returns false, hence velocity negative (open).");
                        velocity_sign = -1;
                    }
                    tmc2209_set_velocity(TMC2209_ADDRESS, velocity_sign * VELOCITY_MILLIHERTZ * 2);
                    ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
                    ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

                    tmc2209_motor_enable(TMC2209_ADDRESS);
                    int64_t close_guard_time_ms = 2450;
                    int64_t open_guard_time_ms = 3000;
                    ESP_LOGI(TAG, "Running for up to %lld milliseconds...", open_guard_time_ms);
                    size_t hysteresis_count = 0;
                    if (is_open()) {
                        // Wait a while after determining we are open before
                        // checking again, otherwise we will never move from open
                        hysteresis_count = 200000;
                    }
                    stop = false;
                    int64_t start_time = esp_timer_get_time();
                    while (((velocity_sign < 0) && !stop && !(esp_timer_get_time() - start_time > (open_guard_time_ms * 1000))) || // opening
                        ((velocity_sign > 0) && !(esp_timer_get_time() - start_time > (close_guard_time_ms * 1000)))) {         // closing
                        // Stop when the appropriate limit is hit
                        if ((hysteresis_count == 0)) {
                            if ((velocity_sign < 0) && is_open()) {
                                ESP_LOGI("INFO", "Door is open, stopping.");
                                stop = true;
                            }
                        }
                        if (hysteresis_count > 0) {
                            hysteresis_count--;
                        }
                        esp_task_wdt_reset();
                    }
                    tmc2209_motor_disable(TMC2209_ADDRESS);
                    ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
                    ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

                    ESP_LOGI(TAG, "Run %d of %d completed.", y + 1, repeats);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
#endif
            tmc2209_set_velocity(TMC2209_ADDRESS, 0);
            tmc2209_motor_disable(TMC2209_ADDRESS);
        }
        ESP_LOGI(TAG, "DONE with motor stuff");
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
        }
        esp_task_wdt_delete(NULL);

    } else {
        ESP_LOGE(TAG, "Initialization failed, system cannot continue, will restart soonish.");
#if defined CONFIG_STEPPER_DIAG_PIN && (CONFIG_STEPPER_DIAG_PIN >= 0)
        tmc2209_deinit_stallguard(CONFIG_STEPPER_DIAG_PIN);
#endif
        if (g_stall_task_handle) {
            vTaskDelete(g_stall_task_handle);
            g_stall_task_handle = NULL;
        }
        if (g_diag_semaphore) {
            vSemaphoreDelete(g_diag_semaphore);
            g_diag_semaphore = NULL;
        }
        tmc2209_deinit();
        network_deinit();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
}

// End of file

