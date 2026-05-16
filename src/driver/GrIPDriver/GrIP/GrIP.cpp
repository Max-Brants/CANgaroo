#include "GrIP.h"
#include "CRC.h"

#include <cstring>
#include <cctype>
#include <queue>

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>

// ═════════════════════════════════════════════════════════════════════════════
// Internal constants
// ═════════════════════════════════════════════════════════════════════════════

static constexpr uint8_t  GRIP_SOH          = 0x01; ///< Start of header
static constexpr uint8_t  GRIP_SOT          = 0x02; ///< Start of text (payload)
static constexpr uint8_t  GRIP_EOT          = 0x03; ///< End of transmission

static constexpr uint8_t  MAX_READ_PER_TICK = 128u;
static constexpr uint16_t GRIP_HEADER_SIZE  = sizeof(GrIP_PacketHeader_t);

/// Wire buffer: header hex-encoded (×2) + SOH/SOT/EOT + payload hex-encoded (×2)
static constexpr size_t TX_BUF_SIZE =
    1u                    // SOH
    + GRIP_HEADER_SIZE * 2u
    + 1u                  // SOT
    + GRIP_BUFFER_SIZE * 2u
    + 1u                  // EOT
    + 4u;                 // safety margin

// ═════════════════════════════════════════════════════════════════════════════
// State machine
// ═════════════════════════════════════════════════════════════════════════════

enum class GrIPState : uint8_t
{
    Idle,
    RxHeader,
    WaitSOT,
    RxData,
    Finish,
};

// ═════════════════════════════════════════════════════════════════════════════
// Context — replaces all former file-scope globals
// ═════════════════════════════════════════════════════════════════════════════

struct GrIPContext
{
    // State machine
    GrIPState state        = GrIPState::Idle;
    bool      sendResponse = false;
    uint8_t   maxReadLeft  = MAX_READ_PER_TICK;
    uint32_t  bytesRead    = 0u;
    int       lastResponse = -1;

    // In-progress receive packet
    GrIP_Packet_t rxPacket = {};

    // Error counters
    GrIP_ErrorFlags_t errors = {};

    // Thread-safe byte streams
    QByteArray rxStream;   ///< data arriving from the serial port
    QByteArray txStream;   ///< data waiting to be written to the serial port
    QMutex     streamMtx;

    // Completed-packet queue (consumed by GrIP_Receive)
    std::queue<GrIP_Packet_t> packetQueue;
    QMutex                    queueMtx;
};

/// Single module-level instance — construction is zero-initialised by default.
static GrIPContext ctx;

// ═════════════════════════════════════════════════════════════════════════════
// Private helpers — encoding / decoding
// ═════════════════════════════════════════════════════════════════════════════

/// Map a nibble (0–15) to its uppercase hex character.
static inline char nibbleToHex(uint8_t nibble) noexcept
{
    static constexpr char kHexTable[] = "0123456789ABCDEF";
    return kHexTable[nibble & 0x0Fu];
}

/// Convert a single hex character to its value (0–15).
/// Behaviour is undefined for non-hex characters.
static inline uint8_t hexDigitToVal(char c) noexcept
{
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return 0u;
}

/// Decode a fixed-length hex string to an unsigned integer (big-endian nibble order).
static uint32_t hexStringToUint(const char *str, uint8_t len) noexcept
{
    uint32_t val = 0u;
    for (uint8_t i = 0; i < len; ++i)
        val = (val << 4u) | hexDigitToVal(str[i]);
    return val;
}

// ═════════════════════════════════════════════════════════════════════════════
// Private helpers — stream I/O (all require streamMtx to be held by caller)
// ═════════════════════════════════════════════════════════════════════════════

/// Enqueue bytes into the TX stream.
static void streamWrite(const uint8_t *data, qint64 len)
{
    ctx.txStream.append(reinterpret_cast<const char *>(data), len);
}

/// Dequeue up to `len` bytes from the RX stream into `dest`.
/// Returns the number of bytes actually read.
static qint64 streamRead(uint8_t *dest, qint64 len)
{
    const qint64 available = ctx.rxStream.size();
    len = qMin(len, available);
    if (len == 0) return 0;

    std::memcpy(dest, ctx.rxStream.constData(), static_cast<size_t>(len));
    ctx.rxStream.remove(0, static_cast<int>(len));
    return len;
}

/// Returns the number of bytes currently in the RX stream.
static qint64 streamAvailable() noexcept
{
    return ctx.rxStream.size();
}

// ═════════════════════════════════════════════════════════════════════════════
// Private helpers — packet handling
// ═════════════════════════════════════════════════════════════════════════════

