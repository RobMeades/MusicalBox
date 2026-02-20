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
#include "esp_log.h"
#include "driver/gpio.h"
#include "errno.h"
#include "driver/uart.h"
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

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */
 
/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

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
        if (err == ESP_OK) {
            tmc2209_start(TMC2209_ADDRESS);
        }
    }

    if (err == ESP_OK) {    
        ESP_LOGI(TAG, "Initialization complete.");

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

        ESP_LOGI(TAG, "Setting TMC2209 %d to half step.", TMC2209_ADDRESS);
        err = tmc2209_set_microstep_resolution(TMC2209_ADDRESS, 2);
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

        ESP_LOGI(TAG, "Rotate TMC2209 %d a little.", TMC2209_ADDRESS);
        err = tmc2209_get_position(TMC2209_ADDRESS);
        if (err >= 0) {
            ESP_LOGI(TAG, "Starting microsteps reading is %d.", err);
            err = ESP_OK;
        }
        ESP_LOGI(TAG, "Setting velocity.");
        err = tmc2209_set_velocity(TMC2209_ADDRESS, 800);
        if (err >= 0) {
            ESP_LOGI(TAG, "Velocity is now %d.", err);
            err = ESP_OK;
        }

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Waiting a bit....");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ESP_LOGI(TAG, "Stopping.");
        tmc2209_set_velocity(TMC2209_ADDRESS, 0);
        err = tmc2209_get_position(TMC2209_ADDRESS);
        if (err >= 0) {
            ESP_LOGI(TAG, "Microsteps reading is now %d.", err);
            err = ESP_OK;
        }

        esp_task_wdt_add(NULL);
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
        }
        esp_task_wdt_delete(NULL);
#if 0
        // Ping the host
        char hostname_buffer[64];
        int hostname_length = network_hostname_from_url(CONFIG_STEPPER_FIRMWARE_UPG_URL, hostname_buffer, sizeof(hostname_buffer));
        if ((hostname_length > 0) && (hostname_length < sizeof(hostname_buffer))) { 
            while (err == ESP_OK) {
                err = ping_start(hostname_buffer);
                if (err != ESP_OK) {
                    ESP_LOGE(}TAG, "Unable to start pinging host \"%s\".", hostname_buffer);
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
        tmc2209_deinit();
        network_deinit();
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
}

// End of file

