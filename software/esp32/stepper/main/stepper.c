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
 * @brief Implementation of an OTA_updated ESP32-based stepper motor driver.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "errno.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_phy_init.h"
#include "driver/uart.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

// OTA buffer
#define OTA_BUFFER_SIZE 1024

// Downloaded file header buffer
#define OTA_FILE_HEADER_BUFFER_SIZE 8192

// SHA-256 digest length
#define HASH_LEN 32 

// OTA URL buffer
#define OTA_URL_SIZE 256

// UART buffer
#define UART_RX_BUFFFER_SIZE 256

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

static const char *TAG = "stepper";

// An OTA data write buffer ready to write to the flash
static char g_ota_write_data[OTA_BUFFER_SIZE + 1] = { 0 };

// Buffer for accumulating header data
static char g_header_buffer[OTA_FILE_HEADER_BUFFER_SIZE] = { 0 };
extern const uint8_t g_server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t g_server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// Semaphore for WiFi synchronization
SemaphoreHandle_t g_wifi_semaphore = NULL;

// Wifi network interface
esp_netif_t *g_sta_netif = NULL;

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
 * STATIC FUNCTIONS: EVENT HANDLERS
 * -------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected to AP");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGI(TAG, "WiFi disconnected, reason: %d", event->reason);
        
        // Print the AP info that failed
        ESP_LOGI(TAG, "AP SSID: %.32s", event->ssid);
        ESP_LOGI(TAG, "AP BSSID: %02x:%02x:%02x:%02x:%02x:%02x",
                event->bssid[0], event->bssid[1], event->bssid[2],
                event->bssid[3], event->bssid[4], event->bssid[5]);
        ESP_LOGI(TAG, "Reason: %d", event->reason);

        ESP_LOGI(TAG, "Attempting to reconnect...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_AUTHMODE_CHANGE) {
        wifi_event_sta_authmode_change_t *event = (wifi_event_sta_authmode_change_t*) event_data;
        ESP_LOGI(TAG, "Authmode changed from %d to %d", 
                 event->old_mode, event->new_mode);
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        SemaphoreHandle_t semaphore = (SemaphoreHandle_t) arg;
        if (semaphore != NULL) {
            xSemaphoreGive(semaphore);
        }
    }
}

// Return a hostname string from a URL.  The string is
// returned in the given buffer and the string length
// (i.e. what strlen() would return) is the return value.
// IMPORTANT the length returned may be larger than
// buffer_len if buffer_len is not big enough to hold
// the (null terminated) string, i.e. buffer_len must be
// at least the expected host name length plus one.
static int hostname_from_url(char *url, char *buffer, int buffer_len)
{
    int length = 0;
    char *hostname_start = strstr(url, "//");
    int buffer_used = 0;

    if (hostname_start && buffer_len > 0) {
        hostname_start += 2;
        for (char *p = hostname_start;
             (*p != 0) && (*p != '/') && (*p != ':');
             p++) {
            if (buffer_used < buffer_len - 1) {
                buffer[buffer_used] = *p;
                buffer_used++;
            }
            length++;
        }
        // Add terminator
        buffer[buffer_used] = 0;
    }

    return length;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MISC
 * -------------------------------------------------------------- */

 static void infinite_loop(void)
 {
     int i = 0;
     ESP_LOGI(TAG, "When new firmware is available on the server, press the reset button to download it.");
     while(1) {
         ESP_LOGI(TAG, "Waiting for a new firmware ... %d", ++i);
         vTaskDelay(2000 / portTICK_PERIOD_MS);
     }
 }
 
 
static void print_sha256 (const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s: %s", label, hash_print);
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: CLEAN-UP
 * -------------------------------------------------------------- */

static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void __attribute__((noreturn)) task_fatal_error()
{
    ESP_LOGE(TAG, "Exiting task due to fatal error...");
    vTaskDelete(NULL);

    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();

    while(1) {}
}


/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: PING THE SERVER
 * -------------------------------------------------------------- */

// Call back to ping session called on receipt of a successful ping response.
static void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint8_t ttl;
    uint16_t seqno;
    uint32_t elapsed_time, recv_len;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    ESP_LOGI(TAG, "ping: %" PRIu32 " bytes from %s icmp_seq=%" PRIu16 " ttl=%" PRIu16 " time=%" PRIu32 " ms",
        recv_len, ipaddr_ntoa((ip_addr_t*)&target_addr), seqno, ttl, elapsed_time);
}

// Call back to ping session called on a ping timeout.
static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    ESP_LOGI(TAG, "ping: from %s icmp_seq=%d timeout",ipaddr_ntoa((ip_addr_t*)&target_addr), seqno);
}

