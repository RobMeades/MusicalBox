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

 #ifndef _PROTOCOL_H_
 #define _PROTOCOL_H_
 
/** @file
 * @brief Protocol definition for comms between an ESP32 "module" and
 * a controlling entity.
 *
 * Note: endianness is not considered here since the ESP32 and a
 * Raspberry Pi are both little-endian.
 *
 * Note: the underlying transport is assumed to be perfect and ordered.
 */
 
#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * COMPILE-TIME MACROS
 * -------------------------------------------------------------- */

// The version of this protocol.
#define PROTOCOL_VERSION      0x01

// Magic bytes for different message types.
#define PROTOCOL_MAGIC_CMD    0xAA
#define PROTOCOL_MAGIC_QRY    0xBB
#define PROTOCOL_MAGIC_RSP    0xCC
#define PROTOCOL_MAGIC_IND    0xDD
#define PROTOCOL_MAGIC_LOG    0xEE

// The maximum length of a message that can be sent to the ESP32
// "module", i.e. the length of a cmd_t.
#define PROTOCOL_ESP32_MAX_RX_LEN (sizeof(cmd_t))

// The maximum length of a log message (including the null terminator).
#define LOG_MESSAGE_MAX_LEN   256

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

// The protocol, states first.
typedef enum {
    STATE_NULL                                = 0x0000,

    // States for the stand begin at 0x1000
    STATE_STAND_BEGIN                         = 0x1000,
    STATE_STAND_STOPPED                       = STATE_STAND_BEGIN,
    STATE_STAND_ROTATING_CLOCKWISE            = STATE_STAND_BEGIN + 1,
    STATE_STAND_ROTATING_ANTICLOCKWISE        = STATE_STAND_BEGIN + 2,
    STATE_STAND_END                           = 0x1fff,

    // States for the lift begin at 0x2000
    STATE_LIFT_BEGIN                          = 0x2000,
    STATE_LIFT_STOPPED_UNKNOWN                = STATE_LIFT_BEGIN,
    STATE_LIFT_STOPPED_DOWN                   = STATE_LIFT_BEGIN + 1,
    STATE_LIFT_STOPPED_UP                     = STATE_LIFT_BEGIN + 2,
    STATE_LIFT_RISING                         = STATE_LIFT_BEGIN + 3,
    STATE_LIFT_LOWERING                       = STATE_LIFT_BEGIN + 4,
    STATE_LIFT_END                            = 0x2fff,

    // States for the plinky-plonky begin at 0x3000
    STATE_PLINKY_PLONKY_BEGIN                 = 0x3000,
    STATE_PLINKY_PLONKY_STOPPED_UNKNOWN       = STATE_PLINKY_PLONKY_BEGIN,
    STATE_PLINKY_PLONKY_STOPPED_AT_REFERENCE  = STATE_PLINKY_PLONKY_BEGIN + 1,
    STATE_PLINKY_PLONKY_PLAYING               = STATE_PLINKY_PLONKY_BEGIN + 2,
    STATE_PLINKY_PLONKY_END                   = 0x3fff,

    // States for a door begin at 0x4000
    STATE_DOOR_BEGIN                          = 0x4000,
    STATE_DOOR_STOPPED_UNKNOWN                = STATE_DOOR_BEGIN,
    STATE_DOOR_STOPPED_CLOSED                 = STATE_DOOR_BEGIN + 1,
    STATE_DOOR_STOPPED_OPEN                   = STATE_DOOR_BEGIN + 2,
    STATE_DOOR_OPENING                        = STATE_DOOR_BEGIN + 3,
    STATE_DOOR_CLOSING                        = STATE_DOOR_BEGIN + 4,
    STATE_DOOR_END                            = 0x4fff
} state_t;

