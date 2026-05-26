#include "CanOpenDecoder.h"

#include "core/Backend.h"
#include "core/DBC/CanOpenDb.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementSetup.h"

namespace {
uint64_t timestampUsec(const BusMessage &frame)
{
    return static_cast<uint64_t>(frame.getFloatTimestamp() * 1000000.0);
}

QString objectLabel(const CanOpenObjectEntry *entry, quint16 index, quint8 subIndex)
{
    if (entry && !entry->name.isEmpty()) {
        return QStringLiteral("%1 (%2)").arg(entry->name, CanOpenDb::formatObjectKey(index, subIndex));
    }
    return CanOpenDb::formatObjectKey(index, subIndex);
}

void initMessage(const BusMessage &frame, ProtocolMessage &outMsg)
{
    outMsg = {};
    outMsg.protocol = QStringLiteral("CANopen");
    outMsg.timestamp = timestampUsec(frame);
    outMsg.rawFrames = { frame };
    outMsg.payload = QByteArray(reinterpret_cast<const char*>(frame.getData()), frame.getLength());
    outMsg.id = frame.getRawId();
}
}

CanOpenDecoder::CanOpenDecoder(Backend *backend)
    : m_backend(backend)
{
}

DecodeStatus CanOpenDecoder::tryDecode(const BusMessage &frame, ProtocolMessage &outMsg)
{
    if (frame.busType() != BusType::CAN || frame.isExtended() || frame.isRTR()) {
        return DecodeStatus::Ignored;
    }

    if (m_backend) {
        bool hasAnyDb = false;
        MeasurementSetup &setup = m_backend->getSetup();
        for (MeasurementNetwork *network : setup.getNetworks()) {
            if (!network->_canOpenDbs.isEmpty()) {
                hasAnyDb = true;
                break;
            }
        }
        if (!hasAnyDb) {
            return DecodeStatus::Ignored;
        }
    }

    if (decodeNmt(frame, outMsg)
        || decodeSyncOrEmcy(frame, outMsg)
        || decodeTime(frame, outMsg)
        || decodeSdo(frame, outMsg)
        || decodeHeartbeat(frame, outMsg)
        || decodePdo(frame, outMsg)) {
        return DecodeStatus::Completed;
    }

    return DecodeStatus::Ignored;
}

void CanOpenDecoder::reset()
{
}

QList<pCanOpenDb> CanOpenDecoder::candidateDbs(quint8 nodeId) const
{
    QList<pCanOpenDb> exact;
    QList<pCanOpenDb> fallback;
    if (!m_backend) {
        return fallback;
    }

    MeasurementSetup &setup = m_backend->getSetup();
    for (MeasurementNetwork *network : setup.getNetworks()) {
        for (const pCanOpenDb &db : network->_canOpenDbs) {
            if (db->configuredNodeId() == nodeId) {
                exact.append(db);
            } else if (db->configuredNodeId() < 0) {
                fallback.append(db);
            }
        }
    }
    return !exact.isEmpty() ? exact : fallback;
}

pCanOpenDb CanOpenDecoder::resolveDbForObject(quint8 nodeId, quint16 index, quint8 subIndex) const
{
    pCanOpenDb firstMatch;
    for (const pCanOpenDb &db : candidateDbs(nodeId)) {
        if (db->findObject(index, subIndex)) {
            if (db->configuredNodeId() == nodeId) {
                return db;
            }
            if (firstMatch.isNull()) {
                firstMatch = db;
            }
        }
    }
    return firstMatch;
}

