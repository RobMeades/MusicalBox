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
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "errno.h"
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
    bool running;
    TaskHandle_t task_handle;
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

// Task to reconnect to the log server on failure.
// This is _extremely_ complex because the socket is non-blocking
// and because the reconnect process can take a while, causing the
// task watchdog to go off.
static void log_reconnect_task(void *arg)
{
    (void) arg;

    esp_task_wdt_add(NULL);

    const int32_t connect_timeout_ms = 5000;
    const int32_t wdt_feed_interval_ms = 100;

    while (g_log_cfg.running) {
        if (!g_log_cfg.connected) {
            ESP_LOGE(TAG, "Reconnecting to log server...");

            // Try to take lock with timeout
            if (xSemaphoreTake(g_log_cfg.lock, pdMS_TO_TICKS(1000)) == pdTRUE) {

                // Close old socket if it exists
                if (g_log_cfg.socket >= 0) {
                    close(g_log_cfg.socket);
                    g_log_cfg.socket = -1;
                }

                // Create new socket
                g_log_cfg.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (g_log_cfg.socket >= 0) {
                    // Make socket non-blocking
                    int32_t flags = fcntl(g_log_cfg.socket, F_GETFL, 0);
                    fcntl(g_log_cfg.socket, F_SETFL, flags | O_NONBLOCK);

                    // Initiate non-blocking connect
                    int32_t rc = connect(g_log_cfg.socket,
                                         (struct sockaddr *) &g_log_cfg.log_server,
                                         sizeof(g_log_cfg.log_server));

                    xSemaphoreGive(g_log_cfg.lock);

                    if (rc == 0) {
                        // Connected immediately
                        xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);
                        g_log_cfg.connected = true;
                        xSemaphoreGive(g_log_cfg.lock);
                        ESP_LOGI(TAG, "Reconnected immediately.");
                    } else if (rc < 0 && errno == EINPROGRESS) {
                        // Connection in progress - poll for completion
                        int32_t elapsed = 0;
                        bool connected = false;

                        while (elapsed < connect_timeout_ms && !connected && g_log_cfg.running) {
                            fd_set fdset;
                            struct timeval tv;

                            FD_ZERO(&fdset);
                            FD_SET(g_log_cfg.socket, &fdset);
                            tv.tv_sec = 0;
                            tv.tv_usec = wdt_feed_interval_ms * 1000;

                            int32_t sel_rc = select(g_log_cfg.socket + 1, NULL, &fdset, NULL, &tv);

                            // Feed watchdog
                            esp_task_wdt_reset();

                            if (sel_rc > 0) {
                                int32_t so_error;
                                socklen_t len = sizeof(so_error);
                                getsockopt(g_log_cfg.socket, SOL_SOCKET, SO_ERROR, &so_error, &len);

                                if (so_error == 0) {
                                    connected = true;
                                    break;
                                } else {
                                    ESP_LOGE(TAG, "Connect failed: %" PRId32 " (%s)", so_error, strerror(so_error));
                                    break;
                                }
                            } else if (sel_rc < 0) {
                                ESP_LOGE(TAG, "Select error: %d (%s)", errno, strerror(errno));
                                break;
                            }

                            elapsed += wdt_feed_interval_ms;
                        }

                        xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);

                        if (connected) {
                            g_log_cfg.connected = true;
                            ESP_LOGI(TAG, "Reconnected after %" PRId32 " ms.", elapsed);
                        } else {
                            ESP_LOGE(TAG, "Connect timeout after %" PRId32 " ms", connect_timeout_ms);
                            close(g_log_cfg.socket);
                            g_log_cfg.socket = -1;
                        }

                        xSemaphoreGive(g_log_cfg.lock);
                    } else {
                        // Connect failed immediately
                        xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);
                        ESP_LOGE(TAG, "Connect failed immediately: %d (%s)", errno, strerror(errno));
                        close(g_log_cfg.socket);
                        g_log_cfg.socket = -1;
                        xSemaphoreGive(g_log_cfg.lock);
                    }
                } else {
                    xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);
                    ESP_LOGE(TAG, "Unable to create new socket %d (%s)!", errno, strerror(errno));
                    xSemaphoreGive(g_log_cfg.lock);
                }
            } else {
                ESP_LOGE(TAG, "Could not take lock, skipping reconnect attempt");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(wdt_feed_interval_ms));
        esp_task_wdt_reset();
    }

    ESP_LOGI(TAG, "Log reconnect task exiting.");
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

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
    int32_t len = vsnprintf(buffer, sizeof(buffer), fmt, args);

    // Forward if level meets minimum and socket connected
    if (len > 0 && g_log_cfg.connected && proto_level >= g_log_cfg.min_level) {

        if (xSemaphoreTake(g_log_cfg.lock, pdMS_TO_TICKS(100)) == pdTRUE) {

            log_msg.magic = PROTOCOL_MAGIC_LOG;
            log_msg.level = proto_level;
            strncpy(log_msg.message, buffer, LOG_MESSAGE_MAX_LEN - 1);
            log_msg.message[LOG_MESSAGE_MAX_LEN - 1] = '\0';

            if (send(g_log_cfg.socket, &log_msg, sizeof(log_msg), MSG_DONTWAIT) < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Buffer full, try again later? Or just drop
                    ESP_LOGD(TAG, "Log send would block, dropping message");
                } else {
                    ESP_LOGI(TAG, "send() failed %d (%s)!", errno, strerror(errno));
                    g_log_cfg.connected = false;
                }
            }

            xSemaphoreGive(g_log_cfg.lock);
        }
    }

    // Always output to UART for local debugging
    return vprintf(fmt, args);
}

