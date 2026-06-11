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

#include "GrIPHandler.h"
#include "CRC.h"
#include <chrono>
#include <cstring>
#include <format>
#include <QThread>
#include <QSerialPort>
#include <QtEndian>


// ---------------------------------------------------------------------------
// System command IDs — sent in Protocol_SystemHeader_t::Command
// ---------------------------------------------------------------------------
#define SYSTEM_REPORT_INFO      0u  // Request firmware version and channel info
#define SYSTEM_SET_STATUS       1u  // Notify device of host open/close state

#define SYSTEM_SEND_CAN_CFG     20u // Configure CAN channel bit rate
#define SYSTEM_SEND_LIN_CFG     21u // Configure LIN channel
#define SYSTEM_START_CAN        22u // Enable/disable CAN channels
#define SYSTEM_START_LIN        23u // Enable/disable LIN channels
#define SYSTEM_ADD_CAN_FRAME    24u // Add CAN frame to schedule
#define SYSTEM_ADD_LIN_FRAME    25u // Add LIN frame to schedule
#define SYSTEM_CAN_MODE         26u // Set CAN channel operating mode
#define SYSTEM_CAN_TXECHO       27u // Enable/disable TX echo
#define SYSTEM_LIN_SET_TABLE    28u

#define SYSTEM_SEND_CAN_FRAME   30u // Transmit a CAN frame immediately
#define SYSTEM_SET_LIN_DATA     31u
#define SYSTEM_SEND_GPIO_CFG            32u // Configure GPIO pin directions and report cycle time
#define SYSTEM_SET_GPIO_OUTPUT          33u // Set GPIO pin output states
#define SYSTEM_GET_CHANNEL_CAPABILITIES 34u // Request capability bitmask for a given channel
#define SYSTEM_SEND_CANFD_CFG           35u // Configure CAN FD channel (arb + data phase baudrates)
#define SYSTEM_LIN_SLEEP_WAKEUP         36u // Put LIN channel to sleep or wake it up
#define SYSTEM_LIN_DIAG_REQ             37u // LIN transport-layer diagnostic master request (0x3C/0x3D)

#define LIN_SLEEP_WAKEUP_ACTION_SLEEP   0u
#define LIN_SLEEP_WAKEUP_ACTION_WAKEUP  1u

// ---------------------------------------------------------------------------
// LIN frame flag bits — used in Protocol_LinFrame_t::Flags for SYSTEM_ADD_LIN_FRAME
static constexpr uint8_t LIN_FLAGS_SPORADIC = 0x10; // Frame belongs to a sporadic slot

// CAN frame flag bits — stored in Protocol_CanFrame_t::Flags
// ---------------------------------------------------------------------------
#define CAN_FLAGS_EXT_ID        0x01 // Extended (29-bit) CAN identifier
#define CAN_FLAGS_FD            0x02 // CAN-FD frame
#define CAN_FLAGS_RTR           0x04 // Remote Transmission Request
#define CAN_FLAGS_BRS           0x08 // Bit Rate Switch (CAN-FD only)
#define CAN_FLAGS_TX            0x80 // Frame originated from the host (TX echo)

// CAN error flag bits — reported in Protocol_CanFrame_t::ErrFlags
static constexpr uint8_t GrIP_ERR_STUFF    = 0x01;
static constexpr uint8_t GrIP_ERR_FORM     = 0x02;
static constexpr uint8_t GrIP_ERR_ACK      = 0x04;
static constexpr uint8_t GrIP_ERR_BIT_REC  = 0x08;
static constexpr uint8_t GrIP_ERR_BIT_DOM  = 0x10;
static constexpr uint8_t GrIP_ERR_CRC      = 0x20;
static constexpr uint8_t GrIP_ERR_SW_SET   = 0x40;


#define GRIP_HEADER_VERSION     1

// ---------------------------------------------------------------------------
// Wire protocol structures
//
// All structs are packed to match the exact byte layout expected by the
// firmware. Fields are in the order they appear on the wire.
// ---------------------------------------------------------------------------

// Common header prepended to every system command payload.
typedef struct __attribute__((packed))
{
    uint8_t Version; // Protocol version — always 1
    uint8_t Command; // SYSTEM_* command identifier
    uint8_t Length;  // Payload length in bytes following this header
    uint8_t Data;    // First payload byte (command-specific)
} Protocol_SystemHeader_t;

typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t Channel;
    uint32_t Baudrate;
    uint8_t EchoTx;
    uint8_t ABOM;
    uint8_t ListenMode;
} Protocol_CanConfig_t;

// Payload for SYSTEM_SEND_CANFD_CFG — configures CAN FD arbitration and data phase.
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t  Channel;
    uint32_t ArbBaudrate;   // Arbitration phase baudrate (Hz)
    uint32_t DataBaudrate;  // Data phase baudrate (Hz); 0 = same as arb (no BRS)
    uint8_t  EchoTx;
    uint8_t  ABOM;
    uint8_t  ListenMode;
} Protocol_CanFdConfig_t;

// Payload for SYSTEM_START_CAN — carries the enable state for both channels.
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;

    uint8_t Channel1; // Enable state for channel 0 (0 = off, 1 = on)
    uint8_t Channel2; // Enable state for channel 1 (0 = off, 1 = on)
} Protocol_ChannelStatus_t;

// Payload for SYSTEM_START_LIN — enables or disables a single LIN channel by index.
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t Channel; // Zero-based LIN channel index
    uint8_t Enable;  // 1 = start, 0 = stop
} Protocol_ChannelEnable_t;

typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;

    uint8_t  BusState[8];
    uint16_t RxDropCount[8];
} Protocol_BusStatus_t;

// Payload for SYSTEM_CAN_MODE — sets listen-only or normal mode.
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;

    uint8_t Channel; // Zero-based channel index
    uint32_t Param;    // 0 = normal, 1 = listen-only
} Protocol_ChannelParam_t;

// Payload for SYSTEM_SEND_CAN_FRAME and incoming DATA_REPORT_CAN_MSG (254).
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;

    uint8_t  Channel;   // Zero-based channel index
    uint32_t ID;        // CAN identifier (11-bit or 29-bit depending on Flags)
    uint8_t  DLC;       // Data length code (0-8 classic, 0-64 FD)
    uint8_t  Flags;     // Bitfield of CAN_FLAGS_* values
    uint8_t  ErrFlags;  // Non-zero if the device detected a bus error
    uint32_t Time;      // Device timestamp (µs, wraps around)
    uint8_t  Data[64];  // Payload bytes — valid range is Data[0..DLC-1]
} Protocol_CanFrame_t;

typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;

    uint32_t Hash;
} Protocol_CanTxEcho_t;

// Reply payload for SYSTEM_REPORT_INFO — sent by the device in response to a version request.
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;     // Header.Command = SYSTEM_REPORT_INFO
    uint8_t Major;                      // Firmware major version
    uint8_t Minor;                      // Firmware minor version
    uint8_t HwRevision;                 // Hardware revision (reserved)
    uint8_t ChannelsCAN;                // Number of classic CAN channels
    uint8_t ChannelsCANFD;              // Number of CAN-FD channels
    uint8_t ChannelsLIN;                // Number of LIN channels
    uint8_t ChannelsADC;                // Number of ADC channels (reserved)
    uint8_t ChannelsGPIO;               // Number of GPIO channels (reserved)
    uint8_t LinScheduleTables;          // LIN schedule tables per channel
    uint8_t MaxCyclicCAN;               // Maximum number of cyclic CAN messages
    char    BuildDate[128];             // Null-terminated build date string
} Protocol_SystemInfoReply_t;

typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t  Channel;
    uint16_t Baudrate;
    uint8_t  Timebase;
    uint16_t Jitter;
    uint8_t  Mode;
    uint8_t  Protocol;
    uint16_t DiagSTmin_ms;   // Min separation between CF frames (0 = no gap)
    uint16_t DiagP2min_ms;   // Min delay before the 0x3D response slot
    uint16_t DiagNAs_ms;     // TX frame abort timeout
    uint16_t DiagNCr_ms;     // Response (0x3D) wait timeout
    uint8_t  SlaveNAD;
} Protocol_LinConfig_t;

typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t Channel;
    uint8_t TableIdx;
} Protocol_LinTable_t;

// Payload for incoming DATA_REPORT_LIN_MSG (253).
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;

    uint8_t  Channel;   // Zero-based LIN channel index
    uint8_t  ID;        // LIN frame identifier (0-63)
    uint8_t  DLC;       // Number of data bytes
    uint8_t  Direction; // SYSTEM_ADD_LIN_FRAME:  0 = subscriber, 1 = publisher
                        // DATA_REPORT_LIN_MSG:  0 = publisher echo (TX), 1 = subscriber response (RX)
    uint8_t  Delay;     // Inter-frame delay in ms
    uint8_t  Flags;     // Bit 0: responded, bit 1: valid checksum, bit 2: sleep event, bit 3: wakeup event
    uint32_t Time;      // Device timestamp (µs, wraps around)
    uint8_t  Data[8];   // LIN payload bytes
} Protocol_LinFrame_t;

typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t Channel;
    uint8_t ID;
    uint8_t Data[8];
} Protocol_LinData_t;

typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t Channel;   // Zero-based LIN channel index
    uint8_t Action;    // LIN_SLEEP_WAKEUP_ACTION_SLEEP / LIN_SLEEP_WAKEUP_ACTION_WAKEUP
} Protocol_LinSleepWakeup_t;

// Payload for SYSTEM_SEND_GPIO_CFG (32) — configure pin directions and auto-report interval.
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t  CycleTime_ms;  // Auto-report interval in ms (0 = disabled, minimum 5)
    uint16_t PinDirection;  // Bitmask: bit N = 1 → output, 0 → input
} Protocol_GPIO_Config_t;

// Payload for SYSTEM_SET_GPIO_OUTPUT (33) — set digital output levels.
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint16_t PinOutputState; // Bitmask: bit N = 1 → high, 0 → low
} Protocol_GPIO_Output_t;

// Payload for incoming DATA_REPORT_GPIO (252).
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint16_t PinState;      // Bitmask: bit N = digital state of pin N
    uint16_t Voltage_mV[8]; // Measured voltage per pin in mV
} Protocol_GpioStatus_t;

#define DATA_REPORT_GPIO            252u
#define DATA_CHANNEL_CAPABILITIES   251u

// Payload for SYSTEM_GET_CHANNEL_CAPABILITIES (34) and incoming DATA_CHANNEL_CAPABILITIES (251).
typedef struct __attribute__((packed))
{
    Protocol_SystemHeader_t Header;
    uint8_t  BusType;       // 0 = CAN, 1 = LIN
    uint8_t  Channel;       // Zero-based channel index
    uint32_t Capabilities;  // Bitmask of supported features (request: ignored; response: filled by device)
} Protocol_ChannelCapabilities_t;


// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

GrIPHandler::GrIPHandler(const QString &name)
{
    m_Exit = false;
    m_ChannelsCAN = 0;
    m_ChannelsCANFD = 0;
    m_PortName = name;

    CRC_Init();
}

GrIPHandler::~GrIPHandler()
{
    Stop();
    delete m_SerialPort;
}

bool GrIPHandler::Start()
{
    m_Exit = false;
    m_WorkerReady = false;

    m_pWorkerThread = QThread::create([this]() { WorkerThread(); });
    m_pWorkerThread->start();

    // Block until the worker thread signals that the port open attempt has completed.
    {
        std::unique_lock<std::mutex> lck(m_MutexReady);
        m_CvReady.wait(lck, [this] { return m_WorkerReady; });
    }

    // m_SerialPort is nullptr if the port failed to open.
    return m_SerialPort != nullptr;
}

void GrIPHandler::Stop()
{
    // Signal the worker loop to exit, then block until it does.
    m_Exit = true;

    if (m_pWorkerThread)
    {
        m_pWorkerThread->wait();
        delete m_pWorkerThread;
        m_pWorkerThread = nullptr;
    }

    // The worker thread has exited — it is now safe to close the port.
    std::unique_lock<std::mutex> lck(m_MutexSerial);

    if (m_SerialPort && m_SerialPort->isOpen())
    {
        m_SerialPort->waitForBytesWritten(20); // Flush any pending TX bytes
        m_SerialPort->clear();
        m_SerialPort->close();
    }
}

// ---------------------------------------------------------------------------
// Device configuration commands
// ---------------------------------------------------------------------------