static uint8_t validateHeader(const GrIP_PacketHeader_t &hdr)
{
    if (hdr.Version != GRIP_VERSION)
        return RET_WRONG_VERSION;

    if (hdr.MsgType >= MSG_MAX_NUM)
        return RET_WRONG_TYPE;

    const uint8_t expectedCRC = CRC_CalculateCRC8(
        reinterpret_cast<const uint8_t *>(&hdr), GRIP_HEADER_SIZE - 2u);

    if (hdr.CRC_Header != expectedCRC)
    {
        ++ctx.errors.CRC_Error;
        return RET_WRONG_CRC;
    }

    return RET_OK;
}

/// Called when a well-formed packet is ready; pushes it into the queue.
static void dispatchPacket(const GrIP_Packet_t &pkt)
{
    switch (pkt.RX_Header.Protocol)
    {
    case PROT_GrIP:  /* TODO: Protocol_ProcessMsg(pkt) */ break;
    case PROT_BoOTA: /* TODO: BoOTA handler            */ break;
    default:
        qWarning() << "GrIP: unknown protocol" << pkt.RX_Header.Protocol;
        break;
    }

    QMutexLocker lk(&ctx.queueMtx);
    ctx.packetQueue.push(pkt);
}

// ═════════════════════════════════════════════════════════════════════════════
// Private helpers — wire encoding
// ═════════════════════════════════════════════════════════════════════════════

/// Serialise a header struct into the TX buffer starting at `idx`.
/// Returns the updated index.
static unsigned int encodeHeader(const GrIP_PacketHeader_t &hdr, uint8_t *buf, unsigned int idx)
{
    buf[idx++] = GRIP_SOH;
    const auto *raw = reinterpret_cast<const uint8_t *>(&hdr);
    for (uint16_t i = 0; i < GRIP_HEADER_SIZE; ++i)
    {
        buf[idx++] = static_cast<uint8_t>(nibbleToHex(raw[i] >> 4u));
        buf[idx++] = static_cast<uint8_t>(nibbleToHex(raw[i]));
    }
    return idx;
}

/// Serialise a payload into the TX buffer starting at `idx`.
/// Returns the updated index.
static unsigned int encodePayload(const uint8_t *data, uint16_t len, uint8_t *buf, unsigned int idx)
{
    buf[idx++] = GRIP_SOT;
    for (uint16_t i = 0; i < len && i < GRIP_BUFFER_SIZE; ++i)
    {
        buf[idx++] = static_cast<uint8_t>(nibbleToHex(data[i] >> 4u));
        buf[idx++] = static_cast<uint8_t>(nibbleToHex(data[i]));
    }
    return idx;
}

// ═════════════════════════════════════════════════════════════════════════════
// Public API
// ═════════════════════════════════════════════════════════════════════════════

void GrIP_Init()
{
    // Reset state machine
    ctx.state        = GrIPState::Idle;
    ctx.sendResponse = false;
    ctx.maxReadLeft  = MAX_READ_PER_TICK;
    ctx.bytesRead    = 0u;
    ctx.lastResponse = -1;

    // Clear buffers (mutexes stay in place)
    {
        QMutexLocker lk(&ctx.streamMtx);
        ctx.rxStream.clear();
        ctx.txStream.clear();
    }
    {
        QMutexLocker lk(&ctx.queueMtx);
        ctx.packetQueue = {};
    }

    // Clear packet and error state
    ctx.rxPacket = {};
    ctx.errors   = {};
    ctx.txStream.clear();
}

// ─────────────────────────────────────────────────────────────────────────────

uint8_t GrIP_TransmitArray(GrIP_ProtocolType_e prot,
                           GrIP_MessageType_e  msg,
                           GrIP_ReturnType_e   ret,
                           const uint8_t      *data,
                           uint16_t            len)
{
    GrIP_Pdu_t pdu{ data, len };
    return GrIP_Transmit(prot, msg, ret, &pdu);
}

// ─────────────────────────────────────────────────────────────────────────────