pCanOpenDb CanOpenDecoder::resolveDbForPdo(quint8 nodeId, quint32 cobId, bool transmit, quint8 pdoNumber, const CanOpenPdo **pdo) const
{
    pCanOpenDb firstMatch;
    const CanOpenPdo *firstPdo = nullptr;
    for (const pCanOpenDb &db : candidateDbs(nodeId)) {
        const QList<const CanOpenPdo*> matches = db->findMatchingPdos(cobId, nodeId, transmit);
        for (const CanOpenPdo *candidate : matches) {
            if (candidate->number != pdoNumber) {
                continue;
            }
            if (db->configuredNodeId() == nodeId) {
                if (pdo) {
                    *pdo = candidate;
                }
                return db;
            }
            if (firstMatch.isNull()) {
                firstMatch = db;
                firstPdo = candidate;
            }
        }
    }
    if (pdo) {
        *pdo = firstPdo;
    }
    return firstMatch;
}

QString CanOpenDecoder::nmtCommandName(quint8 cs)
{
    switch (cs) {
    case 0x01: return QStringLiteral("Start Remote Node");
    case 0x02: return QStringLiteral("Stop Remote Node");
    case 0x80: return QStringLiteral("Enter Pre-operational");
    case 0x81: return QStringLiteral("Reset Node");
    case 0x82: return QStringLiteral("Reset Communication");
    default: return QStringLiteral("Unknown NMT Command");
    }
}

QString CanOpenDecoder::nmtStateName(quint8 state)
{
    switch (state) {
    case 0x00: return QStringLiteral("Boot-up");
    case 0x04: return QStringLiteral("Stopped");
    case 0x05: return QStringLiteral("Operational");
    case 0x7F: return QStringLiteral("Pre-operational");
    default: return QStringLiteral("Unknown State");
    }
}

QString CanOpenDecoder::sdoCommandName(quint8 cmd, bool request)
{
    const quint8 ccsScs = (cmd >> 5) & 0x7;
    if (request) {
        switch (ccsScs) {
        case 1: return QStringLiteral("SDO Download Request");
        case 2: return QStringLiteral("SDO Upload Request");
        case 3: return QStringLiteral("SDO Segment Upload/Download");
        default: return QStringLiteral("SDO Request");
        }
    }

    if (cmd == 0x80) {
        return QStringLiteral("SDO Abort Response");
    }

    switch (ccsScs) {
    case 2: return QStringLiteral("SDO Upload Response");
    case 3: return QStringLiteral("SDO Download Response");
    default: return QStringLiteral("SDO Response");
    }
}

QString CanOpenDecoder::sdoAbortName(quint32 abortCode)
{
    switch (abortCode) {
    case 0x05030000u: return QStringLiteral("Toggle bit not alternated");
    case 0x05040000u: return QStringLiteral("SDO protocol timed out");
    case 0x06010000u: return QStringLiteral("Unsupported access to an object");
    case 0x06010001u: return QStringLiteral("Attempt to read a write only object");
    case 0x06010002u: return QStringLiteral("Attempt to write a read only object");
    case 0x06020000u: return QStringLiteral("Object does not exist");
    case 0x06040041u: return QStringLiteral("Object cannot be mapped to the PDO");
    case 0x06040043u: return QStringLiteral("General parameter incompatibility");
    case 0x06070010u: return QStringLiteral("Data type does not match");
    case 0x06090011u: return QStringLiteral("Sub-index does not exist");
    case 0x08000000u: return QStringLiteral("General error");
    default:
        return QStringLiteral("Abort 0x%1").arg(abortCode, 8, 16, QChar('0')).toUpper();
    }
}

quint64 CanOpenDecoder::extractBitsLittleEndian(const uint8_t *data, int bitOffset, int bitLength)
{
    quint64 value = 0;
    for (int bit = 0; bit < bitLength; ++bit) {
        const int absolute = bitOffset + bit;
        const int byteIndex = absolute / 8;
        const int bitIndex = absolute % 8;
        if (data[byteIndex] & (1u << bitIndex)) {
            value |= (1ULL << bit);
        }
    }
    return value;
}

