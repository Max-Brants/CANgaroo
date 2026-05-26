#pragma once

#include "IDecoder.h"
#include "core/DBC/CanOpenDb.h"

class Backend;

class CanOpenDecoder : public IDecoder
{
public:
    explicit CanOpenDecoder(Backend *backend = nullptr);
    ~CanOpenDecoder() override = default;

    DecodeStatus tryDecode(const BusMessage& frame, ProtocolMessage& outMsg) override;
    void reset() override;

private:
    Backend *m_backend = nullptr;

    QList<pCanOpenDb> candidateDbs(quint8 nodeId) const;
    pCanOpenDb resolveDbForObject(quint8 nodeId, quint16 index, quint8 subIndex) const;
    pCanOpenDb resolveDbForPdo(quint8 nodeId, quint32 cobId, bool transmit, quint8 pdoNumber, const CanOpenPdo **pdo) const;

    static QString nmtCommandName(quint8 cs);
    static QString nmtStateName(quint8 state);
    static QString sdoCommandName(quint8 cmd, bool request);
    static QString sdoAbortName(quint32 abortCode);
    static quint64 extractBitsLittleEndian(const uint8_t *data, int bitOffset, int bitLength);
    static qint64 signExtend(quint64 value, int bitLength);

    bool decodeNmt(const BusMessage &frame, ProtocolMessage &outMsg) const;
    bool decodeSyncOrEmcy(const BusMessage &frame, ProtocolMessage &outMsg) const;
    bool decodeTime(const BusMessage &frame, ProtocolMessage &outMsg) const;
    bool decodeSdo(const BusMessage &frame, ProtocolMessage &outMsg) const;
    bool decodeHeartbeat(const BusMessage &frame, ProtocolMessage &outMsg) const;
    bool decodePdo(const BusMessage &frame, ProtocolMessage &outMsg) const;
};
