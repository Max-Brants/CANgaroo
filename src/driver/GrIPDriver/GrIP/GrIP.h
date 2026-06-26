/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef GRIP_H_INCLUDED
#define GRIP_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#include <QByteArray>

// Current protocol version
#define GRIP_VERSION        4u

// Transmit/Receive buffer size - Do not exceed (GRIP_BUFFER_SIZE - 10)
#define GRIP_BUFFER_SIZE    256u

enum GrIP_ProtocolType_e {
    PROT_GrIP = 0, PROT_BoOTA
};

/**
  * Available message types.
  */
enum GrIP_MessageType_e {
    MSG_SYSTEM_CMD          = 0u,
    MSG_REALTIME_CMD        = 1u,
    MSG_DATA                = 2u,
    MSG_DATA_NO_RESPONSE    = 3u,
    MSG_NOTIFICATION        = 4u,
    MSG_RESPONSE            = 5u,
    MSG_ERROR               = 6u,
    MSG_SYNC                = 7u,
    MSG_MAX_NUM             = 8u
};

/**
  * Return Types.
  */
enum GrIP_ReturnType_e {
    RET_OK              = 0,
    RET_NOK             = 1,
    RET_WRONG_VERSION   = 2,
    RET_WRONG_CRC       = 3,
    RET_WRONG_MAGIC     = 4,
    RET_WRONG_PARAM     = 5,
    RET_WRONG_TYPE      = 6,
    RET_WRONG_LEN       = 7,
};

/**
  * GrIP Packet Header
  * Size: 8 bytes
  */
typedef struct __attribute__((packed))
{
    uint8_t  Version;
    uint8_t  Protocol;
    uint8_t  MsgType;
    uint8_t  ReturnCode;
    uint16_t Length;
    uint8_t  CRC_Header;
    uint8_t  CRC_Data;
} GrIP_PacketHeader_t;

/**
  * GrIP Receive Packet
  */
struct GrIP_Packet_t {
    GrIP_PacketHeader_t RX_Header;
    uint8_t             Data[GRIP_BUFFER_SIZE];
};

/**
  * Data struct.
  * Data: Pointer to data.
  * Length: Length of data in bytes.
  */
struct GrIP_Pdu_t {
    const uint8_t  *Data;
    uint16_t  Length;
};

struct GrIP_ErrorFlags_t {
    uint8_t  LastError;
    uint16_t CRC_Error;
    uint16_t Len_Error;
    uint16_t Param_Error;
};

/**
  * Initialize the module
  */
void GrIP_Init();

/**
  * Transmit a message over GrIP
  */
[[nodiscard]] uint8_t GrIP_TransmitArray(GrIP_ProtocolType_e ProtType, GrIP_MessageType_e MsgType, GrIP_ReturnType_e ReturnCode, const uint8_t *data, uint16_t len);

/**
  * Transmit a message over GrIP
  */
[[nodiscard]] uint8_t GrIP_Transmit(GrIP_ProtocolType_e ProtType, GrIP_MessageType_e MsgType, GrIP_ReturnType_e ReturnCode, const GrIP_Pdu_t *pdu);

/**
  * Sync protocol flow
  */
[[nodiscard]] uint8_t GrIP_SendSync(void);

/**
  * Continuously call this function to process RX messages
  */
void GrIP_Update(void);

/**
  * Get error flags
  */
void GrIP_GetError(GrIP_ErrorFlags_t *ef);

[[nodiscard]] bool GrIP_Receive(GrIP_Packet_t *p);

int GrIP_GetLastResponse(void);

void       GrIP_RxCallback(const QByteArray &data);
QByteArray GrIP_GetTxData();

#endif // GRIP_H_INCLUDED