// Call back to ping session called at end.
static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args)
{
    ip_addr_t target_addr;
    uint32_t transmitted;
    uint32_t received;
    uint32_t total_time_ms;
    uint32_t loss;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));

    if (transmitted > 0) {
        loss = (uint32_t)((1 - ((float)received) / transmitted) * 100);
    } else {
        loss = 0;
    }
    ESP_LOGI(TAG, "ping: %" PRIu32 " packets transmitted, %" PRIu32 " received, %" PRIu32 "%% packet loss, average time %" PRIu32 " ms",
                  transmitted, received, loss, total_time_ms / received);
    // delete the ping sessions, so that we clean up all resources and can create a new ping session
    // we don't have to call delete function in the callback, instead we can call delete function from other tasks
    esp_ping_delete_session(hdl);
}

// Ping the given host name, which should be a null-terminated string.
static esp_err_t ping_start(char *hostname_str)
{
    esp_err_t err = ESP_FAIL;
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    struct addrinfo hint;
    struct addrinfo *res = NULL;
    memset(&hint, 0, sizeof(hint));
 
    /* convert ip4 string or hostname to ip4 address */
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));

    if (getaddrinfo(hostname_str, NULL, &hint, &res) != 0) {
        ESP_LOGI(TAG, "ping: unknown host: %s", hostname_str);
    } else {
        if (res->ai_family == AF_INET) {
            struct in_addr addr4 = ((struct sockaddr_in *) (res->ai_addr))->sin_addr;
            inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
        }
        config.target_addr = target_addr;
        /* set callback functions */
        esp_ping_callbacks_t cbs = {
            .cb_args = NULL,
            .on_ping_success = cmd_ping_on_ping_success,
            .on_ping_timeout = cmd_ping_on_ping_timeout,
            .on_ping_end = cmd_ping_on_ping_end
        };
        esp_ping_handle_t ping;
        err = esp_ping_new_session(&config, &cbs, &ping);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ping: failed to initialise ping session");
        } else {
            err = esp_ping_start(ping);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "ping: failed to start ping to \"%s\"", hostname_str);
            }
        }
    }
    freeaddrinfo(res);

    return err;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: OTA
 * -------------------------------------------------------------- */

// Check if we have enough data to parse the firmware header
//
// The ESP32 firmware header consists of:
//   - esp_image_header_t (8 bytes)
//   - esp_image_segment_header_t (8 bytes) 
//   - esp_app_desc_t (256 bytes)
//
// Total: 272 bytes minimum
static bool has_complete_header(size_t accumulated_size)
{
    // We need at least the image header + first segment header + app descriptor
    size_t header_min_size = sizeof(esp_image_header_t) + 
                             sizeof(esp_image_segment_header_t) + 
                             sizeof(esp_app_desc_t);    
    size_t header_safe_size = header_min_size + 1024; // Safety margin
    
    return accumulated_size >= header_safe_size;
}

// Helper function to get minimum header size
static size_t has_complete_header_min_size(void)
{
    return sizeof(esp_image_header_t) + 
           sizeof(esp_image_segment_header_t) + 
           sizeof(esp_app_desc_t) + 1024;  // + safety margin
}