uint8_t GrIP_Transmit(GrIP_ProtocolType_e prot,
                      GrIP_MessageType_e  msg,
                      GrIP_ReturnType_e   retCode,
                      const GrIP_Pdu_t   *pdu)
{
    GrIP_PacketHeader_t hdr{};
    hdr.Version    = GRIP_VERSION;
    hdr.Protocol   = prot;
    hdr.MsgType    = msg;
    hdr.ReturnCode = retCode;

    if (pdu)
    {
        if (pdu->Length > GRIP_BUFFER_SIZE)
        {
            qWarning() << "GrIP_Transmit: payload too large" << pdu->Length;
            return RET_NOK;
        }

        hdr.Length   = pdu->Length;
        hdr.CRC_Data = (pdu->Length > 0u)
                           ? CRC_CalculateCRC8(pdu->Data, pdu->Length)
                           : 0u;
    }
    else
    {
        hdr.Length   = 0u;
        hdr.CRC_Data = 0u;
    }

    hdr.CRC_Header = CRC_CalculateCRC8(reinterpret_cast<const uint8_t *>(&hdr), GRIP_HEADER_SIZE - 2u);

    // Build wire frame into a stack-local buffer — safe to call from multiple threads simultaneously
    uint8_t localTxBuf[TX_BUF_SIZE] = {};
    unsigned int idx = encodeHeader(hdr, localTxBuf, 0u);

    if (pdu && pdu->Length > 0u)
        idx = encodePayload(pdu->Data, pdu->Length, localTxBuf, idx);

    localTxBuf[idx++] = GRIP_EOT;

    // Push to TX stream (thread-safe)
    {
        QMutexLocker lk(&ctx.streamMtx);
        streamWrite(localTxBuf, static_cast<qint64>(idx));
    }

    return RET_OK;
}

// ─────────────────────────────────────────────────────────────────────────────