void GrIPHandler::SetStatus(bool open)
{
    Protocol_SystemHeader_t header;

    header.Version = GRIP_HEADER_VERSION;
    header.Command = SYSTEM_SET_STATUS;
    header.Length = 0;
    header.Data = open ? 2 : 1; // 1 = closed, 2 = open

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&header), sizeof(Protocol_SystemHeader_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::SetEchoTx(bool enable)
{
    Protocol_SystemHeader_t header;

    header.Version = GRIP_HEADER_VERSION;
    header.Command = SYSTEM_CAN_TXECHO;
    header.Length = 0;
    header.Data = enable ? 1 : 0;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&header), sizeof(Protocol_SystemHeader_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::RequestVersion()
{
    Protocol_SystemHeader_t header;

    header.Version = GRIP_HEADER_VERSION;
    header.Command = SYSTEM_REPORT_INFO;
    header.Length = 0;
    header.Data = 0;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&header), sizeof(Protocol_SystemHeader_t)};

    // Reset cached state under m_MutexData — GetVersion() / Channels_*() read
    // these fields under the same lock, so the clear must also hold it.
    {
        std::unique_lock<std::mutex> dataLck(m_MutexData);
        m_Version.clear();
        m_ChannelsCAN = 0;
        m_ChannelsCANFD = 0;
    }

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);

    // Brief wait for the reply to arrive and be processed before returning,
    // so the caller can read GetVersion() immediately afterwards.
    QThread::msleep(10);
}

// ---------------------------------------------------------------------------
// Device state accessors (thread-safe reads)
// ---------------------------------------------------------------------------

std::string GrIPHandler::GetVersion() const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    return m_Version;
}

int GrIPHandler::Channels_CAN() const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    return m_ChannelsCAN;
}

int GrIPHandler::Channels_CANFD() const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    return m_ChannelsCANFD;
}

int GrIPHandler::Channels_LIN() const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    return m_ChannelsLIN;
}

int GrIPHandler::Channels_LinScheduleTables() const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    return m_LinScheduleTables;
}

int GrIPHandler::MaxCyclicCAN() const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    return m_MaxCyclicCAN;
}

// ---------------------------------------------------------------------------
// Low-level send helpers
// ---------------------------------------------------------------------------

void GrIPHandler::Send(GrIP_ProtocolType_e ProtType, GrIP_MessageType_e MsgType, GrIP_ReturnType_e ReturnCode, const GrIP_Pdu_t *pdu)
{
    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(ProtType, MsgType, ReturnCode, pdu);
}

void GrIPHandler::Send(GrIP_ProtocolType_e ProtType, GrIP_MessageType_e MsgType, GrIP_ReturnType_e ReturnCode, const uint8_t *data, uint16_t len)
{
    // GrIP_Pdu_t holds a non-const pointer; the cast is safe because GrIP
    // only reads the buffer for TX purposes.
    GrIP_Pdu_t pdu = {const_cast<uint8_t *>(data), len};
    Send(ProtType, MsgType, ReturnCode, &pdu);
}

// ---------------------------------------------------------------------------
// CAN channel control
// ---------------------------------------------------------------------------

void GrIPHandler::CanEnableChannel(uint8_t ch, bool enable)
{
    Protocol_ChannelStatus_t status = {};

    status.Header.Version = GRIP_HEADER_VERSION;
    status.Header.Command = SYSTEM_START_CAN;
    status.Header.Length = 2;
    status.Header.Data = 0;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&status), sizeof(Protocol_ChannelStatus_t)};

    if (ch < m_Channel_StatusCAN.size())
    {
        // Update local shadow and rebuild the full channel state word.
        // The firmware expects the enable state of all channels at once,
        // not just the one being changed. Protect the channel vector with
        // m_MutexData which guards receive queues and channel state.
        {
            std::unique_lock<std::mutex> dataLck(m_MutexData);
            m_Channel_StatusCAN[ch] = enable;
            status.Channel1 = m_Channel_StatusCAN.size() > 0 ? m_Channel_StatusCAN[0] : 0;
            status.Channel2 = m_Channel_StatusCAN.size() > 1 ? m_Channel_StatusCAN[1] : 0;
        }

        std::unique_lock<std::mutex> lck(m_MutexSerial);

        if (!enable)
        {
            m_SerialPort->clear(QSerialPort::Output); // Flush TX queues for this channel when disabling it
        }

        std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
    }
}

