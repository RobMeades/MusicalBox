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
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "../../../protocol/protocol.h"
#include "log.h"
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

// The number of milliseconds to vTaskDelay() for in order to let the idle task
// to feed its watchdog
 #define WATCHDOG_FEED_TIME_MS 10

// The sense resisitor wired to the BRA and BRB pins of the TMC2209,
// the BigTreeTech board uses 100 milliOhms
#define TMC2209_RSENSE_MOHM 110

// The percentage of the run current to apply during hold;
// don't need a lot, let it cool down
#define STEPPER_MOTOR_HOLD_CURRENT_PERCENT 20

// Standard short duration for an LED lash
#define DEBUG_LED_SHORT_MS 50

// Standard short duration for a long LED lash
#define DEBUG_LED_LONG_MS 1000

// How often to check a pin for debounce, in microseconds
#define DEBOUNCE_CHECK_PERIOD_US 1000

// The debounce threshold: if DEBOUNCE_CHECK_PERIOD_US is
// once a milliseconds then a value of 20 here means that the
// sensor will have had to have signalled open for the last
// 20 milliseconds to be open
#define DEBOUNCE_THRESHOLD 20

// The maximum motor current to set, in milliAmps, used only
// in production mode
#define MOTOR_CURRENT_MAX_MA 1000

// The maximum motor current to set for a door, in milliAmps,
// used only in production mode
#define MOTOR_CURRENT_DOOR_MAX_MA 250

// The desired stepper motor velocity and current, only used when not in production mode
#if defined(CONFIG_STEPPER_STAND)
    // The velocity and current for the stand
#   define VELOCITY_MILLIHERTZ (1000 * 64 * 2)
#   define STEPPER_MOTOR_CURRENT_MA 1000
#elif (defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0))
    // The velocity and current for plinky-plonky operation (double normal speed)
#   define VELOCITY_MILLIHERTZ (1000 * 64 * 7 * 2)
#   define STEPPER_MOTOR_CURRENT_MA 1000
#elif (defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0))
    // The velocity and current for door operation
#   define VELOCITY_MILLIHERTZ (1000 * 64 * 2)
#   define STEPPER_MOTOR_CURRENT_MA 150
#elif defined(CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0) && \
      defined(CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
    // The velocity for lift operation
#   define VELOCITY_MILLIHERTZ (1000 * 64 * 10)
#   define STEPPER_MOTOR_CURRENT_MA 1000
#else
    // The velocity and a safe current for testing any motor
#   define VELOCITY_MILLIHERTZ (1000 * 64 * 2)
#   define STEPPER_MOTOR_CURRENT_MA 150
#endif

// How long to wait before checking the lift sensors so that
// we don't get ourselves stuck moving out of the up or down
// positions.
#define HYSTERESIS_LIFT_MS 3000

// How long to wait before checking the plinky-plonky sensor
// so that we are able to move away from it.
#define HYSTERESIS_PLINKY_PLONKY_MS 500

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

// Production mode message parser state.
typedef enum {
  MSG_PARSER_STATE_NEED_MAGIC = 0,
  MSG_PARSER_STATE_NEED_CMD_BODY,
  MSG_PARSER_STATE_NEED_QRY_BODY
} msg_parser_state_t;

// Structure to hold a received command or query.
typedef struct {
    bool cmd_not_qry;
    union {
        cmd_msg_t cmd;
        qry_msg_t qry;
        uint8_t buffer[PROTOCOL_ESP32_MAX_RX_LEN];
    };
} cmd_or_qry_t;

// The context data for the message parser.
typedef struct {
    msg_parser_state_t msg_parser_state;
    cmd_or_qry_t cmd_or_qry;
    size_t buffer_index;
} context_parser_t;

// The context data for state.
typedef struct context_state_t {
    state_t init;
    state_t current_state;
    state_t target_state;
    bool cmd_running;
    int64_t start_time_microseconds;
    int32_t timeout_ms;
    bool (*stop_callback)(struct context_state_t *);
} context_state_t;

// The context data for sensor monitoring.
typedef struct {
    bool is_down;
    bool is_at_limit;
    bool is_at_reference;
    bool is_open;
} context_sensor_t;

// The context data for debouncing.
typedef struct {
    esp_timer_create_args_t timer_args;
    esp_timer_handle_t timer_handle;
    size_t triggered_count_is_down;
    size_t triggered_count_is_at_limit;
    size_t triggered_count_is_at_reference;
    size_t triggered_count_is_open;
} context_debounce_t;

// The context data for production mode.
typedef struct {
    int socket;
    bool connected;
    bool running;
    struct sockaddr_in server;
    SemaphoreHandle_t lock;
    TaskHandle_t task_handle_comms_rx;
    TaskHandle_t task_handle_monitor;
    context_parser_t context_parser;
    context_state_t context_state;
    context_sensor_t context_sensor;
    context_debounce_t context_debounce;
} context_production_t;

/* ----------------------------------------------------------------
 * VARIABLES
 * -------------------------------------------------------------- */

// A place to remember the handle of the stall task if created.
static TaskHandle_t g_stall_task_handle = NULL;

// A semaphore to let diag_interrupt_handler signal the stall task.
static SemaphoreHandle_t g_diag_semaphore = NULL;

#if defined(CONFIG_STEPPER_PRODUCTION_MODE)
// The production context
static context_production_t g_context_production = {0};
#else
#  if (defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)) || \
      (defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0))
// A count variable for sensor debouncing, only used when
// not in producion mode.
  static size_t g_sensor_triggered_count = 0;
#  endif
#endif

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MISC
 * -------------------------------------------------------------- */

// Flash the debug LED
static void flash_debug_led(int32_t duration_ms)
{
#if defined(CONFIG_STEPPER_DEBUG_LED_PIN) && (CONFIG_STEPPER_DEBUG_LED_PIN >= 0)
    gpio_set_level(CONFIG_STEPPER_DEBUG_LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(CONFIG_STEPPER_DEBUG_LED_PIN, 1);
#endif
}

/**
 * Creates a hex dump of data in a provided buffer
 * (written by DeepSeek 'cos I couldn't find my own
 * hex print routine and got lazy).
 *
 * @param data          Input data to dump
 * @param data_size     Size of input data in bytes
 * @param output        Output buffer to fill with hex dump
 * @param output_size   Size of output buffer
 * @return              Number of characters written (excluding
 *                      null terminator), or -1 if output
 *                      buffer is too small
 */
int32_t hex_dump_to_buffer(const void *data, size_t data_size,
                           char *output, size_t output_size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    char *out_ptr = output;
    size_t remaining = output_size;
    int32_t total_written = 0;

    if (output_size == 0) return -1;

    for (size_t i = 0; i < data_size; i += 16) {
        int32_t line_written;

        // Print offset (8 hex digits + space)
        line_written = snprintf(out_ptr, remaining, "%08zx  ", i);
        if (line_written < 0 || (size_t)line_written >= remaining) {
            output[output_size - 1] = '\0';
            return -1;
        }
        out_ptr += line_written;
        remaining -= line_written;
        total_written += line_written;

        // Print hex bytes
        for (size_t j = 0; j < 16; j++) {
            if (i + j < data_size) {
                line_written = snprintf(out_ptr, remaining, "%02x ", bytes[i + j]);
            } else {
                line_written = snprintf(out_ptr, remaining, "   ");
            }

            if (line_written < 0 || (size_t)line_written >= remaining) {
                output[output_size - 1] = '\0';
                return -1;
            }
            out_ptr += line_written;
            remaining -= line_written;
            total_written += line_written;

            // Add extra space in the middle
            if (j == 7) {
                if (remaining < 1) {
                    output[output_size - 1] = '\0';
                    return -1;
                }
                *out_ptr++ = ' ';
                remaining--;
                total_written++;
            }
        }

        // Print ASCII representation
        line_written = snprintf(out_ptr, remaining, " |");
        if (line_written < 0 || (size_t)line_written >= remaining) {
            output[output_size - 1] = '\0';
            return -1;
        }
        out_ptr += line_written;
        remaining -= line_written;
        total_written += line_written;

        for (size_t j = 0; j < 16 && i + j < data_size; j++) {
            unsigned char c = bytes[i + j];
            if (remaining < 1) {
                output[output_size - 1] = '\0';
                return -1;
            }
            *out_ptr++ = isprint(c) ? c : '.';
            remaining--;
            total_written++;
        }

        // Close ASCII section and add newline
        line_written = snprintf(out_ptr, remaining, "|\n");
        if (line_written < 0 || (size_t)line_written >= remaining) {
            output[output_size - 1] = '\0';
            return -1;
        }
        out_ptr += line_written;
        remaining -= line_written;
        total_written += line_written;
    }

    // Ensure null termination
    if (remaining > 0) {
        *out_ptr = '\0';
    } else {
        output[output_size - 1] = '\0';
        return -1;
    }

    return total_written;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: MOTOR RELATED
 * -------------------------------------------------------------- */

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

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: SENSOR RELATED (PRODUCTION AND NON-PRODUCTION)
 * -------------------------------------------------------------- */

#if !defined(CONFIG_STEPPER_PRODUCTION_MODE)

#  if (defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)) || \
      (defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0)))