// Wot it says.
static void clean_up()
{
    // Restore default logging
    esp_log_set_vprintf(vprintf);

    if (g_log_cfg.lock) {

        ESP_LOGI(TAG, "Stopping log forwarding.");

        xSemaphoreTake(g_log_cfg.lock, portMAX_DELAY);

        // Let the reconnect task exit
        g_log_cfg.running = false;
        vTaskDelay(1000);

        // Close the socket
        if (g_log_cfg.socket >= 0) {
            close(g_log_cfg.socket);
            g_log_cfg.socket = -1;
        }

        xSemaphoreGive(g_log_cfg.lock);
        // Don't delete the semaphore, someone might have it still
    }
}

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Initialize TCP g_log_cfg with default level
esp_err_t log_init(const char *server_ip, uint16_t port, log_level_t min_level)
{
    esp_err_t err = ESP_OK;

    if (!g_log_cfg.running) {
        if (!g_log_cfg.lock) {
            // Create mutex
            err = ESP_ERR_NO_MEM;
            g_log_cfg.lock = xSemaphoreCreateMutex();
        }

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
                         // Set socket to non-blocking to avoid being a hog
                        int flags = fcntl(g_log_cfg.socket, F_GETFL, 0);
                        fcntl(g_log_cfg.socket, F_SETFL, flags | O_NONBLOCK);
                        g_log_cfg.connected = true;
                        g_log_cfg.running= true;
                        // Start a task to reconnect in the background on failure
                        if (xTaskCreate(&log_reconnect_task, "log_reconnect_task", 1024 * 4, NULL, 5, &g_log_cfg.task_handle) != pdPASS) {
                            err = ESP_ERR_NO_MEM;
                            ESP_LOGE(TAG, "Failed to create reconnect task %d (%s)!", errno, strerror(errno));
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to connect to log server %d (%s)!", errno, strerror(errno));
                        close(g_log_cfg.socket);
                        g_log_cfg.socket = -1;
                    }
                } else {
                    ESP_LOGE(TAG, "Unable to create log socket %d (%s)!", errno, strerror(errno));
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

    if (err != ESP_OK) {
        clean_up();
    }

    return err;
}

// Deinitialise logging
void log_deinit(void)
{
    clean_up();
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