// Then commands.
typedef enum {
    // System commands, start at 0
    CMD_SYSTEM_BEGIN               = 0x0000,
    // CMD_REBOOT has no parameters
    CMD_REBOOT                     = CMD_SYSTEM_BEGIN,
    // CMD_LOG_START has a single parameter which is log_level_t
    CMD_LOG_START                  = CMD_SYSTEM_BEGIN + 1,
    // CMD_LOG_STOP has no parameters
    CMD_LOG_STOP                   = CMD_SYSTEM_BEGIN + 2,
    // CMD_STEPPER_TARGET_START has three parameters:
    // 1: the target state, taken from state_t,
    // 2: the velocity to travel at in milliHertz
    // 3: the current to supply to the stepper motor in milliamps
    // 4: the timeout for the operation in milliseconds
    CMD_STEPPER_TARGET_START       = CMD_SYSTEM_BEGIN + 3,
    CMD_SYSTEM_END                 = 0x00ff,

    // System-level queries start at 0x0100
    // System-level indications start at 0x0200

    // Commands to the stand (there is only init)
    CMD_STAND_BEGIN                 = 0x1000,
    // CMD_STAND_INIT has no parameters, it just
    // means "you are a stand"
    CMD_STAND_INIT                  = CMD_STAND_BEGIN,
    CMD_STAND_END                   = 0x10ff,

    // Queries to the stand start at 0x1100
    // Indications from the stand start at 0x1200

    // Commands to the lift (there is only init)
    CMD_LIFT_BEGIN                  = 0x2000,
    // CMD_LIFT_INIT has no parameters, it just
    // means "you are a lift"
    CMD_LIFT_INIT                   = CMD_LIFT_BEGIN,
    CMD_LIFT_END                    = 0x20ff,

    // Queries to the lift start at 0x2100
    // Indications from the lift start at 0x2200

    // Commands to the plinky-plonky (there is only init)
    CMD_PLINKY_PLONKY_BEGIN         = 0x3000,
    // CMD_PLINKY_PLONKY_INIT has no parameters, it just
    // means "you are a plinky-plonky"
    CMD_PLINKY_PLONKY_INIT          = CMD_PLINKY_PLONKY_BEGIN,
    CMD_PLINKY_PLONKY_END           = 0x30ff,

    // Queries to the plinky-plonky start at 0x3100
    // Indications from the plinky-plonky start at 0x3200

    // Commands to a door (there is only init)
    CMD_DOOR_BEGIN                  = 0x4000,
    // CMD_DOOR_INIT has no parameters, it just
    // means "you are a door"
    CMD_DOOR_INIT                   = CMD_DOOR_BEGIN,
    CMD_DOOR_END                    = 0x40ff

    // Queries to the door start at 0x4100
    // Indications from the door start at 0x4200

} cmd_t;

// Queries.
typedef enum {
    // System-level queries (there are none)
    QRY_SYSTEM_BEGIN                   = 0x0100,
    // QRY_STEPPER_STATE should cause the receiver to
    // return a rsp_msg_t with the value field containing
    // its current state, from state_t.
    QRY_SYSTEM_STEPPER_STATE           = QRY_SYSTEM_BEGIN,
    QRY_SYSTEM_END                     = 0x01ff,

    // Queries to the stand (there are none)
    QRY_STAND_BEGIN                    = 0x1100,
    QRY_STAND_END                      = 0x11ff,

    // Queries to the lift
    QRY_LIFT_BEGIN                     = 0x2100,
    // QRY_LIFT_SENSOR_DOWN should cause the receiver to
    // return a rsp_msg_t with the value field containing
    // 1 if the "down" sensor is currently triggered, else 0
    QRY_LIFT_SENSOR_DOWN               = QRY_LIFT_BEGIN,
    // QRY_LIFT_SENSOR_LIMIT should cause the receiver to
    // return a rsp_msg_t with the value field containing
    // 1 if the "limit" sensor is currently triggered, else 0
    QRY_LIFT_SENSOR_LIMIT              = QRY_LIFT_BEGIN + 1,
    QRY_LIFT_END                       = 0x21ff,

    // Queries to the plinky-plonky
    QRY_PLINKY_PLONKY_BEGIN            = 0x3100,
    // QRY_PLINKY_PLONKY_SENSOR_REFERENCE should cause the
    // receiver to return a rsp_msg_t with the value field
    // containing 1 if the "reference" sensor is currently
    // triggered, else 0
    QRY_PLINKY_PLONKY_SENSOR_REFERENCE = QRY_PLINKY_PLONKY_BEGIN,
    QRY_PLINKY_PLONKY_END              = 0x31ff,

    // Queries to a door
    QRY_DOOR_BEGIN                     = 0x4100,
    // QRY_DOOR_SENSOR_OPEN should cause the receiver
    // to return a rsp_msg_t with the value field
    // containing 1 if the "open" sensor is currently
    // triggered, else 0
    QRY_DOOR_SENSOR_OPEN               = QRY_DOOR_BEGIN,
    QRY_DOOR_END                       = 0x41ff
} qry_t;