#    if defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0)
// Return true if the plinky-plonky is at its reference position,
// non-production version.
static bool is_at_reference(void)
{
    // Return the result of plinky-plonky sensor_debounce_callback() tracking
    return g_sensor_triggered_count >= DEBOUNCE_THRESHOLD;
}
#    endif

#    if defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)
// Return true if a door is in the open position,
// non-production version.
static bool is_open(void)
{
    // Return the result of door sensor_debounce_callback() tracking
    return g_sensor_triggered_count >= DEBOUNCE_THRESHOLD;
}
#    endif

// Debounce timer callback function for reading a sensor,
// non-production mode version.
static void sensor_debounce_callback(void* arg)
{
    gpio_num_t  pin = (gpio_num_t ) arg;
    if (!gpio_get_level(pin)) {
        // The sensor is pulled low
        g_sensor_triggered_count++;
        if (g_sensor_triggered_count > DEBOUNCE_THRESHOLD) {
            // For the unlikely case of a wrap
            g_sensor_triggered_count = DEBOUNCE_THRESHOLD;
        }
    } else {
        // Not open, reset the count
        g_sensor_triggered_count = 0;
    }
}

#    if defined(CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0)
// Return true if the lift is at a limit, else false, non-production version.
static bool is_at_limit(void)
{
    // It's nice and dark in the lift, doesn't seem to need debouncing
    return !gpio_get_level(CONFIG_STEPPER_LIFT_LIMIT_PIN);
}
#    endif

#    if defined(CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
// Return true if the lift is fully down, non-production version.
static bool is_down(void)
{
    // It's nice and dark in the lift, doesn't seem to need debouncing
    return !gpio_get_level(CONFIG_STEPPER_LIFT_DOWN_PIN);
}
#    endif

#  endif  // (defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)) ||
          // (defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0)))

#else

// Return true if the lift is at a limit.
static bool is_at_limit(void)
{
    return g_context_production.context_debounce.triggered_count_is_at_limit >= DEBOUNCE_THRESHOLD;
}

// Return true if the lift is at a limit but it is not down.
static bool is_up(void)
{
    return (g_context_production.context_debounce.triggered_count_is_at_limit >= DEBOUNCE_THRESHOLD) &&
           (g_context_production.context_debounce.triggered_count_is_down == 0);
}

// Return true if the lift is down.
static bool is_down(void)
{
    return g_context_production.context_debounce.triggered_count_is_down >= DEBOUNCE_THRESHOLD;
}

// Return true if the plinky-plonky is at its reference position,
// production version.
static bool is_at_reference(void)
{
    return g_context_production.context_debounce.triggered_count_is_at_reference >= DEBOUNCE_THRESHOLD;
}

// Return true if a door is open, production version.
static bool is_open(void)
{
    return g_context_production.context_debounce.triggered_count_is_open >= DEBOUNCE_THRESHOLD;
}

#endif // #if (!defined(CONFIG_STEPPER_PRODUCTION_MODE)

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: INITIALISATION
 * -------------------------------------------------------------- */

// Generic initialisation.
esp_err_t init(void)
{

#if defined(CONFIG_STEPPER_ENABLE_PIN) && (CONFIG_STEPPER_ENABLE_PIN >= 0)
    // Stop the motor moving too much before it is configured,
    if (gpio_set_level(CONFIG_STEPPER_ENABLE_PIN, 1) == ESP_OK) {
        gpio_set_direction(CONFIG_STEPPER_ENABLE_PIN, GPIO_MODE_OUTPUT);
    }
#endif

    // Print out our Wi-Fi MAC address, if possible
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        ESP_LOGI(TAG, "MAC address %02X:%02X:%02X:%02X:%02X:%02X", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }

    // Create the default event loop, for everyone's use
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s.", esp_err_to_name(err));
    }

#if defined(CONFIG_STEPPER_DEBUG_LED_PIN) && (CONFIG_STEPPER_DEBUG_LED_PIN >= 0)
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

#if defined(CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0)
    // Configure the lift limit pin
    if (err == ESP_OK) {
        err = gpio_set_direction(CONFIG_STEPPER_LIFT_LIMIT_PIN, GPIO_MODE_INPUT);
    }
#endif

#if defined(CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
    // Configure the lift down pin
    if (err == ESP_OK) {
        err = gpio_set_direction(CONFIG_STEPPER_LIFT_DOWN_PIN, GPIO_MODE_INPUT);
    }
#endif

#if defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)
    // Configure the door open pin
    if (err == ESP_OK) {
        err = gpio_set_direction(CONFIG_STEPPER_DOOR_OPEN_PIN, GPIO_MODE_INPUT);
    }
#endif

#if !defined(CONFIG_STEPPER_NO_WIFI)
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

# if defined(CONFIG_STEPPER_PRODUCTION_MODE)
    // Forward logging to the server
    if (err == ESP_OK) {
        err = log_init(CONFIG_STEPPER_PRODUCTION_SERVER, CONFIG_STEPPER_PRODUCTION_LOG_PORT, LOG_INFO);
    }
# endif

#else
    ESP_LOGW(TAG, "CONFIG_STEPPER_NO_WIFI is defined, not connecting to WiFi.");
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

#if defined(CONFIG_STEPPER_NO_STEPPERS)
    // Make sure the operator knows we're not gonna do nuffin
    ESP_LOGW(TAG, "STEPPER_NO_STEPPERS is defined, steppers will not be enabled.");
    for (size_t x= 0; x < 3; x++) {
        flash_debug_led(DEBUG_LED_LONG_MS);
        vTaskDelay(pdMS_TO_TICKS(DEBUG_LED_LONG_MS));
    }
#endif

    return err;
}

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: DEVELOPMENT STUFF, NON-PRODUCTION MODE
 * -------------------------------------------------------------- */

