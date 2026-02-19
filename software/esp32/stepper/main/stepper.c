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

#include "ota.h"
#include "network.h"
#include "ping.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

// UART buffer
#define UART_RX_BUFFFER_SIZE 256

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

static const char *TAG = "stepper";

// UART configuration
static const uart_config_t g_uart_config = {
    .baud_rate = CONFIG_STEPPER_UART_BAUD_RATE,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
};
 
/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

static void __attribute__((noreturn)) task_fatal_error()
{
    ESP_LOGE(TAG, "Exiting task due to fatal error...");
    vTaskDelete(NULL);

    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();

    while(1) {}
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
        ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(err));
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

    // Clean-up if initialization failed
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialization failed, system cannot continue, will restart soonish");
        network_deinit();
        // EXIT
        task_fatal_error();
    }
    
    // If we reach here, initialization was successful
    ESP_LOGI(TAG, "Network initialization complete, OTA task running");

    // Configure the UART that talks to the stepper motor driver
    uart_driver_install(UART_NUM_1, UART_RX_BUFFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &g_uart_config);
    uart_set_pin(UART_NUM_1, CONFIG_STEPPER_UART_TXD_PIN, CONFIG_STEPPER_UART_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Ping the host
    char hostname_buffer[64];
    int hostname_length = network_hostname_from_url(CONFIG_STEPPER_FIRMWARE_UPG_URL, hostname_buffer, sizeof(hostname_buffer));
    if ((hostname_length > 0) && (hostname_length < sizeof(hostname_buffer))) { 
        while (err == ESP_OK) {
            err = ping_start(hostname_buffer);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Unable to start pinging host \"%s\"", hostname_buffer);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        }
    } else {
        ESP_LOGE(TAG, "Unable to find hostname in \"%s\" (or fit it in buffer of size %d).",
                    CONFIG_STEPPER_FIRMWARE_UPG_URL, sizeof(hostname_buffer));
    }
}

// End of file