// Parse and validate firmware header from accumulated buffer
// return 1 if OTA should proceed, 0 if no update needed, -1 on error
static int parse_firmware_header(const char *buffer, size_t buffer_len,
                                 const esp_partition_t *running,
                                 esp_ota_handle_t *update_handle,
                                 const esp_partition_t *update_partition)
{
    esp_app_desc_t new_app_info;
    
    // Extract app description from the accumulated data
    memcpy(&new_app_info, 
           &buffer[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], 
           sizeof(esp_app_desc_t));
    
    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
    }

    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
    esp_app_desc_t invalid_app_info;
    if (last_invalid_app != NULL && 
        esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
        ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
    }

    // Check if this is the same as the last invalid version
    if (last_invalid_app != NULL) {
        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
            ESP_LOGW(TAG, "New version is the same as previously invalid version.");
            ESP_LOGW(TAG, "The firmware with version %s previously failed to boot.", invalid_app_info.version);
            ESP_LOGW(TAG, "To prevent boot loop, we will not install this version.");
            return -1;  // This is still an error - don't install known bad version
        }
    }

    // Check if this is the same as the currently running version
    if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0) {
        ESP_LOGW(TAG, "Current running version is the same as the new version.");
        ESP_LOGW(TAG, "No update needed - already running %s", running_app_info.version);
        return 0;  // Success, but no update needed
    }

    // New valid version - begin OTA operation
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "esp_ota_begin succeeded");
    
    // Write any accumulated data that hasn't been written yet
    if (buffer_len > 0) {
        err = esp_ota_write(*update_handle, (const void *)buffer, buffer_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed for accumulated data");
            esp_ota_abort(*update_handle);
            return -1;
        }
        ESP_LOGI(TAG, "Wrote accumulated %d bytes to flash", buffer_len);
    }
    
    return 1;  // Update in progress
}

/// OTA Task
static void ota_task(void *pvParameter)
{
    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA task");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08"PRIx32", but running from offset 0x%08"PRIx32,
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08"PRIx32")",
             running->type, running->subtype, running->address);

    esp_http_client_config_t config = {
        .url = CONFIG_STEPPER_FIRMWARE_UPG_URL,
        .cert_pem = (char *)g_server_cert_pem_start,
        .timeout_ms = CONFIG_STEPPER_OTA_RECV_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size = 2048,  /* Slightly larger buffer for better throughput */
    };

