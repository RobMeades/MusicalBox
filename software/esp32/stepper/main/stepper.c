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
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "errno.h"
#include "driver/uart.h"
#include "driver/gpio.h"
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

// Count of the number of ping losses.
static uint32_t g_pings_lost = 0;

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

// Callback function for ping loss
static void ping_loss_cb(void *arg)
{
    (void) arg;

    g_pings_lost++;
#if defined (CONFIG_STEPPER_DEBUG_LED_PIN) && (CONFIG_STEPPER_DEBUG_LED_PIN >= 0)
    // Switch on the debug LED forever
    gpio_set_level(CONFIG_STEPPER_DEBUG_LED_PIN, 0);
#endif
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Entry point
void app_main(void)
{
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
            tmc2209_init_stallguard(TMC2209_ADDRESS, -1, 0, CONFIG_STEPPER_DIAG_PIN,
                                    diag_interrupt_handler, NULL);
        }
#endif
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initialization complete.");

#if 0
        tmc2209_start(TMC2209_ADDRESS);

        uint32_t stepper_data = 0;
        ESP_LOGI(TAG, "Read data from TMC2209 %d.", TMC2209_ADDRESS);
        if (tmc2209_read(TMC2209_ADDRESS, 0, &stepper_data) == sizeof(stepper_data)) {
            ESP_LOGI(TAG, "Got back 0x%08x.", stepper_data);
        } else {
            ESP_LOGE(TAG, "Read failed.");
        }
        ESP_LOGI(TAG, "Read line state of TMC2209 %d.", TMC2209_ADDRESS);
        err = tmc2209_read_lines(TMC2209_ADDRESS);
        if (err >= 0) {
            ESP_LOGI(TAG, "Lines are 0x%04x.", err);
            err = ESP_OK;
        }

        ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
        ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

        ESP_LOGI(TAG, "Setting TMC2209 %d to full step.", TMC2209_ADDRESS);
        err = tmc2209_set_microstep_resolution(TMC2209_ADDRESS, 1);
        if (err >= 0) {
            ESP_LOGI(TAG, "Microstep resolution is now %d.", err);
            err = ESP_OK;
        }
        ESP_LOGI(TAG, "Getting TMC2209 %d microstep resolution.", TMC2209_ADDRESS);
        err = tmc2209_get_microstep_resolution(TMC2209_ADDRESS);
        if (err >= 0) {
            ESP_LOGI(TAG, "Microstep resolution read-back is %d.", err);
            err = ESP_OK;
        }

#if defined CONFIG_STEPPER_DIAG_PIN && (CONFIG_STEPPER_DIAG_PIN >= 0)
        if (err == ESP_OK) {
            tmc2209_set_stallguard(TMC2209_ADDRESS, -1, 100);
        }
#endif
        ESP_LOGI(TAG, "Rotate TMC2209 %d a little.", TMC2209_ADDRESS);
        err = tmc2209_get_position(TMC2209_ADDRESS);
        if (err >= 0) {
            ESP_LOGI(TAG, "Starting microsteps reading is %d.", err);
            err = ESP_OK;
        }
        ESP_LOGI(TAG, "Setting velocity.");
        err = tmc2209_set_velocity(TMC2209_ADDRESS, (1000 * 64) * 4);
        if (err >= 0) {
            ESP_LOGI(TAG, "Velocity is now %d.", err);
            err = ESP_OK;
        }

        ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
        ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Waiting for one revolution of the 24BYJ48-034"
                     " stepper motor (at full step resolution)...");
            for (size_t x = 0; x < 10; x++) {
                vTaskDelay(pdMS_TO_TICKS((1000 * 32) / 4 / 10));
                ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));
            }
        }

        ESP_LOGI(TAG, "Stopping.");
        tmc2209_set_velocity(TMC2209_ADDRESS, 0);
        err = tmc2209_get_position(TMC2209_ADDRESS);
        if (err >= 0) {
            ESP_LOGI(TAG, "Microsteps reading is now %d.", err);
            err = ESP_OK;
        }

        ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
        ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

        ESP_LOGI(TAG, "DONE with motor stuff");
        esp_task_wdt_add(NULL);
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
        }
        esp_task_wdt_delete(NULL);
#else
        // Ping the host
        char hostname_buffer[64];
        int hostname_length = network_hostname_from_url(CONFIG_STEPPER_FIRMWARE_UPG_URL, hostname_buffer, sizeof(hostname_buffer));
        if ((hostname_length > 0) && (hostname_length < sizeof(hostname_buffer))) { 
            while (err == ESP_OK) {
                if (g_pings_lost == 0) {
                    // Flash the debug LED as a keep-alive
                    flash_debug_led(DEBUG_LED_LONG_MS);
                }
                err = ping_start(hostname_buffer, ping_loss_cb, NULL);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Unable to start pinging host \"%s\".", hostname_buffer);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10000));
                }
            }
        } else {
            ESP_LOGE(TAG, "Unable to find hostname in \"%s\" (or fit it in buffer of size %d).",
                     CONFIG_STEPPER_FIRMWARE_UPG_URL, sizeof(hostname_buffer));
        }
#endif
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

