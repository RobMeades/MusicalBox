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
#define PROTOCOL_MAGIC_RSP    0xBB
#define PROTOCOL_MAGIC_IND    0xCC
#define PROTOCOL_MAGIC_LOG    0xDD

// The maximum length of a log message (including the null terminator).
#define LOG_MESSAGE_MAX_LEN   256

/* ----------------------------------------------------------------
 * TYPES
 * -------------------------------------------------------------- */

// The protocol, commands first.
typedef enum {
    // System commands, start at 0
    CMD_SYSTEM_BEGIN               = 0x0000,
    CMD_REBOOT                     = CMD_SYSTEM_BEGIN,
    CMD_LOG_START                  = CMD_SYSTEM_BEGIN + 1,
    CMD_LOG_STOP                   = CMD_SYSTEM_BEGIN + 2,

    // System-level queries start at 0x0100
    // System-level indications start at 0x0200

    // Commands for the stand
    CMD_STAND_BEGIN                = 0x1000,
    CMD_STAND_ROTATE_CLOCKWISE     = CMD_STAND_BEGIN,
    CMD_STAND_ROTATE_ANTICLOCKWISE = CMD_STAND_BEGIN + 1,

    // Queries to the stand start at 0x1100
    // Indications from the stand start at 0x1200

    // Commands for the lift
    CMD_LIFT_BEGIN                  = 0x2000,
    CMD_LIFT_UP                     = CMD_LIFT_BEGIN,
    CMD_LIFT_DOWN                   = CMD_LIFT_BEGIN + 1,

    // Queries to the lift start at 0x2100
    // Indications from the lift start at 0x2200

    // Commands for the plinky-plonky
    CMD_PLINKY_PLONKY_BEGIN         = 0x3000,
    CMD_PLINKY_PLONKY_PLAY_START    = CMD_PLINKY_PLONKY_BEGIN,
    CMD_PLINKY_PLONKY_PLAY_STOP     = CMD_PLINKY_PLONKY_BEGIN + 1,

    // Queries to the plinky-plonky start at 0x3100
    // Indications from the plinky-plonky start at 0x3200

    // Commands for a door
    CMD_DOOR_BEGIN                  = 0x4000,
    CMD_DOOR_OPEN                   = CMD_DOOR_BEGIN,
    CMD_DOOR_CLOSE                  = CMD_DOOR_BEGIN + 1 

    // Queries to the door start at 0x4100
    // Indications from the door start at 0x4200

} cmd_t;

// Queries.
typedef enum {
    // System-level queries (there are none)
    QRY_SYSTEM_BEGIN                   = 0x0100,

    // Queries to the stand
    QRY_STAND_BEGIN                    = 0x1100,
    QRY_STAND_ROTATION_STATE           = QRY_STAND_BEGIN,

    // Queries to the lift
    QRY_LIFT_BEGIN                     = 0x2100,
    QRY_LIFT_STATE                     = QRY_LIFT_BEGIN,
    QRY_LIFT_SENSOR_LIMIT              = QRY_LIFT_BEGIN + 1,

    // Queries to the plinky-plonky
    QRY_PLINKY_PLONKY_BEGIN            = 0x3100,
    QRY_PLINKY_PLONKY_STATE            = QRY_PLINKY_PLONKY_BEGIN,
    QRY_PLINKY_PLONKY_SENSOR_REFERENCE = QRY_PLINKY_PLONKY_BEGIN + 1,

    // Queries to a door
    QRY_DOOR_BEGIN                     = 0x4100,
    QRY_DOOR_STATE                     = QRY_DOOR_BEGIN,
    QRY_DOOR_SENSOR_OPEN               = QRY_DOOR_BEGIN + 1
} qry_t;

// Indications.
typedef enum {
    // System-level indications (there are none)
    IND_SYSTEM_BEGIN                             = 0x0200,

    // Indications from the stand (there are none)
    IND_STAND_BEGIN                              = 0x1200,

    // Indications from the lift
    IND_LIFT_BEGIN                               = 0x2200,
    IND_LIFT_SENSOR_TRIGGERED_LIFT_DOWN          = IND_LIFT_BEGIN,
    IND_LIFT_SENSOR_TRIGGERED_LIFT_LIMIT         = IND_LIFT_BEGIN + 1,

    // Indications from the plinky-plonky
    IND_PLINKY_PLONKY_BEGIN                      = 0x3200,
    IND_PLINKY_PLONKY_SENSOR_TRIGGERED_REFERENCE = IND_PLINKY_PLONKY_BEGIN,

    // Indications from a door
    IND_DOOR_BEGIN                               = 0x4200,
    IND_DOOR_SENSOR_TRIGGERED_DOOR_OPEN          = IND_DOOR_BEGIN
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
    STATUS_OK                    = 0x0000,
    STATUS_ERROR_GENERIC         = 0x0001,
    STATUS_ERROR_INVALID_COMMAND = 0x0002,
    STATUS_ERROR_INVALID_PARAM   = 0x0003,
    STATUS_ERROR_BUSY            = 0x0004,
    STATUS_ERROR_TIMEOUT         = 0x0005
} status_t;

// Message structures
typedef struct __attribute__((packed)) {
    uint8_t magic;      // PROTOCOL_MAGIC_CMD
    uint16_t command;   // cmd_t
    uint32_t param;     // Parameter (if any)
} cmd_msg_t;

typedef struct __attribute__((packed)) {
    uint8_t magic;      // PROTOCOL_MAGIC_RSP
    uint16_t status;    // non-zero means error
    uint32_t value;     // Response value (if any)
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