#if !defined(CONFIG_STEPPER_PRODUCTION_MODE)

#  if defined(CONFIG_STEPPER_STAND)

// Being the stand.
static void be_the_stand(void)
{
    // We're operating the stand
    ESP_LOGI(TAG, "STAND MODE");
    ESP_LOGI(TAG, "Setting velocity.");
    tmc2209_set_velocity(TMC2209_ADDRESS, VELOCITY_MILLIHERTZ);
    size_t run_time_seconds = 5;
    ESP_LOGI(TAG, "Running the motor for up to %d second(s)...", run_time_seconds);
    tmc2209_motor_enable(TMC2209_ADDRESS);
    vTaskDelay(pdMS_TO_TICKS(run_time_seconds * 1000));
    ESP_LOGI(TAG, "Stopping.");
}

# endif // #if defined(CONFIG_STEPPER_STAND)

#  if defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0)

// Being the plinky-plonky.

static void be_the_plinky_plonky(void)
{
    // We're operating the plinky-plonky
    ESP_LOGI(TAG, "PLINKY-PLONKY MODE");
    // Start a timer to read the pin and debounce it
    esp_timer_create_args_t reference_sensor_debounce_timer_args = {sensor_debounce_callback,
                                                                    (void *) CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN,
                                                                    ESP_TIMER_TASK,
                                                                    "plinky-plonky debounce",
                                                                    false};
    esp_timer_handle_t reference_sensor_handle = NULL;
    esp_err_t err = esp_timer_create(&reference_sensor_debounce_timer_args, &reference_sensor_handle);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(reference_sensor_handle, DEBOUNCE_CHECK_PERIOD_US);
    }
    if (err == ESP_OK) {
        bool stop = false;
        ESP_LOGI(TAG, "Setting velocity.");
        tmc2209_set_velocity(TMC2209_ADDRESS, VELOCITY_MILLIHERTZ);
        ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
        ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

        tmc2209_motor_enable(TMC2209_ADDRESS);
        int64_t guard_time_ms = 30000;
        ESP_LOGI(TAG, "Running for up to %lld milliseconds...", guard_time_ms);
        size_t hysteresis_count = 0;
        if (is_at_reference()) {
            // Wait a while after determining we are open before
            // checking again, otherwise we will never move from open
            hysteresis_count = 1000 / WATCHDOG_FEED_TIME_MS / 2;
        }
        stop = false;
        int64_t start_time = esp_timer_get_time();
        while ((!stop && !(esp_timer_get_time() - start_time > (guard_time_ms * 1000)))) {
            // Stop when the reference point is reached
            if ((hysteresis_count == 0)) {
                if (is_at_reference()) {
                    ESP_LOGI("INFO", "At reference, stopping.");
                    stop = true;
                }
            }
            if (hysteresis_count > 0) {
                hysteresis_count--;
            }
            esp_task_wdt_reset();
            // Yield to let the idle task run and reset its watchdog
            vTaskDelay(pdMS_TO_TICKS(WATCHDOG_FEED_TIME_MS));
        }
        tmc2209_motor_disable(TMC2209_ADDRESS);
        ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
        ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));
        ESP_LOGI(TAG, "Run completed.");
    }
    if (reference_sensor_handle != NULL) {
        esp_timer_stop(reference_sensor_handle);
        esp_timer_delete(reference_sensor_handle);
    }
}

#  endif // #if defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0)

#  if defined(CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0) && \
      defined(CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)

// Being the lift.

