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
 * @brief Implementation of logging that includes forwarding
 * to a remote client.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "log.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

 // Logging prefix
 #define TAG "log"

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

typedef struct {
    int socket;
    bool connected;
    struct sockaddr_in log_server;
    SemaphoreHandle_t lock;
    log_level_t min_level;  // Minimum level to forward
} log_cfg_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

static log_cfg_t g_log_cfg = {
    .socket = -1,
    .connected = false,
    .min_level = LOG_INFO,  // Default: forward INFO and above
    .lock = NULL
};

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS
 * -------------------------------------------------------------- */

// Custom vprintf handler with level filtering
static int tcp_log_vprintf(const char *fmt, va_list args)
{
    char buffer[LOG_MESSAGE_MAX_LEN];
    log_msg_t log_msg;

    // Parse the log level from format string
    // ESP-IDF logs start with level character: "I (123) TAG: message"
    esp_log_level_t msg_level = ESP_LOG_INFO;
    if (fmt[0] == 'E') msg_level = ESP_LOG_ERROR;
    else if (fmt[0] == 'W') msg_level = ESP_LOG_WARN;
    else if (fmt[0] == 'I') msg_level = ESP_LOG_INFO;
    else if (fmt[0] == 'D') msg_level = ESP_LOG_DEBUG;
    else if (fmt[0] == 'V') msg_level = ESP_LOG_VERBOSE;

    // Convert to protocol log level
    log_level_t proto_level;
    switch(msg_level) {
        case ESP_LOG_ERROR: proto_level = LOG_ERROR; break;
        case ESP_LOG_WARN:  proto_level = LOG_WARN; break;
        case ESP_LOG_INFO:  proto_level = LOG_INFO; break;
        case ESP_LOG_DEBUG: proto_level = LOG_DEBUG; break;
        case ESP_LOG_VERBOSE: proto_level = LOG_DEBUG; break; // Map VERBOSE to DEBUG
        default: proto_level = LOG_INFO;
    }

    // Format the message
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);

    // Forward if level meets minimum and socket connected
    if (len > 0 && g_log_cfg.connected && proto_level >= g_log_cfg.min_level) {
        xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);

        log_msg.magic = PROTOCOL_MAGIC_LOG;
        log_msg.level = proto_level;
        strncpy(log_msg.message, buffer, LOG_MESSAGE_MAX_LEN - 1);
        log_msg.message[LOG_MESSAGE_MAX_LEN - 1] = '\0';

        send(g_log_cfg.socket, &log_msg, sizeof(log_msg), 0);

        xSemaphoreGive(g_log_cfg.lock);
    }

    // Always output to UART for local debugging
    return vprintf(fmt, args);
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialize TCP g_log_cfg with default level
esp_err_t log_init(const char *server_ip, uint16_t port, log_level_t min_level)
{
    esp_err_t err = ESP_OK;

    if (!g_log_cfg.lock) {
        // Create mutex
        err = ESP_ERR_NO_MEM;
        g_log_cfg.lock = xSemaphoreCreateMutex();
        if (g_log_cfg.lock) {

            xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);

            g_log_cfg.min_level = min_level;
            // Create socket if not already created
            err = ESP_OK;
            if (g_log_cfg.socket < 0) {
                err = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (err >= 0) {
                    g_log_cfg.socket = err;
                    err = ESP_OK;
                    // Configure server address
                    g_log_cfg.log_server.sin_family = AF_INET;
                    g_log_cfg.log_server.sin_port = htons(port);
                    inet_pton(AF_INET, server_ip, &g_log_cfg.log_server.sin_addr);
                    
                    // Connect to the log server
                    err = connect(g_log_cfg.socket,
                                  (struct sockaddr *) &g_log_cfg.log_server, 
                                  sizeof(g_log_cfg.log_server));
                    if (err == 0) {
                        g_log_cfg.connected = true;
                    } else {
                        ESP_LOGE(TAG, "Failed to connect to log server!");
                        close(g_log_cfg.socket);
                        g_log_cfg.socket = -1;
                    }
                } else {
                    ESP_LOGE(TAG, "Unable to create log socket!");
                }
            }

            xSemaphoreGive(g_log_cfg.lock);

            if (err == ESP_OK) {
                // Set vprintf handler
                esp_log_set_vprintf(tcp_log_vprintf);
                ESP_LOGI(TAG, "Logs will be forwarded to %s:%d, log level %d.",
                         server_ip, port, g_log_cfg.min_level);
            }
        }
    }

    return err;
}

// Deinitialise logging
void log_deinit(void)
{
    // Restore default logging
    esp_log_set_vprintf(vprintf);

    if (g_log_cfg.lock) {

        ESP_LOGI(TAG, "Stopping log forwarding.");

        xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);

        // Close the socket
        if (g_log_cfg.socket >= 0) {
            close(g_log_cfg.socket);
            g_log_cfg.socket = -1;
        }

        xSemaphoreGive(g_log_cfg.lock);
    }
}

// Set minimum log level to forward
esp_err_t log_set_min_level(log_level_t level)
{
    esp_err_t err = ESP_ERR_INVALID_STATE;

    if (g_log_cfg.lock) {

        xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);

        g_log_cfg.min_level = level;

        xSemaphoreGive(g_log_cfg.lock);

        ESP_LOGI(TAG, "Log level set to %d.", g_log_cfg.min_level);
    }

    return err;
}

// End of file