qint64 CanOpenDecoder::signExtend(quint64 value, int bitLength)
{
    if (bitLength <= 0 || bitLength >= 64) {
        return static_cast<qint64>(value);
    }
    const quint64 signBit = 1ULL << (bitLength - 1);
    if ((value & signBit) == 0) {
        return static_cast<qint64>(value);
    }
    const quint64 mask = ~((1ULL << bitLength) - 1ULL);
    return static_cast<qint64>(value | mask);
}

bool CanOpenDecoder::decodeNmt(const BusMessage &frame, ProtocolMessage &outMsg) const
{
    if (frame.getId() != 0x000 || frame.getLength() < 2) {
        return false;
    }

    initMessage(frame, outMsg);
    const quint8 command = frame.getByte(0);
    const quint8 nodeId = frame.getByte(1);
    outMsg.name = QStringLiteral("NMT - %1").arg(nmtCommandName(command));
    outMsg.description = nodeId == 0 ? QStringLiteral("Broadcast") : QStringLiteral("Node %1").arg(nodeId);
    outMsg.type = MessageType::Request;
    outMsg.metadata.insert(QStringLiteral("Node ID"), nodeId == 0 ? QStringLiteral("Broadcast") : QString::number(nodeId));
    outMsg.metadata.insert(QStringLiteral("Command"), nmtCommandName(command));
    return true;
}

bool CanOpenDecoder::decodeSyncOrEmcy(const BusMessage &frame, ProtocolMessage &outMsg) const
{
    const quint32 cobId = frame.getId();
    if (cobId == 0x080) {
        initMessage(frame, outMsg);
        outMsg.name = QStringLiteral("SYNC");
        outMsg.description = frame.getLength() == 0 ? QStringLiteral("Synchronization object") : QStringLiteral("SYNC / vendor specific payload");
        return true;
    }
    if (cobId <= 0x080 || cobId > 0x0FF) {
        return false;
    }

    initMessage(frame, outMsg);
    const quint8 nodeId = static_cast<quint8>(cobId - 0x080);
    const quint16 errorCode = frame.getLength() >= 2 ? static_cast<quint16>(frame.getByte(0) | (frame.getByte(1) << 8)) : 0;
    const quint8 errorRegister = frame.getLength() >= 3 ? frame.getByte(2) : 0;
    outMsg.name = QStringLiteral("EMCY");
    outMsg.description = QStringLiteral("Emergency from node %1").arg(nodeId);
    outMsg.type = MessageType::NegativeResponse;
    outMsg.metadata.insert(QStringLiteral("Node ID"), nodeId);
    outMsg.metadata.insert(QStringLiteral("Error Code"), QStringLiteral("0x%1").arg(errorCode, 4, 16, QChar('0')).toUpper());
    outMsg.metadata.insert(QStringLiteral("Error Register"), QStringLiteral("0x%1").arg(errorRegister, 2, 16, QChar('0')).toUpper());
    outMsg.metadata.insert(QStringLiteral("Sender"), QStringLiteral("Node %1").arg(nodeId));
    return true;
}

bool CanOpenDecoder::decodeTime(const BusMessage &frame, ProtocolMessage &outMsg) const
{
    if (frame.getId() != 0x100) {
        return false;
    }

    initMessage(frame, outMsg);
    outMsg.name = QStringLiteral("TIME");
    if (frame.getLength() >= 6) {
        const quint32 ms = static_cast<quint32>(frame.getByte(0)
            | (frame.getByte(1) << 8)
            | (frame.getByte(2) << 16)
            | (frame.getByte(3) << 24));
        const quint16 days = static_cast<quint16>(frame.getByte(4) | (frame.getByte(5) << 8));
        outMsg.description = QStringLiteral("%1 ms, %2 days").arg(ms).arg(days);
        outMsg.metadata.insert(QStringLiteral("Milliseconds"), QString::number(ms));
        outMsg.metadata.insert(QStringLiteral("Days"), QString::number(days));
    } else {
        outMsg.description = QStringLiteral("TIME object");
    }
    return true;
}