void GrIPHandler::LinEnableChannel(uint8_t ch, bool enable)
{
    if (ch >= m_Channel_StatusLIN.size())
        return;

    Protocol_ChannelEnable_t cmd = {};
    cmd.Header.Version = GRIP_HEADER_VERSION;
    cmd.Header.Command = SYSTEM_START_LIN;
    cmd.Header.Length  = 2; // sizeof(Channel) + sizeof(Enable)
    cmd.Channel        = ch;
    cmd.Enable         = enable ? 1u : 0u;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&cmd), sizeof(Protocol_ChannelEnable_t)};

    {
        std::unique_lock<std::mutex> dataLck(m_MutexData);
        m_Channel_StatusLIN[ch] = enable;
    }

    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::CanSetMode(uint8_t ch, bool listen_only)
{
    Protocol_ChannelParam_t mode = {};

    mode.Header.Version = GRIP_HEADER_VERSION;
    mode.Header.Command = SYSTEM_CAN_MODE;
    mode.Header.Length = sizeof(Protocol_ChannelParam_t) - sizeof(Protocol_SystemHeader_t);
    mode.Header.Data = 0;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&mode), sizeof(Protocol_ChannelParam_t)};

    mode.Channel = ch;
    mode.Param = static_cast<uint8_t>(listen_only);

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::LinSetScheduleTable(uint8_t ch, uint8_t table_idx)
{
    Protocol_LinTable_t tbl_cfg;

    tbl_cfg.Header.Version = GRIP_HEADER_VERSION;
    tbl_cfg.Header.Command = SYSTEM_LIN_SET_TABLE;
    tbl_cfg.Header.Length = sizeof(Protocol_LinTable_t) - sizeof(Protocol_SystemHeader_t);
    tbl_cfg.Header.Data = 0;

    tbl_cfg.Channel = ch;
    tbl_cfg.TableIdx = table_idx;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&tbl_cfg), sizeof(Protocol_LinTable_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::LinSleepWakeup(uint8_t ch, bool wakeup)
{
    Protocol_LinSleepWakeup_t cmd = {};

    cmd.Header.Version = GRIP_HEADER_VERSION;
    cmd.Header.Command = SYSTEM_LIN_SLEEP_WAKEUP;
    cmd.Header.Length = sizeof(Protocol_LinSleepWakeup_t) - sizeof(Protocol_SystemHeader_t);
    cmd.Header.Data = 0;

    cmd.Channel = ch;
    cmd.Action = wakeup ? LIN_SLEEP_WAKEUP_ACTION_WAKEUP : LIN_SLEEP_WAKEUP_ACTION_SLEEP;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&cmd), sizeof(Protocol_LinSleepWakeup_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::CanSetBaudrate(uint8_t ch, uint32_t baud)
{
    Protocol_ChannelParam_t cfg = {};

    cfg.Header.Version = GRIP_HEADER_VERSION;
    cfg.Header.Command = SYSTEM_SEND_CAN_CFG;
    cfg.Header.Length = sizeof(Protocol_ChannelParam_t) - sizeof(Protocol_SystemHeader_t);
    cfg.Header.Data = 0;

    cfg.Channel  = ch;
    cfg.Param = (baud); // Device expects big-endian

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&cfg), sizeof(Protocol_ChannelParam_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);

    // Allow the device time to reconfigure its CAN hardware before the next
    // command arrives.
    QThread::msleep(4);
}

void GrIPHandler::CanSetConfig(uint8_t ch, uint32_t baud, bool listen, bool echoTx, bool abom)
{
    Protocol_CanConfig_t cfg = {};

    cfg.Header.Version = GRIP_HEADER_VERSION;
    cfg.Header.Command = SYSTEM_SEND_CAN_CFG;
    cfg.Header.Length = sizeof(Protocol_CanConfig_t) - sizeof(Protocol_SystemHeader_t);
    cfg.Header.Data = 0;

    cfg.Channel = ch;
    cfg.Baudrate = baud;
    cfg.EchoTx = echoTx;
    cfg.ABOM = abom;
    cfg.ListenMode = listen;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&cfg), sizeof(Protocol_CanConfig_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::CanSetFdConfig(uint8_t ch, uint32_t arbBaud, uint32_t dataBaud, bool listen, bool echoTx, bool abom)
{
    Protocol_CanFdConfig_t cfg = {};

    cfg.Header.Version  = GRIP_HEADER_VERSION;
    cfg.Header.Command  = SYSTEM_SEND_CANFD_CFG;
    cfg.Header.Length   = sizeof(Protocol_CanFdConfig_t) - sizeof(Protocol_SystemHeader_t);
    cfg.Header.Data     = 0;

    cfg.Channel      = ch;
    cfg.ArbBaudrate  = arbBaud;
    cfg.DataBaudrate = dataBaud;
    cfg.EchoTx       = echoTx;
    cfg.ABOM         = abom;
    cfg.ListenMode   = listen;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&cfg), sizeof(Protocol_CanFdConfig_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::LinSetConfig(uint8_t ch, uint32_t baud, uint8_t mode, uint8_t protocol,
                               uint8_t timebase, uint16_t jitter_us,
                               uint16_t diagSTmin_ms, uint16_t diagP2min_ms,
                               uint16_t diagNAs_ms, uint16_t diagNCr_ms,
                               uint8_t slaveNad)
{
    Protocol_LinConfig_t cfg = {};

    cfg.Header.Version = GRIP_HEADER_VERSION;
    cfg.Header.Command = SYSTEM_SEND_LIN_CFG;
    cfg.Header.Length  = sizeof(Protocol_LinConfig_t) - sizeof(Protocol_SystemHeader_t);
    cfg.Header.Data    = 0;

    cfg.Channel      = ch;
    cfg.Baudrate     = static_cast<uint16_t>(baud);
    cfg.Mode         = mode;
    cfg.Protocol     = protocol;
    cfg.Timebase     = timebase;
    cfg.Jitter       = jitter_us;
    cfg.DiagSTmin_ms = diagSTmin_ms;
    cfg.DiagP2min_ms = diagP2min_ms;
    cfg.DiagNAs_ms   = diagNAs_ms;
    cfg.DiagNCr_ms   = diagNCr_ms;
    cfg.SlaveNAD     = slaveNad;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&cfg), sizeof(Protocol_LinConfig_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::LinSendDiagRequest(uint8_t ch, uint8_t nad, const uint8_t *data, uint8_t len)
{
    if (!data || len == 0)
        return;

    // Wire layout after the 4-byte system header: [Channel, NAD, SID, d0, ...]
    // The firmware adds the PCI byte automatically based on payload length.
    // Total PDU = header (4) + Channel (1) + NAD (1) + payload (len)
    const uint16_t pduLen = static_cast<uint16_t>(sizeof(Protocol_SystemHeader_t) + 2u + len);
    std::vector<uint8_t> buf(pduLen, 0u);

    Protocol_SystemHeader_t *hdr = reinterpret_cast<Protocol_SystemHeader_t *>(buf.data());
    hdr->Version = GRIP_HEADER_VERSION;
    hdr->Command = SYSTEM_LIN_DIAG_REQ;
    hdr->Length  = static_cast<uint8_t>(2u + len);  // Channel + NAD + payload
    hdr->Data    = 0u;

    buf[sizeof(Protocol_SystemHeader_t) + 0] = ch;
    buf[sizeof(Protocol_SystemHeader_t) + 1] = nad;
    std::memcpy(buf.data() + sizeof(Protocol_SystemHeader_t) + 2, data, len);

    GrIP_Pdu_t p = {buf.data(), pduLen};

    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::GpioSetConfig(bool enable, uint8_t cycleTime_ms, uint16_t pinDirection)
{
    Protocol_GPIO_Config_t cfg = {};

    cfg.Header.Version = GRIP_HEADER_VERSION;
    cfg.Header.Command = SYSTEM_SEND_GPIO_CFG;
    cfg.Header.Length  = sizeof(Protocol_GPIO_Config_t) - sizeof(Protocol_SystemHeader_t);
    cfg.Header.Data    = enable ? 1u : 0u;

    cfg.CycleTime_ms = cycleTime_ms;
    cfg.PinDirection = pinDirection;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&cfg), sizeof(Protocol_GPIO_Config_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::GpioSetOutput(uint16_t pinOutputState)
{
    Protocol_GPIO_Output_t out = {};

    out.Header.Version = GRIP_HEADER_VERSION;
    out.Header.Command = SYSTEM_SET_GPIO_OUTPUT;
    out.Header.Length  = sizeof(Protocol_GPIO_Output_t) - sizeof(Protocol_SystemHeader_t);
    out.Header.Data    = 0;

    out.PinOutputState = pinOutputState;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&out), sizeof(Protocol_GPIO_Output_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

void GrIPHandler::RequestChannelCapabilities(uint8_t busType, uint8_t channel)
{
    Protocol_ChannelCapabilities_t req = {};

    req.Header.Version = GRIP_HEADER_VERSION;
    req.Header.Command = SYSTEM_GET_CHANNEL_CAPABILITIES;
    req.Header.Length  = sizeof(Protocol_ChannelCapabilities_t) - sizeof(Protocol_SystemHeader_t);
    req.Header.Data    = 0;

    req.BusType   = busType;
    req.Channel   = channel;

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&req), sizeof(Protocol_ChannelCapabilities_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);
    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

uint32_t GrIPHandler::GetChannelCapabilities(uint8_t busType, uint8_t channel) const
{
    const uint16_t key = static_cast<uint16_t>((busType << 8) | channel);
    std::unique_lock<std::mutex> lck(m_MutexData);
    auto it = m_ChannelCapabilities.find(key);
    return it != m_ChannelCapabilities.end() ? it->second : 0u;
}

void GrIPHandler::LinAddFrame(uint8_t ch, const BusMessage &msg, uint8_t frame_time, bool isSporadic)
{
    Protocol_LinFrame_t frame = {};

    frame.Header.Version = GRIP_HEADER_VERSION;
    frame.Header.Command = SYSTEM_ADD_LIN_FRAME;
    frame.Header.Length = sizeof(Protocol_LinFrame_t) - sizeof(Protocol_SystemHeader_t);
    frame.Header.Data = 0;

    frame.Channel   = ch;
    frame.ID        = static_cast<uint8_t>(msg.getId());
    frame.DLC       = msg.getLength();
    frame.Direction = msg.isRX() ? 0 : 1;
    frame.Delay     = frame_time;
    frame.Flags     = isSporadic ? LIN_FLAGS_SPORADIC : 0u;

    std::memcpy(frame.Data, msg.getData(), msg.getLength());

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&frame), sizeof(Protocol_LinFrame_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    std::ignore = GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p);
}

// ---------------------------------------------------------------------------
// CAN receive queue access
// ---------------------------------------------------------------------------

bool GrIPHandler::CanAvailable(uint8_t ch) const
{
    std::unique_lock<std::mutex> lck(m_MutexData);

    if (ch < m_ReceiveQueue.size())
    {
        return !m_ReceiveQueue[ch].empty();
    }

    return false;
}

uint8_t GrIPHandler::CanGetState(uint8_t ch) const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    if (ch < m_CanBusStatus.size())
    {
        return m_CanBusStatus[ch];
    }
    else
    {
        qWarning() << "GrIPHandler::CanGetState: channel index" << ch << "out of range (size:" << m_CanBusStatus.size() << ")";
    }
    return 0;
}

uint16_t GrIPHandler::CanGetRxDropCount(uint8_t ch) const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    if (ch < m_CanRxDropCount.size())
    {
        return m_CanRxDropCount[ch];
    }
    return 0;
}

uint8_t GrIPHandler::LinGetState(uint8_t ch) const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    if (ch < m_LinBusStatus.size())
    {
        return m_LinBusStatus[ch];
    }
    else
    {
        qWarning() << "GrIPHandler::LinGetState: channel index" << ch << "out of range (size:" << m_CanBusStatus.size() << ")";
    }
    return 0;
}

uint16_t GrIPHandler::GpioGetPinState() const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    return m_GPIO_PinState;
}

uint16_t GrIPHandler::GpioGetAnalogValue(uint8_t pin) const
{
    std::unique_lock<std::mutex> lck(m_MutexData);
    if (pin < 8)
        return m_GPIO_AnalogValues[pin];
    return 0;
}

BusMessage GrIPHandler::CanReceive(uint8_t ch)
{
    std::unique_lock<std::mutex> lck(m_MutexData);

    if (ch < m_ReceiveQueue.size() && !m_ReceiveQueue[ch].empty())
    {
        auto front = m_ReceiveQueue[ch].front();
        m_ReceiveQueue[ch].pop();
        return front;
    }

    return BusMessage();
}

bool GrIPHandler::LinAvailable(uint8_t ch) const
{
    std::unique_lock<std::mutex> lck(m_MutexData);

    if (ch < m_LinReceiveQueue.size())
    {
        return !m_LinReceiveQueue[ch].empty();
    }

    return false;
}

BusMessage GrIPHandler::LinReceive(uint8_t ch)
{
    std::unique_lock<std::mutex> lck(m_MutexData);

    if (ch < m_LinReceiveQueue.size() && !m_LinReceiveQueue[ch].empty())
    {
        auto front = m_LinReceiveQueue[ch].front();
        m_LinReceiveQueue[ch].pop();
        return front;
    }

    return BusMessage();
}

// ---------------------------------------------------------------------------
// CAN transmit
// ---------------------------------------------------------------------------

bool GrIPHandler::CanTransmit(uint8_t ch, const BusMessage &msg)
{
    Protocol_CanFrame_t frame = {};

    const uint8_t dataLen = static_cast<uint8_t>(std::min(static_cast<int>(msg.getLength()), 64));

    frame.Header.Version = GRIP_HEADER_VERSION;
    frame.Header.Command = SYSTEM_SEND_CAN_FRAME;
    frame.Header.Length = static_cast<uint16_t>(sizeof(Protocol_CanFrame_t) - sizeof(Protocol_SystemHeader_t) - 64u + dataLen);
    frame.Header.Data = 0;

    const uint16_t pduLen = static_cast<uint16_t>(sizeof(Protocol_CanFrame_t) - 64u + dataLen);
    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&frame), pduLen};

    frame.Channel = ch;
    frame.ID = msg.getId();
    frame.DLC = dataLen;
    frame.ErrFlags = 0;

    // Build flags from the BusMessage properties
    frame.Flags = 0;
    if (msg.isExtended())
    {
        frame.Flags |= CAN_FLAGS_EXT_ID;
    }
    if (msg.isFD())
    {
        frame.Flags |= CAN_FLAGS_FD;
    }
    if (msg.isBRS())
    {
        frame.Flags |= CAN_FLAGS_BRS;
    }
    if (msg.isRTR())
    {
        frame.Flags |= CAN_FLAGS_RTR;
    }

    for (int i = 0; i < dataLen; i++)
    {
        frame.Data[i] = msg.getByte(i);
    }

    // Compute a 32-bit FNV-1a hash over the frame's key fields and the current
    // monotonic time as a correlation token. The device echoes this value back
    // in the Time field, allowing TX echoes to be matched to their originating
    // send call.
    {
        constexpr uint32_t basis = 2166136261u;
        constexpr uint32_t prime = 16777619u;
        uint32_t hash = basis;
        const auto mix = [&](const uint8_t *data, size_t len) noexcept
        {
            for (size_t i = 0; i < len; i++) { hash ^= data[i]; hash *= prime; }
        };
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        mix(&frame.Channel, 1);
        mix(reinterpret_cast<const uint8_t *>(&frame.ID), sizeof(frame.ID));
        mix(&frame.DLC, 1);
        mix(&frame.Flags, 1);
        mix(reinterpret_cast<const uint8_t *>(&now), sizeof(now));
        frame.Time = hash;

        std::unique_lock<std::mutex> dataLck(m_MutexData);
        BusMessage pendingMsg = msg;
        pendingMsg.setTimestamp_ms(QDateTime::currentMSecsSinceEpoch());
        m_TxPending.insert_or_assign(hash, TxPendingEntry{ch, pendingMsg});
    }

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    // GrIP_Transmit returns 0 on success
    return GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p) == 0;
}