uint8_t GrIP_SendSync()
{
    return GrIP_Transmit(PROT_GrIP, MSG_SYNC, RET_OK, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────

void GrIP_Update()
{
    QMutexLocker lk(&ctx.streamMtx);

    switch (ctx.state)
    {
    // ── Idle: wait for start-of-header byte ──────────────────────────────────
    case GrIPState::Idle:
    {
        if (streamAvailable() < 1) break;

        uint8_t magic = 0u;
        streamRead(&magic, 1);

        if (magic == GRIP_SOH)
        {
            ctx.state            = GrIPState::RxHeader;
            ctx.errors.LastError = RET_OK;
        }
        // Any other byte is silently discarded
        break;
    }

        // ── RxHeader: accumulate and decode the fixed-size header ─────────────────
    case GrIPState::RxHeader:
    {
        const qint64 needed = static_cast<qint64>(GRIP_HEADER_SIZE) * 2;
        if (streamAvailable() < needed) break;

        uint8_t rawHex[GRIP_HEADER_SIZE * 2u] = {};
        streamRead(rawHex, needed);

        auto *dst = reinterpret_cast<uint8_t *>(&ctx.rxPacket.RX_Header);
        for (uint16_t i = 0; i < GRIP_HEADER_SIZE; ++i)
            dst[i] = static_cast<uint8_t>(hexStringToUint(
                reinterpret_cast<const char *>(&rawHex[i * 2u]), 2u));

        const uint8_t hdrResult = validateHeader(ctx.rxPacket.RX_Header);
        if (hdrResult != RET_OK)
        {
            qDebug() << "GrIP: bad header, error=" << hdrResult;
            ctx.errors.LastError = hdrResult;
            ctx.state            = GrIPState::Finish;
            break;
        }

        if (ctx.rxPacket.RX_Header.Length > GRIP_BUFFER_SIZE)
        {
            qWarning() << "GrIP: payload too large" << ctx.rxPacket.RX_Header.Length;
            ctx.errors.LastError = RET_WRONG_LEN;
            ++ctx.errors.Len_Error;
            ctx.state = GrIPState::Finish;
            break;
        }

        const bool isResponseOrSync =
            (ctx.rxPacket.RX_Header.MsgType == MSG_RESPONSE) ||
            (ctx.rxPacket.RX_Header.MsgType == MSG_SYNC)     ||
            (ctx.rxPacket.RX_Header.MsgType == MSG_ERROR);

        if (isResponseOrSync)
        {
            ctx.sendResponse = false;
            if (ctx.rxPacket.RX_Header.ReturnCode != RET_OK)
            {
                qDebug() << "GrIP: response code=" << ctx.rxPacket.RX_Header.ReturnCode;
                ctx.lastResponse = ctx.rxPacket.RX_Header.ReturnCode;
            }
            ctx.state = GrIPState::Finish;
        }
        else if (ctx.rxPacket.RX_Header.Length > 0u)
        {
            ctx.state = GrIPState::WaitSOT;
        }
        else
        {
            // Header-only packet (no payload)
            ctx.errors.LastError = RET_OK;
            dispatchPacket(ctx.rxPacket);
            ctx.state = GrIPState::Finish;
        }
        break;
    }

        // ── WaitSOT: consume the start-of-text marker ─────────────────────────────
    case GrIPState::WaitSOT:
    {
        if (streamAvailable() < 1) break;

        uint8_t magic = 0u;
        streamRead(&magic, 1);

        if (magic == GRIP_SOT)
        {
            ctx.state       = GrIPState::RxData;
            ctx.maxReadLeft = MAX_READ_PER_TICK;
            ctx.bytesRead   = 0u;
        }
        else
        {
            // Unexpected byte — resynchronise
            qDebug() << "GrIP: expected SOT, got" << Qt::hex << magic;
            ctx.state = GrIPState::Idle;
        }
        break;
    }

        // ── RxData: decode hex-encoded payload bytes ──────────────────────────────
    case GrIPState::RxData:
    {
        while (streamAvailable() > 1 && ctx.maxReadLeft > 0)
        {
            --ctx.maxReadLeft;

            uint8_t hexPair[2u] = {};
            streamRead(hexPair, 2);

            if (!std::isxdigit(hexPair[0]) || !std::isxdigit(hexPair[1]))
            {
                qDebug() << "GrIP: non-hex bytes in payload";
                ctx.errors.LastError = RET_WRONG_PARAM;
                ctx.sendResponse     = (ctx.rxPacket.RX_Header.MsgType != MSG_RESPONSE);
                ctx.state            = GrIPState::Finish;

                // If one of those bytes was actually EOT, skip the Finish EOT read
                if (hexPair[0] == GRIP_EOT || hexPair[1] == GRIP_EOT)
                    ctx.state = GrIPState::Idle;

                break;
            }

            if (ctx.bytesRead < GRIP_BUFFER_SIZE)
            {
                ctx.rxPacket.Data[ctx.bytesRead] =
                    static_cast<uint8_t>(hexStringToUint(reinterpret_cast<const char *>(hexPair), 2u));
            }
            ++ctx.bytesRead;

            const bool allReceived = (ctx.bytesRead >= ctx.rxPacket.RX_Header.Length) ||
                                     (ctx.bytesRead >= GRIP_BUFFER_SIZE);
            if (allReceived)
            {
                const uint8_t computedCRC =
                    CRC_CalculateCRC8(ctx.rxPacket.Data, ctx.rxPacket.RX_Header.Length);

                if (ctx.rxPacket.RX_Header.CRC_Data == computedCRC)
                {
                    ctx.errors.LastError = RET_OK;
                    dispatchPacket(ctx.rxPacket);

                    const bool needsAck =
                        (ctx.rxPacket.RX_Header.MsgType != MSG_DATA_NO_RESPONSE) &&
                        (ctx.rxPacket.RX_Header.MsgType != MSG_RESPONSE);
                    ctx.sendResponse = needsAck;
                }
                else
                {
                    qDebug() << "GrIP: CRC mismatch on payload";
                    ctx.errors.LastError = RET_WRONG_CRC;
                    ++ctx.errors.CRC_Error;
                }

                ctx.state = GrIPState::Finish;
                break;
            }
        }

        // Reset per-tick budget regardless of early exit
        ctx.maxReadLeft = MAX_READ_PER_TICK;
        break;
    }

        // ── Finish: send ACK/NAK if required, consume trailing EOT ───────────────
    case GrIPState::Finish:
    {
        if (ctx.sendResponse)
        {
            // Temporarily release the stream lock while transmitting
            // (GrIP_Transmit re-acquires it)
            lk.unlock();
            std::ignore = GrIP_Transmit(PROT_GrIP, MSG_RESPONSE,
                          static_cast<GrIP_ReturnType_e>(ctx.errors.LastError), nullptr);
            lk.relock();
            ctx.sendResponse = false;
        }

        if (streamAvailable() > 0)
        {
            uint8_t eot = 0u;
            streamRead(&eot, 1);
            if (eot != GRIP_EOT)
                qDebug() << "GrIP: expected EOT, got" << Qt::hex << eot;
        }

        ctx.state = GrIPState::Idle;
        break;
    }

    default:
        qWarning() << "GrIP: unexpected state, resetting";
        ctx.state = GrIPState::Idle;
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

bool GrIP_Receive(GrIP_Packet_t *out)
{
    Q_ASSERT(out);
    QMutexLocker lk(&ctx.queueMtx);

    if (ctx.packetQueue.empty()) return false;

    *out = ctx.packetQueue.front();
    ctx.packetQueue.pop();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

void GrIP_GetError(GrIP_ErrorFlags_t *out)
{
    if (out)
        std::memcpy(out, &ctx.errors, sizeof(GrIP_ErrorFlags_t));
}

// ─────────────────────────────────────────────────────────────────────────────

int GrIP_GetLastResponse()
{
    if (ctx.lastResponse != -1)
    {
        const int tmp = ctx.lastResponse;
        ctx.lastResponse = -1;
        return tmp;
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────

void GrIP_RxCallback(const QByteArray &data)
{
    QMutexLocker lk(&ctx.streamMtx);
    ctx.rxStream.append(data);
}

// ─────────────────────────────────────────────────────────────────────────────

QByteArray GrIP_GetTxData()
{
    QMutexLocker lk(&ctx.streamMtx);
    QByteArray out;
    out.swap(ctx.txStream);
    return out;
}