bool CanOpenDecoder::decodeSdo(const BusMessage &frame, ProtocolMessage &outMsg) const
{
    const quint32 cobId = frame.getId();
    const bool request = (cobId >= 0x600 && cobId <= 0x67F);
    if ((!request && !(cobId >= 0x580 && cobId <= 0x5FF)) || frame.getLength() < 4) {
        return false;
    }

    const quint8 nodeId = static_cast<quint8>(cobId - (request ? 0x600 : 0x580));
    const quint8 cmd = frame.getByte(0);
    const quint16 index = static_cast<quint16>(frame.getByte(1) | (frame.getByte(2) << 8));
    const quint8 subIndex = frame.getByte(3);
    const pCanOpenDb db = resolveDbForObject(nodeId, index, subIndex);
    const CanOpenObjectEntry *entry = db ? db->findObject(index, subIndex) : nullptr;

    initMessage(frame, outMsg);
    outMsg.name = sdoCommandName(cmd, request);
    outMsg.description = objectLabel(entry, index, subIndex);
    outMsg.type = request ? MessageType::Request : MessageType::PositiveResponse;
    outMsg.metadata.insert(QStringLiteral("Node ID"), nodeId);
    outMsg.metadata.insert(QStringLiteral("Object"), CanOpenDb::formatObjectKey(index, subIndex));
    if (entry && !entry->name.isEmpty()) {
        outMsg.metadata.insert(QStringLiteral("Object Name"), entry->name);
    }
    outMsg.metadata.insert(QStringLiteral("Sender"), QStringLiteral("Node %1").arg(nodeId));
    if (db && !db->deviceName().isEmpty()) {
        outMsg.metadata.insert(QStringLiteral("Device"), db->deviceName());
    }

    if (!request && cmd == 0x80 && frame.getLength() >= 8) {
        const quint32 abortCode = static_cast<quint32>(frame.getByte(4)
            | (frame.getByte(5) << 8)
            | (frame.getByte(6) << 16)
            | (frame.getByte(7) << 24));
        outMsg.name = QStringLiteral("SDO Abort Response");
        outMsg.description = sdoAbortName(abortCode);
        outMsg.type = MessageType::NegativeResponse;
        outMsg.metadata.insert(QStringLiteral("Abort Code"), QStringLiteral("0x%1").arg(abortCode, 8, 16, QChar('0')).toUpper());
        return true;
    }

    const bool expedited = (cmd & 0x02u) != 0;
    const bool sizeIndicated = (cmd & 0x01u) != 0;
    if (expedited && frame.getLength() >= 8) {
        int dataSize = 4;
        if (sizeIndicated) {
            dataSize = 4 - ((cmd >> 2) & 0x03u);
        }
        quint64 rawValue = 0;
        for (int i = 0; i < dataSize; ++i) {
            rawValue |= static_cast<quint64>(frame.getByte(4 + i)) << (8 * i);
        }
        const QString valueText = entry && entry->isSigned()
            ? QString::number(signExtend(rawValue, qMax(1, dataSize * 8)))
            : QString::number(rawValue);
        outMsg.metadata.insert(QStringLiteral("Value"), valueText);
    }
    return true;
}

bool CanOpenDecoder::decodeHeartbeat(const BusMessage &frame, ProtocolMessage &outMsg) const
{
    const quint32 cobId = frame.getId();
    if (cobId < 0x700 || cobId > 0x77F || frame.getLength() < 1) {
        return false;
    }

    const quint8 nodeId = static_cast<quint8>(cobId - 0x700);
    initMessage(frame, outMsg);
    const quint8 state = frame.getByte(0);
    outMsg.name = state == 0x00 ? QStringLiteral("Boot-up") : QStringLiteral("Heartbeat");
    outMsg.description = QStringLiteral("%1 from node %2").arg(nmtStateName(state)).arg(nodeId);
    outMsg.metadata.insert(QStringLiteral("Node ID"), nodeId);
    outMsg.metadata.insert(QStringLiteral("State"), nmtStateName(state));
    outMsg.metadata.insert(QStringLiteral("Sender"), QStringLiteral("Node %1").arg(nodeId));
    return true;
}