bool GrIPHandler::LinSendData(uint8_t ch, const BusMessage &msg)
{
    Protocol_LinData_t frame = {};

    frame.Header.Version = GRIP_HEADER_VERSION;
    frame.Header.Command = SYSTEM_SET_LIN_DATA;
    frame.Header.Length = sizeof(Protocol_LinData_t) - sizeof(Protocol_SystemHeader_t);
    frame.Header.Data = 0;

    frame.Channel = ch;
    frame.ID = msg.getId();

    std::memcpy(frame.Data, msg.getData(), msg.getLength());

    GrIP_Pdu_t p = {reinterpret_cast<uint8_t *>(&frame), sizeof(Protocol_LinConfig_t)};

    std::unique_lock<std::mutex> lck(m_MutexSerial);

    return GrIP_Transmit(PROT_GrIP, MSG_SYSTEM_CMD, RET_OK, &p) == 0;
}

// ---------------------------------------------------------------------------
// Packet dispatcher (called from WorkerThread, holds m_MutexData)
// ---------------------------------------------------------------------------

void GrIPHandler::ProcessData(GrIP_Packet_t &packet, qint64 rxTimestamp_ms)
{
    std::unique_lock<std::mutex> lck(m_MutexData);

    switch (packet.RX_Header.MsgType)
    {
    case MSG_SYSTEM_CMD:
    {
        const Protocol_SystemHeader_t* hdr =
            reinterpret_cast<const Protocol_SystemHeader_t*>(packet.Data);
        switch (hdr->Command)
        {
        case SYSTEM_REPORT_INFO:
        {
            Protocol_SystemInfoReply_t info;
            std::memcpy(&info, packet.Data, sizeof(Protocol_SystemInfoReply_t));
            info.BuildDate[sizeof(info.BuildDate) - 1] = '\0'; // Ensure null-termination

            m_Version = std::format("{}.{}-<{}>", info.Major, info.Minor, info.BuildDate);
            m_ChannelsCAN       = info.ChannelsCAN;
            m_ChannelsCANFD     = info.ChannelsCANFD;
            m_ChannelsLIN       = info.ChannelsLIN;
            m_LinScheduleTables = info.LinScheduleTables;
            m_MaxCyclicCAN      = info.MaxCyclicCAN;

            // Rebuild per-channel queues to match the reported channel count.
            // CAN-FD channels follow classic CAN channels in the same vectors.
            m_Channel_StatusCAN.clear();
            m_ReceiveQueue.clear();
            m_CanBusStatus.clear();
            m_CanRxDropCount.clear();
            m_Channel_StatusLIN.clear();

            for (int i = 0; i < info.ChannelsCAN + info.ChannelsCANFD; i++)
            {
                m_Channel_StatusCAN.push_back(false);
                m_ReceiveQueue.push_back({});
                m_CanBusStatus.push_back(CANIL_CAN_State::CAN_Off);
                m_CanRxDropCount.push_back(0);
            }
            m_LinReceiveQueue.clear();
            for (int i = 0; i < info.ChannelsLIN; i++)
            {
                m_Channel_StatusLIN.push_back(false);
                m_LinReceiveQueue.push_back({});
                m_LinBusStatus.push_back(CANIL_CAN_State::CAN_Off);
            }
            break;
        }

        default:
            break;
        }
        break;
    }

    case MSG_REALTIME_CMD:
        break;

    case MSG_DATA:
    case MSG_DATA_NO_RESPONSE:
    {
        // The sub-command is encoded in the header that precedes the frame data.
        Protocol_SystemHeader_t header;
        std::memcpy(&header, packet.Data, sizeof(Protocol_SystemHeader_t));

        switch (header.Command)
        {
        case 254u: // DATA_REPORT_CAN_MSG — incoming CAN frame from the bus
        {
            Protocol_CanFrame_t frame;
            std::memcpy(&frame, packet.Data, sizeof(Protocol_CanFrame_t));

            BusMessage msg(frame.ID);

            // Initialise all flags to false; set individually from frame.Flags below.
            msg.setBusType(BusType::CAN);
            msg.setErrorFrame(false);
            msg.setRTR(false);
            msg.setFD(false);
            msg.setBRS(false);
            msg.setLength(frame.DLC);
            msg.setRX(true);
            msg.setTimestamp_ms(rxTimestamp_ms);

            if (frame.Flags & CAN_FLAGS_EXT_ID)
            {
                msg.setExtended(true);
            }
            if (frame.Flags & CAN_FLAGS_FD)
            {
                msg.setFD(true);
            }
            if (frame.Flags & CAN_FLAGS_RTR)
            {
                msg.setRTR(true);
            }
            if (frame.Flags & CAN_FLAGS_BRS)
            {
                msg.setBRS(true);
            }

            if (frame.ErrFlags)
            {
                qDebug() << "CAN error flags:" << frame.ErrFlags;
                if (frame.ErrFlags & GrIP_ERR_STUFF)                  msg.setErrorFlag(BusError::Stuff);
                if (frame.ErrFlags & GrIP_ERR_FORM)                   msg.setErrorFlag(BusError::Form);
                if (frame.ErrFlags & GrIP_ERR_ACK)                    msg.setErrorFlag(BusError::Ack);
                if (frame.ErrFlags & (GrIP_ERR_BIT_REC | GrIP_ERR_BIT_DOM)) msg.setErrorFlag(BusError::Bit);
                if (frame.ErrFlags & GrIP_ERR_CRC)                    msg.setErrorFlag(BusError::Crc);
                if (frame.ErrFlags & GrIP_ERR_SW_SET)                 msg.setErrorFlag(BusError::Generic);
            }

            for (int i = 0; i < frame.DLC; i++)
            {
                msg.setByte(i, frame.Data[i]);
            }

            // Only enqueue if the channel is currently enabled
            if (frame.Channel < m_Channel_StatusCAN.size() && m_Channel_StatusCAN[frame.Channel])
            {
                m_ReceiveQueue[frame.Channel].push(msg);
            }

            break;
        }

        case 253u: // DATA_REPORT_LIN_MSG — incoming LIN frame from the bus
        {
            Protocol_LinFrame_t frame;
            std::memcpy(&frame, packet.Data, sizeof(Protocol_LinFrame_t));

            constexpr uint8_t LIN_FLAG_RESPONDED      = 0x01;
            constexpr uint8_t LIN_FLAG_VALID_CHECKSUM = 0x02;
            constexpr uint8_t LIN_FLAG_SLEEP          = 0x04;
            constexpr uint8_t LIN_FLAG_WAKEUP         = 0x08;

            const bool isNormal        = (frame.Flags & (LIN_FLAG_RESPONDED | LIN_FLAG_VALID_CHECKSUM))
                                          == (LIN_FLAG_RESPONDED | LIN_FLAG_VALID_CHECKSUM);
            const bool isSleepOrWakeup = (frame.Flags & (LIN_FLAG_SLEEP | LIN_FLAG_WAKEUP)) != 0;

            BusMessage msg(frame.ID);
            msg.setBusType(BusType::LIN);
            if (!isSleepOrWakeup)
            {
                const bool responded     = (frame.Flags & LIN_FLAG_RESPONDED)      != 0;
                const bool validChecksum = (frame.Flags & LIN_FLAG_VALID_CHECKSUM) != 0;
                if (!responded)
                    msg.setErrorFlag(BusError::LinNotResponded);
                else if (!validChecksum)
                    msg.setErrorFlag(BusError::LinChecksumError);
            }
            msg.setFlags(frame.Flags);
            msg.setLength(frame.DLC);
            msg.setRX(frame.Direction == 1); // 1 = subscriber response (RX), 0 = publisher echo (TX)
            msg.setTimestamp_ms(rxTimestamp_ms);

            for (int i = 0; i < frame.DLC; i++)
            {
                msg.setByte(i, frame.Data[i]);
            }

            // Only enqueue if the channel is enabled
            if (frame.Channel < m_Channel_StatusLIN.size() && m_Channel_StatusLIN[frame.Channel])
            {
                m_LinReceiveQueue[frame.Channel].push(msg);
            }
            /*qDebug() << "LIN MSG — ID:" << frame.ID
                     << "DLC:" << frame.DLC
                     << "Alive:" << (frame.Flags & 0x1)
                     << "Valid:" << ((frame.Flags & 0x2) >> 1);*/

            break;
        }

        case 220u:  // CAN Status Frame
        {
            Protocol_BusStatus_t frame;
            std::memcpy(&frame, packet.Data, std::min(sizeof(Protocol_BusStatus_t), (size_t)packet.RX_Header.Length + sizeof(Protocol_SystemHeader_t)));

            for (size_t i = 0; i < m_CanBusStatus.size(); i++)
            {
                m_CanBusStatus[i] = frame.BusState[i];
            }
            for (size_t i = 0; i < m_CanRxDropCount.size(); i++)
            {
                m_CanRxDropCount[i] = frame.RxDropCount[i];
            }
            //qDebug() << "CAN Status: " << frame.BusState[0] << " - " << frame.BusState[1];
            break;
        }

        case 219u:  // LIN Status Frame
        {
            Protocol_BusStatus_t frame;
            std::memcpy(&frame, packet.Data, std::min(sizeof(Protocol_BusStatus_t),
                static_cast<size_t>(packet.RX_Header.Length) + sizeof(Protocol_SystemHeader_t)));

            for (size_t i = 0; i < m_LinBusStatus.size(); i++)
            {
                m_LinBusStatus[i] = frame.BusState[i];
            }
            //qDebug() << "LIN Status: " << frame.BusState[0] << " - " << frame.BusState[1];
            break;
        }

        case 209u:
        {
            Protocol_CanTxEcho_t frame;
            std::memcpy(&frame, packet.Data, sizeof(Protocol_CanTxEcho_t));

            auto it = m_TxPending.find(frame.Hash);
            if (it != m_TxPending.end())
            {
                auto &entry = it->second;

                const qint64 age = QDateTime::currentMSecsSinceEpoch() - entry.msg.getTimestamp_ms();
                if (entry.errorReported)
                {
                    qDebug() << "Late TX echo received (hash=" << frame.Hash << ", age=" << age << "ms)";
                }

                entry.msg.setRX(false);
                if (frame.Header.Data != 0)
                {
                    const uint8_t ef = frame.Header.Data;
                    if (ef & GrIP_ERR_STUFF)                   entry.msg.setErrorFlag(BusError::Stuff);
                    if (ef & GrIP_ERR_FORM)                    entry.msg.setErrorFlag(BusError::Form);
                    if (ef & GrIP_ERR_ACK)                     entry.msg.setErrorFlag(BusError::Ack);
                    if (ef & (GrIP_ERR_BIT_REC | GrIP_ERR_BIT_DOM)) entry.msg.setErrorFlag(BusError::Bit);
                    if (ef & GrIP_ERR_CRC)                     entry.msg.setErrorFlag(BusError::Crc);
                    if (ef & GrIP_ERR_SW_SET)                  entry.msg.setErrorFlag(BusError::Generic);
                }

                if (entry.ch < m_ReceiveQueue.size())
                {
                    m_ReceiveQueue[entry.ch].push(entry.msg);
                }
                m_TxPending.erase(it);
            }
            break;
        }

        case DATA_REPORT_GPIO:
        {
            Protocol_GpioStatus_t frame = {};
            std::memcpy(&frame, packet.Data, std::min(sizeof(Protocol_GpioStatus_t),
                static_cast<size_t>(packet.RX_Header.Length) + sizeof(Protocol_SystemHeader_t)));

            m_GPIO_PinState = frame.PinState;
            std::memcpy(m_GPIO_AnalogValues, frame.Voltage_mV, sizeof(m_GPIO_AnalogValues));

            // Snapshot protected members before releasing the lock so the emit
            // cannot race with GpioGetPinState() / GpioGetAnalogValue().
            const uint16_t pinSnapshot = m_GPIO_PinState;
            const QVector<uint16_t> analogSnapshot(m_GPIO_AnalogValues, m_GPIO_AnalogValues + 8);
            lck.unlock();
            emit gpioUpdated(pinSnapshot, analogSnapshot);
            break;
        }

        case DATA_CHANNEL_CAPABILITIES:
        {
            Protocol_ChannelCapabilities_t cap = {};
            std::memcpy(&cap, packet.Data, std::min(sizeof(Protocol_ChannelCapabilities_t),
                static_cast<size_t>(packet.RX_Header.Length) + sizeof(Protocol_SystemHeader_t)));

            const uint16_t key = static_cast<uint16_t>((cap.BusType << 8) | cap.Channel);
            m_ChannelCapabilities[key] = cap.Capabilities;

            const uint8_t busType = cap.BusType;
            const uint8_t channel = cap.Channel;
            const uint32_t capabilities = cap.Capabilities;
            lck.unlock();
            emit channelCapabilitiesReceived(busType, channel, capabilities);
            break;
        }

        default:
            qDebug() << "Unknown command: " << header.Command;
            break;
        }
        break;
    }

    case MSG_NOTIFICATION:
    {
        // The device sends free-form log strings prefixed by a type byte.
        // packet.Data[0] = notification type (unused), [1..] = null-terminated string.
        qDebug() << "DEV:" << reinterpret_cast<const char *>(&packet.Data[1]);
        break;
    }

    case MSG_RESPONSE:
        break;

    case MSG_ERROR:
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void GrIPHandler::WorkerThread()
{
    // QSerialPort must be created on the thread that uses it.
    m_SerialPort = new QSerialPort();
    m_SerialPort->setPortName(m_PortName);
    m_SerialPort->setBaudRate(3000000);
    m_SerialPort->setDataBits(QSerialPort::Data8);
    m_SerialPort->setParity(QSerialPort::NoParity);
    m_SerialPort->setStopBits(QSerialPort::OneStop);
    m_SerialPort->setFlowControl(QSerialPort::NoFlowControl);
    m_SerialPort->setReadBufferSize(1024 * 8);

    GrIP_Init();

    if (!m_SerialPort->open(QIODevice::ReadWrite))
    {
        qCritical() << "Serial port open failed:" << m_SerialPort->errorString();
        delete m_SerialPort;
        m_SerialPort = nullptr;

        // Notify Start() that the open attempt is done (failed).
        {
            std::unique_lock<std::mutex> lck(m_MutexReady);
            m_WorkerReady = true;
        }
        m_CvReady.notify_one();
        return;
    }

    qRegisterMetaType<QSerialPort::SerialPortError>("SerialThread");
    connect(m_SerialPort, &QSerialPort::errorOccurred, this, &GrIPHandler::handleSerialError);

    // Discard any stale data left in hardware buffers from a previous session.
    m_SerialPort->flush();
    m_SerialPort->clear();
    m_SerialPort->waitForReadyRead(50);

    // Notify Start() that the port is open and ready for commands.
    {
        std::unique_lock<std::mutex> lck(m_MutexReady);
        m_WorkerReady = true;
    }
    m_CvReady.notify_one();

    while (!m_Exit)
    {
        bool hadData = false;

        // --- RX: read bytes from the port and feed the GrIP framer ---
        m_SerialPort->waitForReadyRead(1);

        // Capture timestamp as close to the read as possible
        qint64 rxTimestamp_ms = QDateTime::currentMSecsSinceEpoch();

        if (m_SerialPort->bytesAvailable())
        {
            const QByteArray data = m_SerialPort->readAll();
            if (!data.isEmpty())
            {
                GrIP_RxCallback(data);
                hadData = true;
            }
        }

        // --- TX: flush any bytes queued by GrIP_Transmit calls ---
        const auto tx = GrIP_GetTxData();
        if (!tx.isEmpty())
        {
            m_SerialPort->write(tx);
            m_SerialPort->flush();
        }

        // --- Advance GrIP state machines (retransmit timers, ACK tracking, etc.) ---
        for (int i = 0; i < 512; i++)
        {
            GrIP_Update();
        }

        // --- Dispatch fully decoded packets, up to 32 per iteration ---
        for (int i = 0; i < 32; i++)
        {
            GrIP_Packet_t dat = {};
            if (GrIP_Receive(&dat))
            {
                ProcessData(dat, rxTimestamp_ms);
                hadData = true;
            }
            else
            {
                break; // No more packets queued
            }
        }

        // --- Expire TX frames that never received an echo (rate-limited to 100 ms) ---
        static qint64 lastPurgeMs = 0;
        if (rxTimestamp_ms - lastPurgeMs >= 100)
        {
            PurgeStaleTxPending(1000);
            lastPurgeMs = rxTimestamp_ms;
        }

        // Only sleep when idle — skip the delay when data is flowing
        // to minimise timestamp jitter on back-to-back frames.
        if (!hadData)
        {
            QThread::usleep(500);
        }
    }
}

// ---------------------------------------------------------------------------
// TX pending timeout
// ---------------------------------------------------------------------------

void GrIPHandler::PurgeStaleTxPending(qint64 timeout_ms)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::unique_lock<std::mutex> lck(m_MutexData);

    for (auto it = m_TxPending.begin(); it != m_TxPending.end(); )
    {
        auto &[hash, entry] = *it;
        const qint64 age = now - entry.msg.getTimestamp_ms();

        if (age >= 5000)
        {
            qDebug() << "TxPending abandoned — no echo after 5s (hash=" << hash << ")";
            it = m_TxPending.erase(it);
        }
        else
        {
            if (age >= timeout_ms && !entry.errorReported)
            {
                entry.msg.setRX(false);
                entry.msg.setErrorFrame(true);
                if (entry.ch < m_ReceiveQueue.size())
                {
                    m_ReceiveQueue[entry.ch].push(entry.msg);
                }
                entry.errorReported = true;
                qDebug() << "TxPending timeout — marked as error, waiting for late echo (hash=" << hash << ", age=" << age << "ms)";
            }
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Serial error handler (Qt slot)
// ---------------------------------------------------------------------------

void GrIPHandler::handleSerialError(QSerialPort::SerialPortError error)
{
    static const auto toErrorString = [](QSerialPort::SerialPortError err) -> QString
    {
        switch (err)
        {
        case QSerialPort::NoError:
            return {};
        case QSerialPort::DeviceNotFoundError:
            return QStringLiteral("Device not found");
        case QSerialPort::PermissionError:
            return QStringLiteral("Permission denied");
        case QSerialPort::OpenError:
            return QStringLiteral("Open error");
        case QSerialPort::WriteError:
            return QStringLiteral("Write error");
        case QSerialPort::ReadError:
            return QStringLiteral("Read error");
        case QSerialPort::ResourceError:
            return QStringLiteral("Resource error");
        case QSerialPort::UnsupportedOperationError:
            return QStringLiteral("Unsupported operation");
        case QSerialPort::TimeoutError:
            return {}; // Transient; not worth logging
        case QSerialPort::NotOpenError:
            return QStringLiteral("Not open error");
        default:
            return QStringLiteral("Unknown error");
        }
    };

    const QString msg = toErrorString(error);
    if (!msg.isEmpty())
    {
        qWarning() << "Serial port error:" << msg;
    }
}