static void be_the_lift(void)
{
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
        if (is_at_limit()) {
            // Wait three seconds after determining we are at a limit before
            // checking again
            hysteresis_count = loops_per_second * (HYSTERESIS_LIFT_MS / 1000);
        }
        bool stop = false;
        for (size_t x = 0; (x < loops_per_second * guard_time_seconds) && !stop; x++) {
            vTaskDelay(pdMS_TO_TICKS(1000 / loops_per_second));
            ESP_LOGI("INFO", "at limit %s, down %s.", is_at_limit() ? "true" : "false",
                    is_down() ? "true" : "false");
            // Stop when the appropriate limit is hit
            if ((hysteresis_count == 0)) {
                if (is_at_limit()) {
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

        ESP_LOGI(TAG, "Stopping.");
        tmc2209_set_velocity(TMC2209_ADDRESS, 0);
        esp_err_t err = tmc2209_get_position(TMC2209_ADDRESS);
        if (err >= 0) {
            ESP_LOGI(TAG, "Microsteps reading is now %d.", err);
            err = ESP_OK;
        }
        tmc2209_motor_disable(TMC2209_ADDRESS);
        ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
        ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

        ESP_LOGI(TAG, "Run %d of %d completed.", y + 1, repeats);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#  endif // #if defined(CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0) &&
         //     defined(CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)

#  if defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)

// Being a door.
static void be_a_door(void)
{
    // We're operating a door
    ESP_LOGI(TAG, "DOOR MODE");
    // Switch off StallGuard and CoolStep as they don't work well at low speeds
    ESP_LOGI(TAG, "Switching off StallGuard and CoolStep.");
    esp_err_t err = tmc2209_set_stealth_chop_threshold(TMC2209_ADDRESS, UINT32_MAX);
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
        // Start a timer to read the pin and debounce it
        esp_timer_create_args_t door_sensor_debounce_timer_args = {sensor_debounce_callback,
                                                                    (void *) CONFIG_STEPPER_DOOR_OPEN_PIN,
                                                                    ESP_TIMER_TASK,
                                                                    "door debounce",
                                                                    false};
        esp_timer_handle_t door_sensor_handle = NULL;
        if (err == ESP_OK) {
            err = esp_timer_create(&door_sensor_debounce_timer_args, &door_sensor_handle);
            if (err == ESP_OK) {
                err = esp_timer_start_periodic(door_sensor_handle, DEBOUNCE_CHECK_PERIOD_US);
            }
        }
        if (err == ESP_OK) {
            size_t repeats = 10;
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
                tmc2209_set_velocity(TMC2209_ADDRESS, velocity_sign * VELOCITY_MILLIHERTZ);
                ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
                ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

                tmc2209_motor_enable(TMC2209_ADDRESS);
                int64_t close_guard_time_ms = 2550;
                int64_t open_guard_time_ms = 3000;
                ESP_LOGI(TAG, "Running for up to %lld milliseconds...", open_guard_time_ms);
                size_t hysteresis_count = 0;
                if (is_open()) {
                    // Wait a while after determining we are open before
                    // checking again, otherwise we will never move from open
                    hysteresis_count = 1000 / WATCHDOG_FEED_TIME_MS / 2;
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
                    // Yield to let the idle task run and reset its watchdog
                    vTaskDelay(pdMS_TO_TICKS(WATCHDOG_FEED_TIME_MS));
                }
                tmc2209_motor_disable(TMC2209_ADDRESS);
                ESP_LOGI("INFO", "TSTEP %d.", tmc2209_get_tstep(TMC2209_ADDRESS));
                ESP_LOGI("INFO", "SG_RESULT %d.", tmc2209_get_sg_result(TMC2209_ADDRESS));

                ESP_LOGI(TAG, "Run %d of %d completed.", y + 1, repeats);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (door_sensor_handle != NULL) {
            esp_timer_stop(door_sensor_handle);
            esp_timer_delete(door_sensor_handle);
        }
    }
}

#  endif // #if defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)

// Test mode.
static void do_test_mode(void)
{
    ESP_LOGI(TAG, "TEST MODE");
    ESP_LOGI(TAG, "Setting velocity.");
    tmc2209_set_velocity(TMC2209_ADDRESS, VELOCITY_MILLIHERTZ);
    size_t run_time_seconds = 3;
    ESP_LOGI(TAG, "Running the motor for up to %d second(s)...", run_time_seconds);
    tmc2209_motor_enable(TMC2209_ADDRESS);
    vTaskDelay(pdMS_TO_TICKS(run_time_seconds * 1000));
    ESP_LOGI(TAG, "Stopping.");
}

#endif // #if !defined(CONFIG_STEPPER_PRODUCTION_MODE)

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: PRODUCTION MODE, STATES
 * -------------------------------------------------------------- */

#if defined(CONFIG_STEPPER_PRODUCTION_MODE)

// Return true if the stand is rotating in a clockwise direction.
static bool is_state_stand_rotating_clockwise(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return millihertz < 0;
}

// Return true if the stand is rotating in an anticlockwise direction.
static bool is_state_stand_rotating_anticlockwise(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return millihertz > 0;
}

// Return true if the lift is stopped and down.
static bool is_state_lift_stopped_down(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return (millihertz == 0) && is_down();
}

// Return true if the lift is stopped and up.
static bool is_state_lift_stopped_up(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return (millihertz == 0) && is_up();
}

// Return true if the lift is rising.
static bool is_state_lift_rising(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return millihertz > 0;
}

// Return true if the lift is lowering.
static bool is_state_lift_lowering(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return millihertz < 0;
}

// Return true if the plinky-plonky is stopped at the
// reference position.
static bool is_state_plinky_plonky_stopped_at_reference(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return (millihertz == 0) && is_at_reference();
}

// Return true if the plinky-plonky is playing.
static bool is_state_plinky_plonky_playing(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return millihertz != 0;
}

// Return true if a door is stopped and open
static bool is_state_door_stopped_open(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return (millihertz == 0) && is_open();
}

// Return true if a door is opening.
static bool is_state_door_opening(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return millihertz < 0;
}

// Return true if a door is closing.
static bool is_state_door_closing(void)
{
    int32_t millihertz = 0;
    tmc2209_get_velocity(TMC2209_ADDRESS, &millihertz);
    return millihertz > 0;
}

// Return true if we are being the stand, based on the
// init command we should have received at start of day
static bool we_are_stand(state_t init)
{
    return (init >= STATE_STAND_BEGIN) && (init < STATE_STAND_END);
}

// Return true if we are being the lift, based on the
// init command we should have received at start of day
static bool we_are_lift(state_t init)
{
    return (init >= STATE_LIFT_BEGIN) && (init < STATE_LIFT_END);
}

// Return true if we are being the plinky-plonky, based on the
// init command we should have received at start of day
static bool we_are_plinky_plonky(state_t init)
{
    return (init >= STATE_PLINKY_PLONKY_BEGIN) && (init < STATE_PLINKY_PLONKY_END);
}

// Return true if we are being a door, based on the
// init command we should have received at start of day
static bool we_are_door(state_t init)
{
    return (init >= STATE_DOOR_BEGIN) && (init < STATE_DOOR_END);
}

// Get the current state, which depends on what we are being,
// hence the need for the last target state, also updates
// the current state.
// Note: if we've never been given a target state, this will return
// STATE_NULL, we just don't know who we are yet.
static state_t get_state(context_state_t *context)
{
    state_t state = STATE_NULL;

    if (we_are_stand(context->init)) {
        if (is_state_stand_rotating_clockwise()) {
            state = STATE_STAND_ROTATING_CLOCKWISE;
        } else if (is_state_stand_rotating_anticlockwise()) {
            state = STATE_STAND_ROTATING_ANTICLOCKWISE;
        } else {
            state = STATE_STAND_STOPPED;
        }
    } else if (we_are_lift(context->init)) {
        // Got to go through these in the right order,
        // narrowest conditions first
        if (is_state_lift_stopped_up()) {
            state = STATE_LIFT_STOPPED_UP;
        } else if (is_state_lift_stopped_down()) {
            state = STATE_LIFT_STOPPED_DOWN;
        } else if (is_state_lift_rising()) {
            state = STATE_LIFT_RISING;
        } else if (is_state_lift_lowering()) {
            state = STATE_LIFT_LOWERING;
        } else {
            state = STATE_LIFT_STOPPED_UNKNOWN;
        }
    } else if (we_are_plinky_plonky(context->init)) {
        // Got to go through these in the right order,
        // narrowest conditions first
        if (is_state_plinky_plonky_stopped_at_reference()) {
            state = STATE_PLINKY_PLONKY_STOPPED_AT_REFERENCE;
        } else if (is_state_plinky_plonky_playing()) {
            state = STATE_PLINKY_PLONKY_PLAYING;
        } else {
            state = STATE_PLINKY_PLONKY_STOPPED_UNKNOWN;
        }
    } else if (we_are_door(context->init)) {
        // Got to go through these in the right order,
        // narrowest conditions first
        if (is_state_door_stopped_open()) {
            state = STATE_DOOR_STOPPED_OPEN;
        } else if (is_state_door_opening()) {
            state = STATE_DOOR_OPENING;
        } else if (is_state_door_closing()) {
            state = STATE_DOOR_CLOSING;
        } else {
            // Note that we have no way of
            // telling if we're closed or not,
            // i.e. we can't return
            // STATE_DOOR_STOPPED_CLOSED from here
            state = STATE_DOOR_STOPPED_UNKNOWN;
        }
    }

    context->current_state = state;

    return state;
}

#endif // #if defined(CONFIG_STEPPER_PRODUCTION_MODE)

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: PRODUCTION MODE, COMMANDS
 * -------------------------------------------------------------- */

#if defined(CONFIG_STEPPER_PRODUCTION_MODE)

// Send data to the server.
static esp_err_t send_tx_data(uint8_t *buffer, size_t len, int socket)
{
    esp_err_t err = ESP_OK;
    size_t total_written = 0;

    while ((total_written < len) && (err == ESP_OK)) {
        int32_t len_written = send(socket, buffer + total_written, len - total_written, 0);
        if (len_written >= 0) {
            total_written += len_written;
        } else {
            err = -errno;
            ESP_LOGE(TAG, "Error sending to server %d (%s)!", errno, strerror(errno));
        }
    }

    return err;
}

// Stop callback for the lift, downwards bound.
// IMPORTANT: the production context should be locked before this is called.
static bool is_down_callback(context_state_t *context_state)
{
    (void) context_state;
    return is_down();
}


// Stop callback for the lift, upwards bound.
// IMPORTANT: the production context should be locked before this is called.
static bool is_up_callback(context_state_t *context_state)
{
    bool stop = false;

    // Need to wait for the lift to move away from the limit at the bottom
    if (esp_timer_get_time() - context_state->start_time_microseconds > (HYSTERESIS_LIFT_MS * 1000)) {
        stop = is_up();
    }

    return stop;
}

// Stop callback plinky-plonky.
// IMPORTANT: the production context should be locked before this is called.
static bool is_at_reference_callback(context_state_t *context_state)
{
    bool stop = false;

    // Need to allow some time for us to move past the sensor from "park"
    if (esp_timer_get_time() - context_state->start_time_microseconds > (HYSTERESIS_PLINKY_PLONKY_MS * 1000)) {
        stop = is_at_reference();
    }

    return stop;
}

// Stop callback for a door: no need for any hysteresis here.
// IMPORTANT: the production context should be locked before this is called.
static bool is_open_callback(context_state_t *context_state)
{
    (void) context_state;
    return is_open();
}

// Given a command message that is to achieve a target state, filter
// out requests that don't make sense and set up concluding conditions.
// IMPORTANT: the production context should be locked before this is called.
static status_t filter_and_conclude(cmd_msg_t *cmd_msg,
                                    context_state_t *context_state,
                                    bool (**stop_callback)(context_state_t *))
{
    status_t status = STATUS_ERROR_INVALID_PARAM;
    *stop_callback = NULL;

    if ((we_are_door(context_state->init) && (cmd_msg->param_3 <= MOTOR_CURRENT_DOOR_MAX_MA)) ||
        (cmd_msg->param_3 <= MOTOR_CURRENT_MAX_MA))  {
        // Only accept commands that make sense for who we are
        if ((we_are_stand(context_state->init) && (cmd_msg->param_1 >= STATE_STAND_BEGIN) &&
             (cmd_msg->param_1 <= STATE_STAND_END)) ||
            (we_are_lift(context_state->init) && (cmd_msg->param_1 >= STATE_LIFT_BEGIN) &&
             (cmd_msg->param_1 <= STATE_LIFT_END)) ||
            (we_are_plinky_plonky(context_state->init) && (cmd_msg->param_1 >= STATE_PLINKY_PLONKY_BEGIN) &&
             (cmd_msg->param_1 <= STATE_PLINKY_PLONKY_END)) ||
            (we_are_door(context_state->init) && (cmd_msg->param_1 >= STATE_DOOR_BEGIN) &&
             (cmd_msg->param_1 <= STATE_DOOR_END))) {
            status = STATUS_OK;
            // If the command is a "stopped" one, mask
            // any velocity (cmd_msg->param_2) to zero
            if ((cmd_msg->param_1 == STATE_STAND_STOPPED) ||
                (cmd_msg->param_1 == STATE_LIFT_STOPPED_UNKNOWN) ||
                (cmd_msg->param_1 == STATE_PLINKY_PLONKY_STOPPED_UNKNOWN) ||
                (cmd_msg->param_1 == STATE_DOOR_STOPPED_UNKNOWN)) {
                ESP_LOGW(TAG, "\"Stopped\" type target state (0x%04x) received with"
                        " non-zero velocity (%d), zeroing the velocity.",
                        cmd_msg->param_1, cmd_msg->param_2);
                cmd_msg->param_2 = 0;
            }
            // Now, if necessary, set up an end condition associated
            // with a sensor check
            if ((cmd_msg->param_1 == STATE_LIFT_STOPPED_DOWN) ||
                (cmd_msg->param_1 == STATE_LIFT_LOWERING)) {
                *stop_callback = is_down_callback;
            } else if ((cmd_msg->param_1 == STATE_LIFT_STOPPED_UP) ||
                       (cmd_msg->param_1 == STATE_LIFT_RISING)) {
                *stop_callback = is_up_callback;
            } else if ((cmd_msg->param_1 == STATE_PLINKY_PLONKY_STOPPED_AT_REFERENCE) ||
                       (cmd_msg->param_1 == STATE_PLINKY_PLONKY_PLAYING)) {
                *stop_callback = is_at_reference_callback;
            } else if ((cmd_msg->param_1 == STATE_DOOR_STOPPED_OPEN) ||
                       (cmd_msg->param_1 == STATE_DOOR_OPENING)) {
                *stop_callback = is_open_callback;
            }
        } else {
            ESP_LOGE(TAG, "Target state (0x%04x) not applicable, ignoring command.",
                     cmd_msg->param_1);
        }
    }

    return status;
}

// Set a command in motion.
// IMPORTANT: the production context should be locked before this is called.
static status_t do_cmd(cmd_msg_t *cmd_msg,
                       context_production_t *context)
{
    status_t status = STATUS_ERROR_INVALID_COMMAND;
    context_state_t *context_state = &context->context_state;
    bool (*stop_callback)(context_state_t *) = NULL;

    switch(cmd_msg->command) {
        case CMD_REBOOT:
            status = STATUS_ERROR_UNHANDLED_COMMAND;
            // TODO
            ESP_LOGE(TAG, "Received CMD_REBOOT (0x%04x), not implemented yet.",
                     cmd_msg->command);
        break;
        case CMD_LOG_START:
            status = STATUS_ERROR_UNHANDLED_COMMAND;
            // TODO
            ESP_LOGE(TAG, "Received CMD_LOG_START (0x%04x), not implemented yet.",
                     cmd_msg->command);
        break;
        case CMD_LOG_STOP:
            status = STATUS_ERROR_UNHANDLED_COMMAND;
            // TODO
            ESP_LOGE(TAG, "Received CMD_LOG_STOP (0x%04x), not implemented yet.",
                     cmd_msg->command);
        break;
        case CMD_STEPPER_TARGET_START:
            // For this command, the parameters have meaning:
            //
            // param_1: the target state, taken from state_t,
            // param_2: the velocity to travel at in TSTEPs
            // param_3: the current to supply to the stepper motor in milliamps
            // param_4: the timeout for the operation in milliseconds

            // Some protection to avoid motor burn-out
            ESP_LOGI(TAG, "Received CMD_STEPPER_TARGET_START (0x%04x):"
                     " target state 0x%04x, velocity %d milliHertz, %d mA, timeout %d ms.",
                     cmd_msg->command, cmd_msg->param_1, cmd_msg->param_2,
                     cmd_msg->param_3, cmd_msg->param_4);
            // Some states don't make sense as target states, or have
            // terminating actions we need to set up; do that here
            status = filter_and_conclude(cmd_msg, context_state, &stop_callback);
            if (status == STATUS_OK)  {
                // If there was already a command running, send
                // an abort indication
                if (context_state->cmd_running) {
                    ind_msg_t ind = {0};
                    ind.magic = PROTOCOL_MAGIC_IND;
                    ind.ind = IND_SYSTEM_STEPPER_TARGET_END;
                    ind.value = STATUS_ERROR_ABORT;
                    send_tx_data((uint8_t *) &ind, sizeof(ind), context->socket);
                    context_state->cmd_running = false;
                    ESP_LOGW(TAG, "Aborting previous target (0x%04x) after"
                             " %lld ms to accommodate new one (0x%04x).",
                             context_state->target_state,
                             (esp_timer_get_time() - context_state->start_time_microseconds) / 1000,
                             cmd_msg->param_1);
                }
                // Do the thing, disabling the motor to begin with
                status = STATUS_ERROR_HARDWARE;
                esp_err_t err = tmc2209_motor_disable(TMC2209_ADDRESS);
                if (err == ESP_OK) {
                    if (cmd_msg->param_2 != 0) {
                        // We're gonna move, so set the current
                        err = tmc2209_set_current(TMC2209_ADDRESS, TMC2209_RSENSE_MOHM,
                                                  cmd_msg->param_3,
                                                  STEPPER_MOTOR_HOLD_CURRENT_PERCENT);
                    }
                    if (err >= 0) {
                        ESP_LOGI(TAG, "Current will be %d mA.", err);
                        err = tmc2209_set_velocity(TMC2209_ADDRESS, cmd_msg->param_2);
                    }
                    if ((err == ESP_OK) && (cmd_msg->param_2 != 0)) {
                        // We're gonna move, so enable the motor
                        err = tmc2209_motor_enable(TMC2209_ADDRESS);
                    }
                }
                if (err == ESP_OK) {
                    context_state->start_time_microseconds = esp_timer_get_time();
                    context_state->timeout_ms = cmd_msg->param_4;
                    context_state->target_state = cmd_msg->param_1;
                    context_state->stop_callback = stop_callback;
                    context_state->cmd_running = true;
                    // Update the current state, now that we know what we are
                    // the rest is left to the monitoring task
                    get_state(context_state);
                    status = STATUS_OK;
                }
            }
        break;
        case CMD_STAND_INIT:
        case CMD_LIFT_INIT:
        case CMD_PLINKY_PLONKY_INIT:
        case CMD_DOOR_INIT:
            ESP_LOGI(TAG, "Received CMD_*_INIT (0x%04x).", cmd_msg->command);
            // fall-through
            // This tells us what we are
            context_state->init = cmd_msg->command;
            status = STATUS_OK;
        break;
        default:
            ESP_LOGE(TAG, "Unknown command received 0x%04x", cmd_msg->command);
        break;
    }

    return status;
}

// Wot it says
static void stop_motor()
{
    esp_err_t err = tmc2209_set_velocity(TMC2209_ADDRESS, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tmc2209_set_velocity() returned error"
                " (%d) when stopping, continuing...", err);
    }
    err = tmc2209_motor_disable(TMC2209_ADDRESS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tmc2209_motor_disable() returned error"
                " (%d) when stopping, continuing...", err);
    }
}

// Monitor the operation of commands.
static void monitor_task(void *arg)
{
    context_production_t *context = (context_production_t *) arg;
    context_sensor_t *context_sensor = &context->context_sensor;
    context_state_t *context_state = &context->context_state;
    ind_msg_t ind = {0};
    ind.magic = PROTOCOL_MAGIC_IND;

    // Allow us to feed the watchdog
    esp_task_wdt_add(NULL);

    xSemaphoreTake(context->lock, portMAX_DELAY);

    // Set up the initial shadow sensor values
    context_sensor->is_down = is_down();
    context_sensor->is_at_limit = is_at_limit();
    context_sensor->is_at_reference = is_at_reference();
    context_sensor->is_open = is_open();

    xSemaphoreGive(context->lock);

    // Main monitoring loop
    while (context->running) {

        xSemaphoreTake(context->lock, portMAX_DELAY);

        // Read all of the sensors and send indications
        // as necessary
        bool previous_state;
        if (we_are_lift(context_state->init)){
            previous_state = context_sensor->is_down;
            context_sensor->is_down = is_down();
            if (!previous_state && context_sensor->is_down) {
                ind.ind = IND_LIFT_SENSOR_TRIGGERED_LIFT_DOWN;
                send_tx_data((uint8_t *) &ind, sizeof(ind), context->socket);
                ESP_LOGI(TAG, "Sent IND_LIFT_SENSOR_TRIGGERED_LIFT_DOWN.");
            }
            previous_state = context_sensor->is_at_limit;
            context_sensor->is_at_limit = is_at_limit();
            if (!previous_state && context_sensor->is_at_limit) {
                ind.ind = IND_LIFT_SENSOR_TRIGGERED_LIFT_LIMIT;
                send_tx_data((uint8_t *) &ind, sizeof(ind), context->socket);
                ESP_LOGI(TAG, "Sent IND_LIFT_SENSOR_TRIGGERED_LIFT_LIMIT.");
            }
        }
        if (we_are_plinky_plonky(context_state->init)){
            previous_state = context_sensor->is_at_reference;
            context_sensor->is_at_reference = is_at_reference();
            if (!previous_state && context_sensor->is_at_reference) {
                ind.ind = IND_PLINKY_PLONKY_SENSOR_TRIGGERED_REFERENCE;
                send_tx_data((uint8_t *) &ind, sizeof(ind), context->socket);
                ESP_LOGI(TAG, "Sent IND_PLINKY_PLONKY_SENSOR_TRIGGERED_REFERENCE.");
            }
        }
        if (we_are_door(context_state->init)){
            previous_state = context_sensor->is_open;
            context_sensor->is_open = is_open();
            if (!previous_state && context_sensor->is_open) {
                ind.ind = IND_DOOR_SENSOR_TRIGGERED_DOOR_OPEN;
                send_tx_data((uint8_t *) &ind, sizeof(ind), context->socket);
                ESP_LOGI(TAG, "Sent IND_DOOR_SENSOR_TRIGGERED_DOOR_OPEN.");
            }
        }

        // Update our state
        get_state(context_state);

        if (context_state->cmd_running) {
            // Check the stop callback, if there is one
            if ((context_state->stop_callback != NULL) &&
                (context_state->stop_callback(context_state))) {
                // Seems we're there
                stop_motor();
                context_state->cmd_running = false;
                ESP_LOGI(TAG, "Stop callback returned true.");
            } else {
                // No stop callback check for a timeout
                if (esp_timer_get_time() - context_state->start_time_microseconds > (context_state->timeout_ms * 1000)) {
                    //  Timeout
                    context_state->cmd_running = false;
                    stop_motor();
                    ind.ind = IND_SYSTEM_STEPPER_TARGET_END;
                    ind.value = STATUS_ERROR_TIMEOUT;
                    send_tx_data((uint8_t *) &ind, sizeof(ind), context->socket);
                    ESP_LOGW(TAG, "Sent IND_SYSTEM_STEPPER_TARGET_END, timeout.");
                }
            }
        }

        xSemaphoreGive(context->lock);

        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

#endif // #if defined(CONFIG_STEPPER_PRODUCTION_MODE)

/* ----------------------------------------------------------------
 * STATIC FUNCTIONS: PRODUCTION MODE
 * -------------------------------------------------------------- */

#if defined(CONFIG_STEPPER_PRODUCTION_MODE)

// Answer a query, updating the current state as we go.
// IMPORTANT: the production context should be locked before this is called.
static int32_t answer_qry(qry_t qry, uint32_t *value,
                          context_state_t *context)
{
    status_t status = STATUS_ERROR_INVALID_QUERY;

    switch (qry) {
        case QRY_SYSTEM_STEPPER_STATE:
            *value = get_state(context);
            status = STATUS_OK;
        break;
        case QRY_LIFT_SENSOR_DOWN:
            if (we_are_lift(context->init)) {
                *value = is_down();
                status = STATUS_OK;
            }
        break;
        case QRY_LIFT_SENSOR_LIMIT:
            if (we_are_lift(context->init)) {
                *value = is_at_limit();
                status = STATUS_OK;
            }
        break;
        case QRY_PLINKY_PLONKY_SENSOR_REFERENCE:
            if (we_are_plinky_plonky(context->init)) {
                *value = is_at_reference();
                status = STATUS_OK;
            }
        break;
        case QRY_DOOR_SENSOR_OPEN:
            if (we_are_door(context->init)) {
                *value = is_open();
                status = STATUS_OK;
            }
        break;
        case QRY_SYSTEM_END:
        case QRY_STAND_BEGIN:
        case QRY_STAND_END:
        case QRY_LIFT_END:
        case QRY_PLINKY_PLONKY_END:
        case QRY_DOOR_END:
            // To keep the compiler happy
            break;
        default:
            ESP_LOGE(TAG, "Unknown query received 0x%04x", qry);
        break;
    }

    return status;
}

// Set socket to non-blocking mode.
static esp_err_t set_socket_non_blocking(int sock) {
    if (sock < 0) {
        ESP_LOGE(TAG, "Invalid socket descriptor");
        return ESP_FAIL;
    }

    // Get current flags
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "fcntl F_GETFL failed: %d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }

    // Set the non-blocking flag
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "fcntl F_SETFL (non-blocking) failed: %d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Socket %d set to non-blocking mode", sock);
    return ESP_OK;
}

// Process a buffer of received data, decrementing the contents of
// *len on return and returning a pointer to a command or
// query if one is found.  Returns either when a command or a query
// is found in the buffer or there is no more data to process.
// IMPORTANT: the production context should be locked before this is called.
static cmd_or_qry_t *process_rx_data(uint8_t *buffer, int32_t *len,
                                     context_parser_t *context)
{
    int32_t bytes_processed = 0;
    cmd_or_qry_t *cmd_or_qry = NULL;

    while ((bytes_processed < *len) && (cmd_or_qry == NULL)) {
        switch (context->msg_parser_state) {
            case MSG_PARSER_STATE_NEED_MAGIC:
            {
                context->cmd_or_qry.buffer[context->buffer_index] = buffer[bytes_processed];
                if (context->cmd_or_qry.buffer[context->buffer_index] == PROTOCOL_MAGIC_CMD) {
                    context->msg_parser_state = MSG_PARSER_STATE_NEED_CMD_BODY;
                    context->buffer_index++;
                } else if (context->cmd_or_qry.buffer[context->buffer_index] == PROTOCOL_MAGIC_QRY) {
                    context->msg_parser_state = MSG_PARSER_STATE_NEED_QRY_BODY;
                    context->buffer_index++;
                } else {
                    ESP_LOGW(TAG, "Received unknown magic byte from host (0x%02x)!",
                             context->cmd_or_qry.buffer[context->buffer_index]);
                }
            }
            break;
            case MSG_PARSER_STATE_NEED_CMD_BODY:
            {
                context->cmd_or_qry.buffer[context->buffer_index] = buffer[bytes_processed];
                context->buffer_index++;
                if (context->buffer_index >= sizeof(cmd_msg_t)) {
                    context->cmd_or_qry.cmd_not_qry = true;
                    cmd_or_qry = &(context->cmd_or_qry);
                    ESP_LOGI(TAG, "Received command 0x%02x, reference %d,"
                             " parameters 0x%x, 0x%x, 0x%x, 0x%x.",
                             context->cmd_or_qry.cmd.command,
                             context->cmd_or_qry.cmd.reference,
                             context->cmd_or_qry.cmd.param_1,
                             context->cmd_or_qry.cmd.param_2,
                             context->cmd_or_qry.cmd.param_3,
                             context->cmd_or_qry.cmd.param_4);
                }
            }
            break;
            case MSG_PARSER_STATE_NEED_QRY_BODY:
            {
                context->cmd_or_qry.buffer[context->buffer_index] = buffer[bytes_processed];
                context->buffer_index++;
                if (context->buffer_index >= sizeof(qry_msg_t)) {
                    context->cmd_or_qry.cmd_not_qry = false;
                    cmd_or_qry = &(context->cmd_or_qry);
                    ESP_LOGI(TAG, "Received query 0x%02x, reference %d.",
                             context->cmd_or_qry.qry.query,
                             context->cmd_or_qry.qry.reference);
                }
            }
            break;
            default:
                break;
        }

        bytes_processed++;
    }

    // Decrement the length we were passed by the number of bytes
    // processed
    *len -= bytes_processed;

    if (cmd_or_qry) {
        // Found something: reset context for next time
        context->buffer_index = 0;
        context->msg_parser_state = MSG_PARSER_STATE_NEED_MAGIC;
    }


    return cmd_or_qry;
}

// Task to receive comms from the server.
static void comms_rx_task(void *arg)
{
    context_production_t *context = (context_production_t *) arg;
    rsp_msg_t rsp = {0};
    rsp.magic = PROTOCOL_MAGIC_RSP,
    rsp.status = STATUS_ERROR_GENERIC;
    size_t nothing_received_count = 0;

    // Allow us to feed the watchdog
    esp_task_wdt_add(NULL);

    // Main command processing loop
    while (context->running) {

        xSemaphoreTake(context->lock, portMAX_DELAY);

        // We will be receiving either a command or a query
        uint8_t buffer[PROTOCOL_ESP32_MAX_RX_LEN];
        // Non-blocking receive
        int32_t err = recv(context->socket, &buffer, sizeof(buffer), 0);
        if (err > 0) {
            // Process received data
            while (err > 0) {
                cmd_or_qry_t *cmd_or_qry = process_rx_data(buffer, &err, &context->context_parser);
                if (cmd_or_qry) {
                    if (!cmd_or_qry->cmd_not_qry) {
                        // Got a query, get the answer
                        uint32_t value = 0;
                        rsp.reference = cmd_or_qry->qry.reference;
                        rsp.cmd_or_qry = cmd_or_qry->qry.query;
                        rsp.status = answer_qry(cmd_or_qry->qry.query, &value,
                                                &context->context_state);
                        rsp.value = value;
                    } else {
                        // Got a command: set it in motion
                        rsp.reference = cmd_or_qry->cmd.reference;
                        rsp.cmd_or_qry = cmd_or_qry->cmd.command;
                        rsp.status = do_cmd(&cmd_or_qry->cmd, context);
                    }
                    // Send the response.
                    send_tx_data((uint8_t *) &rsp, sizeof(rsp), context->socket);
                }
            }
        } else if (err == 0) {
            // Connection closed by peer
            ESP_LOGI(TAG, "Connection closed by peer!");
            context->connected = false;
        } else {
            // Error or would block
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available right now - this is expected in non-blocking mode
                // Just yield to allow other tasks to run
                nothing_received_count++;
                vTaskDelay(pdMS_TO_TICKS(WATCHDOG_FEED_TIME_MS));
            } else {
                // Real error occurred
                ESP_LOGE(TAG, "recv() failed %d (%s)!", errno, strerror(errno));
                context->connected = false;
            }
        }

        xSemaphoreGive(context->lock);

        esp_task_wdt_reset();

        if (!context->connected) {
            // Wait for a reconnection
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            // Just a short delay
            vTaskDelay(pdMS_TO_TICKS(WATCHDOG_FEED_TIME_MS));
        }
        if (nothing_received_count > 1000) {
            ESP_LOGI(TAG, "Waiting for a command.");
            nothing_received_count = 0;
        }
    }

    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

// Debounce a single pin.
static void debounce_pin(gpio_num_t pin, size_t *triggered_count)
{
    if (!gpio_get_level(pin)) {
        // The sensor is pulled low
        (*triggered_count)++;
        if ((*triggered_count) > DEBOUNCE_THRESHOLD) {
            // For the unlikely case of a wrap
            (*triggered_count) = DEBOUNCE_THRESHOLD;
        }
    } else {
        // Not open, reset the count
        (*triggered_count) = 0;
    }
}

// Debounce timer callback function for debouncing all of the
// sensors in production.
static void debounce_callback(void* arg)
{
    context_debounce_t *context = (context_debounce_t *) arg;

#  if defined(CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0)
    debounce_pin(CONFIG_STEPPER_LIFT_LIMIT_PIN, &context->triggered_count_is_at_limit);
#  endif
#  if defined(CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
    debounce_pin(CONFIG_STEPPER_LIFT_DOWN_PIN, &context->triggered_count_is_down);
#  endif
#  if defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0)
    debounce_pin(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN, &context->triggered_count_is_at_reference);
#  endif
#  if defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)
    debounce_pin(CONFIG_STEPPER_DOOR_OPEN_PIN, &context->triggered_count_is_open);
#  endif
}

// Production mode.
static void do_production(const char *server_ip, uint16_t port)
{
    context_debounce_t *context_debounce = &g_context_production.context_debounce;

    ESP_LOGI(TAG, "PRODUCTION MODE");

    // Create mutex
    if (!g_context_production.lock) {
        g_context_production.lock = xSemaphoreCreateMutex();
    }
    if (g_context_production.lock) {

        xSemaphoreTake(g_context_production.lock, portMAX_DELAY);

        // Start a timer to read the sensor pins and debounce them
        context_debounce->timer_args.callback = debounce_callback;
        context_debounce->timer_args.arg = (void *) &g_context_production.context_debounce;
        context_debounce->timer_args.dispatch_method = ESP_TIMER_TASK;
        context_debounce->timer_args.name = "debounce";
        context_debounce->timer_args.skip_unhandled_events = false;
        context_debounce->timer_handle = NULL;
        esp_err_t err = esp_timer_create(&context_debounce->timer_args,
                                          &context_debounce->timer_handle);
        if (err == ESP_OK) {
            err = esp_timer_start_periodic(context_debounce->timer_handle,
                                           DEBOUNCE_CHECK_PERIOD_US);
        }
        if (err == ESP_OK) {
            // Create a TCP socket for comms with the server
            err = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        }
        if (err >= 0) {
            g_context_production.socket = err;
            err = ESP_OK;
            // Configure server address
            g_context_production.server.sin_family = AF_INET;
            g_context_production.server.sin_port = htons(port);
            inet_pton(AF_INET, server_ip, &g_context_production.server.sin_addr);

            ESP_LOGI(TAG, "Connecting to %s:%d...",  server_ip, port);

            // Connect to the server
            err = connect(g_context_production.socket,
                            (struct sockaddr *) &g_context_production.server,
                            sizeof(g_context_production.server));
            if (err == 0) {
                g_context_production.connected = true;
                g_context_production.running = true;
                // Put socket into non-blocking mode as we don't want to
                // get stuck in a recv()
                 set_socket_non_blocking(g_context_production.socket);
            } else {
                ESP_LOGE(TAG, "Failed to connect to server %d (%s)!", errno, strerror(errno));
            }
        } else {
            ESP_LOGE(TAG, "Unable to create socket %d (%s)!", errno, strerror(errno));
        }

        xSemaphoreGive(g_context_production.lock);
    }

    // Start tasks to receive comms from the server and monitor the operation of commands
    if (g_context_production.connected &&
        (xTaskCreate(&monitor_task, "monitor_task", 1024 * 4, &g_context_production, 5, &g_context_production.task_handle_monitor) == pdPASS) &&
        (xTaskCreate(&comms_rx_task, "comms_rx_task", 1024 * 4, &g_context_production, 5, &g_context_production.task_handle_comms_rx) == pdPASS)) {
        ESP_LOGI(TAG, "Waiting for commands from server.");
        while(1) {
            flash_debug_led(DEBUG_LED_SHORT_MS);
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!g_context_production.connected) {
                ESP_LOGE(TAG, "Reconnecting to server after error!");

                xSemaphoreTake(g_context_production.lock, portMAX_DELAY);

                // Close old socket if it exists
                if (g_context_production.socket >= 0) {
                    close(g_context_production.socket);
                    g_context_production.socket = -1;
                }

                // Create new socket
                g_context_production.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (g_context_production.socket >= 0) {
                    if (connect(g_context_production.socket,
                                (struct sockaddr *) &g_context_production.server,
                                sizeof(g_context_production.server)) == 0) {
                        set_socket_non_blocking(g_context_production.socket);
                        g_context_production.connected = true;
                        ESP_LOGI(TAG, "Reconnected.");
                    } else {
                        ESP_LOGE(TAG, "Unable to reconnect to server %d (%s)!", errno, strerror(errno));
                    }
                } else {
                    ESP_LOGE(TAG, "Unable to create new socket %d (%s)!", errno, strerror(errno));
                }

                xSemaphoreGive(g_context_production.lock);

                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            esp_task_wdt_reset();
        }
    }

    // Tidy up
    g_context_production.running = false;

    xSemaphoreTake(g_context_production.lock, portMAX_DELAY);

    close(g_context_production.socket);
    g_context_production.socket = -1;

    if (g_context_production.task_handle_comms_rx) {
        vTaskDelete(g_context_production.task_handle_comms_rx);
    }

    if (g_context_production.task_handle_monitor) {
        vTaskDelete(g_context_production.task_handle_monitor);
    }

    if (context_debounce->timer_handle != NULL) {
        esp_timer_stop(context_debounce->timer_handle);
        esp_timer_delete(context_debounce->timer_handle);
    }

    xSemaphoreGive(g_context_production.lock);
    // Don't delete the semaphore in case someone has it
}

#endif // #if defined(CONFIG_STEPPER_PRODUCTION_MODE)

/* ----------------------------------------------------------------
 * PUBLIC FUNCTIONS
 * -------------------------------------------------------------- */

// Entry point
void app_main(void)
{

    ESP_LOGI(TAG, "Stepper app_main start");

    esp_err_t err = init();
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

#if defined CONFIG_STEPPER_DIAG_PIN && (CONFIG_STEPPER_DIAG_PIN >= 0)
        if (err == ESP_OK) {
            tmc2209_set_stallguard(TMC2209_ADDRESS, -1, 0);
        }
#endif

        // Allow us to feed the watchdog
        esp_task_wdt_add(NULL);

#if defined(CONFIG_STEPPER_PRODUCTION_MODE)

#  if defined(CONFIG_STEPPER_NO_WIFI)
#    error Cannot define CONFIG_STEPPER_NO_WIFI in production mode, the Pi has to be able to talk to the ESP32s
#  endif

        if (err == ESP_OK) {
            // Test mode only
            do_production(CONFIG_STEPPER_PRODUCTION_SERVER, CONFIG_STEPPER_PRODUCTION_PORT);
        }
#else

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

        if (err == ESP_OK) {
#  if defined(CONFIG_STEPPER_STAND)
            // We're operating the stand
            be_the_stand();
#  elif defined(CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN) && (CONFIG_STEPPER_PLINKY_PLONKY_REFERENCE_PIN >= 0)
            // We're operating the plinky-plonky
            be_the_plinky_plonky();
#  elif defined(CONFIG_STEPPER_LIFT_LIMIT_PIN) && (CONFIG_STEPPER_LIFT_LIMIT_PIN >= 0) && \
        defined(CONFIG_STEPPER_LIFT_DOWN_PIN) && (CONFIG_STEPPER_LIFT_DOWN_PIN >= 0)
            // We're operating the lift
            be_the_lift();
#  elif defined(CONFIG_STEPPER_DOOR_OPEN_PIN) && (CONFIG_STEPPER_DOOR_OPEN_PIN >= 0)
            // We're operating a door
            be_a_door();
#  else
            // Test mode only
            do_test_mode();
#  endif
            tmc2209_set_velocity(TMC2209_ADDRESS, 0);
            tmc2209_motor_disable(TMC2209_ADDRESS);
        }
        ESP_LOGI(TAG, "DONE with motor stuff");
        log_deinit();
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
        }
        esp_task_wdt_delete(NULL);

#endif // #if defined(CONFIG_STEPPER_PRODUCTION_MODE)

    } else {
        ESP_LOGE(TAG, "Initialization failed, system cannot continue, will restart soonish.");
    }

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
    log_deinit();
    network_deinit();
    esp_restart();
}

// End of file