bool CanOpenDecoder::decodePdo(const BusMessage &frame, ProtocolMessage &outMsg) const
{
    const quint32 cobId = frame.getId();
    bool transmit = false;
    quint8 pdoNumber = 0;
    quint8 nodeId = 0;

    if (cobId >= 0x180 && cobId <= 0x1FF) {
        transmit = true; pdoNumber = 1; nodeId = static_cast<quint8>(cobId - 0x180);
    } else if (cobId >= 0x200 && cobId <= 0x27F) {
        transmit = false; pdoNumber = 1; nodeId = static_cast<quint8>(cobId - 0x200);
    } else if (cobId >= 0x280 && cobId <= 0x2FF) {
        transmit = true; pdoNumber = 2; nodeId = static_cast<quint8>(cobId - 0x280);
    } else if (cobId >= 0x300 && cobId <= 0x37F) {
        transmit = false; pdoNumber = 2; nodeId = static_cast<quint8>(cobId - 0x300);
    } else if (cobId >= 0x380 && cobId <= 0x3FF) {
        transmit = true; pdoNumber = 3; nodeId = static_cast<quint8>(cobId - 0x380);
    } else if (cobId >= 0x400 && cobId <= 0x47F) {
        transmit = false; pdoNumber = 3; nodeId = static_cast<quint8>(cobId - 0x400);
    } else if (cobId >= 0x480 && cobId <= 0x4FF) {
        transmit = true; pdoNumber = 4; nodeId = static_cast<quint8>(cobId - 0x480);
    } else if (cobId >= 0x500 && cobId <= 0x57F) {
        transmit = false; pdoNumber = 4; nodeId = static_cast<quint8>(cobId - 0x500);
    } else {
        return false;
    }

    const CanOpenPdo *pdo = nullptr;
    const pCanOpenDb db = resolveDbForPdo(nodeId, cobId, transmit, pdoNumber, &pdo);

    initMessage(frame, outMsg);
    outMsg.name = QStringLiteral("%1PDO%2").arg(transmit ? QStringLiteral("T") : QStringLiteral("R")).arg(pdoNumber);
    outMsg.description = QStringLiteral("Node %1").arg(nodeId);
    outMsg.metadata.insert(QStringLiteral("Node ID"), nodeId);
    outMsg.metadata.insert(QStringLiteral("Sender"), QStringLiteral("Node %1").arg(nodeId));
    if (db && !db->deviceName().isEmpty()) {
        outMsg.metadata.insert(QStringLiteral("Device"), db->deviceName());
        outMsg.description = QStringLiteral("%1 - %2").arg(db->deviceName()).arg(outMsg.description);
    }

    if (!pdo) {
        return true;
    }

    int bitOffset = 0;
    for (const CanOpenPdoMapping &mapping : pdo->mappings) {
        if (mapping.bitLength <= 0 || bitOffset + mapping.bitLength > frame.getLength() * 8) {
            break;
        }
        const quint64 raw = extractBitsLittleEndian(frame.getData(), bitOffset, mapping.bitLength);
        const QString key = mapping.objectName.isEmpty()
            ? CanOpenDb::formatObjectKey(mapping.objectIndex, mapping.objectSubIndex)
            : QStringLiteral("%1 (%2)").arg(mapping.objectName, CanOpenDb::formatObjectKey(mapping.objectIndex, mapping.objectSubIndex));
        const QString valueText = mapping.isSigned
            ? QString::number(signExtend(raw, mapping.bitLength))
            : QString::number(raw);
        outMsg.metadata.insert(key, valueText);
        bitOffset += mapping.bitLength;
    }
    return true;
}