#ifdef CONFIG_STEPPER_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        task_fatal_error();
    }
    
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        task_fatal_error();
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Content-Length: %d", content_length);

    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%"PRIx32,
             update_partition->subtype, update_partition->address);

    int binary_file_length = 0;
    bool image_header_was_checked = false;
    size_t header_accumulated = 0;
    int zero_read_count = 0;
    const int max_zero_reads = 10;  // Maximum consecutive zero reads before considering connection dead
    
    // State variables to control the loop
    bool transfer_complete = false;
    bool connection_error = false;
    bool same_version_detected = false;
    
    // Deal with all receive packets
    while (!transfer_complete && !connection_error && !same_version_detected) {
        int data_read = esp_http_client_read(client, g_ota_write_data, OTA_BUFFER_SIZE);
        
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            http_cleanup(client);
            task_fatal_error();  // This function doesn't return
        } else if (data_read > 0) {
            // Reset zero read counter on successful read
            zero_read_count = 0;
            
            if (image_header_was_checked == false) {
                // Accumulate data until we have enough to parse the header
                if (header_accumulated + data_read <= OTA_FILE_HEADER_BUFFER_SIZE) {
                    memcpy(g_header_buffer + header_accumulated, g_ota_write_data, data_read);
                    header_accumulated += data_read;
                    ESP_LOGI(TAG, "Accumulated %d/%d bytes for header", header_accumulated, OTA_FILE_HEADER_BUFFER_SIZE);
                } else {
                    ESP_LOGE(TAG, "Header buffer overflow!");
                    http_cleanup(client);
                    task_fatal_error();
                }
                
                /* Check if we have accumulated enough data to parse the header */
                if (has_complete_header(header_accumulated)) {
                    ESP_LOGI(TAG, "Complete header accumulated (%d bytes)", header_accumulated);
                    
                    int parse_result = parse_firmware_header(g_header_buffer, header_accumulated, 
                                                             running, &update_handle, update_partition);

                    if (parse_result == 1) {
                        // Update in progress
                        image_header_was_checked = true;
                        binary_file_length = header_accumulated;
                        ESP_LOGI(TAG, "New version detected, continuing with OTA...");
                        // Will continue to next iteration naturally
                    } else if (parse_result == 0) {
                        // Same version - no update needed
                        ESP_LOGI(TAG, "Already running latest version, no update needed");
                        http_cleanup(client);
                        
                        // Clean up
                        if (update_handle) {
                            esp_ota_abort(update_handle);
                        }
                        
                        // Set flag to exit loop and go to infinite loop
                        same_version_detected = true;
                    } else { /* parse_result == -1 */
                        // Fatal error (like invalid version that previously failed)
                        ESP_LOGE(TAG, "Firmware header validation failed");
                        http_cleanup(client);
                        if (update_handle) {
                            esp_ota_abort(update_handle);
                        }
                        task_fatal_error();
                    }
                } else {
                    ESP_LOGI(TAG, "Still accumulating header data (need more than %zu bytes, currently at %zu)", 
                             has_complete_header_min_size(), header_accumulated);
                    // Need to keep accumulating - will continue naturally
                }
            } else {
                // Header already processed, write data directly
                err = esp_ota_write(update_handle, (const void *)g_ota_write_data, data_read);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_write failed");
                    http_cleanup(client);
                    esp_ota_abort(update_handle);
                    task_fatal_error();
                }
                binary_file_length += data_read;
            }
        } else if (data_read == 0) {
            // Handle zero read - this could be temporary or permanent
            zero_read_count++;
            ESP_LOGI(TAG, "Zero read #%d, header_accumulated=%d, header_checked=%d", 
                     zero_read_count, header_accumulated, image_header_was_checked);
            
            // If we haven't checked the header yet, we need to be careful
            if (!image_header_was_checked) {
                // We haven't accumulated enough header data yet - this zero read might be temporary
                if (esp_http_client_is_complete_data_received(client) == true) {
                    // Server says transfer is complete, but we don't have enough header data
                    ESP_LOGE(TAG, "Connection closed before accumulating enough header data!");
                    ESP_LOGE(TAG, "Accumulated only %zu bytes, need at least %zu", 
                             header_accumulated, has_complete_header_min_size());
                    http_cleanup(client);
                    task_fatal_error();
                } else if (zero_read_count >= max_zero_reads) {
                    // Too many consecutive zero reads - connection might be dead
                    ESP_LOGE(TAG, "Too many zero reads (%d) while accumulating header", zero_read_count);
                    http_cleanup(client);
                    task_fatal_error();
                } else {
                    // Temporary zero read - wait a bit and continue
                    ESP_LOGI(TAG, "Temporary zero read during header accumulation, waiting...");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    // Will continue naturally
                }
            } else {
                // We've already processed the header, now handle zero reads normally
                if (errno == ECONNRESET || errno == ENOTCONN) {
                    ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
                    connection_error = true;
                }
                else if (esp_http_client_is_complete_data_received(client) == true) {
                    ESP_LOGI(TAG, "Connection closed - transfer complete");
                    transfer_complete = true;
                }
                else if (zero_read_count >= max_zero_reads) {
                    ESP_LOGE(TAG, "Too many zero reads (%d) after header, connection may be dead", zero_read_count);
                    connection_error = true;
                }
                else {
                    // Small delay to avoid busy-looping when no data available
                    ESP_LOGI(TAG, "Temporary zero read, waiting for more data...");
                    vTaskDelay(pdMS_TO_TICKS(50));
                    // Will continue naturally
                }
            }
        }
    }
    
    // Handle the different exit conditions
    if (same_version_detected) {
        // Go to infinite loop (this function doesn't return)
        infinite_loop();
    }
    
    if (connection_error) {
        ESP_LOGE(TAG, "Connection error during OTA");
        http_cleanup(client);
        if (update_handle) {
            esp_ota_abort(update_handle);
        }
        task_fatal_error();
    }
    
    // Normal completion path
    ESP_LOGI(TAG, "Total write binary data length: %d", binary_file_length);
    
    if (esp_http_client_is_complete_data_received(client) != true) {
        ESP_LOGE(TAG, "Error in receiving complete file");
        http_cleanup(client);
        if (update_handle) {
            esp_ota_abort(update_handle);
        }
        task_fatal_error();
    }

    // Only call esp_ota_end if we've started the OTA operation
    if (update_handle) {
        err = esp_ota_end(update_handle);
        if (err != ESP_OK) {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            } else {
                ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
            }
            http_cleanup(client);
            task_fatal_error();
        }
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        http_cleanup(client);
        task_fatal_error();
    }
    
    ESP_LOGI(TAG, "Prepare to restart system!");
    esp_restart();
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: INITIALISATION
 * -------------------------------------------------------------- */