// Indications.
typedef enum {
    // System-level indications
    IND_SYSTEM_BEGIN                             = 0x0200,
    // This is sent by a stepper when it has completed a
    // CMD_STEPPER_TARGET_START, either successfully or not,
    // the value field of the ind_msg_t indicating containing
    // status_t (i.e. zero for success)
    IND_SYSTEM_STEPPER_TARGET_END                = IND_SYSTEM_BEGIN,
    IND_SYSTEM_END                               = 0x02ff,

    // Indications from the stand (there are none)
    IND_STAND_BEGIN                              = 0x1200,
    IND_STAND_END                                = 0x12ff,

    // Indications from the lift
    IND_LIFT_BEGIN                               = 0x2200,
    IND_LIFT_SENSOR_TRIGGERED_LIFT_DOWN          = IND_LIFT_BEGIN,
    IND_LIFT_SENSOR_TRIGGERED_LIFT_LIMIT         = IND_LIFT_BEGIN + 1,
    IND_LIFT_END                                 = 0x22ff,

    // Indications from the plinky-plonky
    IND_PLINKY_PLONKY_BEGIN                      = 0x3200,
    IND_PLINKY_PLONKY_SENSOR_TRIGGERED_REFERENCE = IND_PLINKY_PLONKY_BEGIN,
    IND_PLINKY_PLONKY_END                        = 0x32ff,

    // Indications from a door
    IND_DOOR_BEGIN                               = 0x4200,
    IND_DOOR_SENSOR_TRIGGERED_DOOR_OPEN          = IND_DOOR_BEGIN,
    IND_DOOR_END                                 = 0x42ff
} ind_t;

// Log levels.
typedef enum {
    LOG_DEBUG    = 0x00,
    LOG_INFO     = 0x01,
    LOG_WARN     = 0x02,
    LOG_ERROR    = 0x03
} log_level_t;

// Status codes.
typedef enum {
    STATUS_OK                      = 0,
    STATUS_ERROR_GENERIC           = 1,
    STATUS_ERROR_INVALID_COMMAND   = 2,
    STATUS_ERROR_UNHANDLED_COMMAND = 3,
    STATUS_ERROR_INVALID_QUERY     = 4,
    STATUS_ERROR_UNHANDLED_QUERY   = 5,
    STATUS_ERROR_INVALID_PARAM     = 6,
    STATUS_ERROR_ABORT             = 7,
    STATUS_ERROR_BUSY              = 8,
    STATUS_ERROR_TIMEOUT           = 9,
    STATUS_ERROR_HARDWARE          = 10
} status_t;

// Message structures
typedef struct __attribute__((packed)) {
    uint8_t magic;      // PROTOCOL_MAGIC_CMD
    uint16_t command;   // cmd_t
    uint8_t reference;  // Reference to be copied into the response, increment after each command
    uint32_t param_1;   // Parameter 1
    uint32_t param_2;   // Parameter 2
    uint32_t param_3;   // Parameter 3
    uint32_t param_4;   // Parameter 4
} cmd_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;      // PROTOCOL_MAGIC_QRY
    uint16_t query;     // qry_t
    uint8_t reference;  // Reference to be copied into the response, increment after each query
} qry_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;       // PROTOCOL_MAGIC_RSP
    uint16_t cmd_or_qry; // the value of the command or query that caused the response 
    uint8_t reference;   // Reference from a command or query
    uint16_t status;     // non-zero means error
    uint32_t value;      // Response value (if any)
} rsp_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;      // PROTOCOL_MAGIC_IND
    uint16_t ind;       // ind_t
    uint32_t value;     // Associated value (if any)
} ind_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;      // PROTOCOL_MAGIC_LOG
    uint8_t level;      // log_level_t
    char message[LOG_MESSAGE_MAX_LEN];  // Null-terminated string
} log_msg_t;

#ifdef __cplusplus
}
#endif
 
/** @}*/
 
#endif // _PROTOCOL_H_
 
 // End of file