// Initialise the non-volatile memory
static esp_err_t nvs_init()
{
    uint8_t sha_256[HASH_LEN] = { 0 };
    esp_partition_t partition;

    // Get sha256 digest for the partition table
    partition.address   = ESP_PARTITION_TABLE_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_MAX_LEN;
    partition.type      = ESP_PARTITION_TYPE_DATA;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for the partition table: ");

    // Get sha256 digest for bootloader
    partition.address   = ESP_BOOTLOADER_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_OFFSET;
    partition.type      = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // Get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "No diagnostic, continuing execution ...");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Initialize NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // If this happens, we erase NVS partition and initialize NVS again.
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err == ESP_OK) {
            err = nvs_flash_init();
        } else {
            ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(erase_err));
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
    }

    return err;
}

// Initialise networking: on exit g_sta_netif will be non-NULL,
// as will g_wifi_semaphore; requires the default event loop to
// have been created.
static esp_err_t networking_init()
{
    // Initialize network interface
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize network interface: %s", esp_err_to_name(err));
    }

    // Create semaphore for WiFi synchronization
    if (err == ESP_OK && g_wifi_semaphore == NULL) {
        g_wifi_semaphore = xSemaphoreCreateBinary();
        if (g_wifi_semaphore == NULL) {
            ESP_LOGE(TAG, "Failed to create WiFi semaphore");
            err = ESP_ERR_NO_MEM;
        }
    }

    // Create default WiFi station
    if (err == ESP_OK) {
        g_sta_netif = esp_netif_create_default_wifi_sta();
        if (g_sta_netif == NULL) {
            ESP_LOGE(TAG, "Failed to create default WiFi station");
            err = ESP_ERR_NO_MEM;
        }
    }

    // Initialize WiFi
    if (err == ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(err));
        }
    }

    // Register event handlers for WiFi and IP
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                         &wifi_event_handler, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register WiFi event handler: %s", esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                         &ip_event_handler, g_wifi_semaphore);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        }
    }

    // Set WiFi to station mode
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(err));
        }
    }

    // Configure WiFi connection
    if (err == ESP_OK) {
        wifi_config_t wifi_config = {
            .sta = {
                .ssid = CONFIG_STEPPER_WIFI_SSID,
                .password = CONFIG_STEPPER_WIFI_PASSWORD,
                .threshold.authmode = WIFI_AUTH_OPEN,
                .pmf_cfg = {
                    .capable = true,
                    .required = false
                },
            },
        };

        err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set WiFi configuration: %s", esp_err_to_name(err));
        }
    }

    // Start WiFi
    if (err == ESP_OK) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi connecting to %s...", CONFIG_STEPPER_WIFI_SSID);
        }
    }

    // Wait for IP address (with timeout)
    if (err == ESP_OK) {
        if (xSemaphoreTake(g_wifi_semaphore, pdMS_TO_TICKS(60000)) == pdTRUE) {
            ESP_LOGI(TAG, "WiFi connected, IP obtained");
        } else {
            ESP_LOGE(TAG, "Failed to obtain IP address within timeout");
            err = ESP_ERR_TIMEOUT;
        }
    }

    // Disable WiFi power save mode for best OTA operation
    if (err == ESP_OK) {
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to disable WiFi power save: %s", esp_err_to_name(ps_err));
            // Continue anyway, this is not fatal
        }
    }

    // Clean-up on error
    if (err != ESP_OK) {
        if (g_sta_netif) {
            esp_netif_destroy_default_wifi(g_sta_netif);
            g_sta_netif = NULL;
            // Can't clean up g_wifi_semaphore as it may still be taken
        }
    }

    return err;
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

    // Initialise non-volatile storage
    if (err == ESP_OK) {
        err = nvs_init();
    }

    // Initialize networking
    if (err == ESP_OK) {
        err = networking_init();
    }

    // Create OTA task
    BaseType_t task_created = pdFAIL;
    if (err == ESP_OK) {
        task_created = xTaskCreate(&ota_task, "ota_task", 10240, NULL, 5, NULL);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA task");
            err = ESP_ERR_NO_MEM;
        }
    }

    // Clean-up if initialization failed
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initialization failed, system cannot continue, will restart soonish");
        
        if (g_sta_netif != NULL) {
            esp_netif_destroy_default_wifi(g_sta_netif);
            g_sta_netif = NULL;
        }

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
    int hostname_length = hostname_from_url(CONFIG_STEPPER_FIRMWARE_UPG_URL, hostname_buffer, sizeof(hostname_buffer));
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

