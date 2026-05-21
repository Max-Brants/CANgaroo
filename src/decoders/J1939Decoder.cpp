#include "J1939Decoder.h"

#include <QtGlobal> // qMin

J1939Decoder::J1939Decoder() {}

static QString pgnName(uint32_t pgn)
{
    switch (pgn) {
        case 0xFEEC: return "Vehicle Identification (VIN)";
        case 0xF004: return "Electronic Engine Controller 1 (EEC1)";
        case 0xFEEF: return "Engine Fluid Level/Pressure";
        case 0xFEE5: return "Engine Hours/Revolutions";
        case 0xFEF1: return "Cruise Control/Vehicle Speed";
        case 0xF000: return "Torque/Speed Control";
        case 0xFECA: return "Active Diagnostic Trouble Codes";
        case 0xFECB: return "Previously Active DTCs";
        case 0xFEDA: return "Software Identification";
        case 0xFEDB: return "ECU Identification Information";
        default:     return QString("PGN: 0x%1").arg(pgn, 4, 16, QChar('0'));
    }
}

DecodeStatus J1939Decoder::tryDecode(const BusMessage& frame, ProtocolMessage& outMsg) {
    if (frame.isErrorFrame() || frame.isRTR() || !frame.isExtended()) {
        return DecodeStatus::Ignored;
    }

    uint32_t id  = frame.getId();
    uint32_t pgn = extractPgn(id);
    uint8_t  sa  = static_cast<uint8_t>(id & 0xFF);
    uint8_t  da  = static_cast<uint8_t>((id >> 8) & 0xFF); // PS field = destination for PDU1 PGNs

    if (pgn == 0x00EC00) { // TP.CM
        if (frame.getLength() < 8) { return DecodeStatus::Ignored; }
        uint8_t controlByte = frame.getByte(0);

        if (controlByte == 32 || controlByte == 16) { // BAM (32) or RTS (16)
            uint16_t size        = static_cast<uint16_t>(frame.getByte(1) | (frame.getByte(2) << 8));
            uint8_t  totalPkts   = frame.getByte(3);
            uint32_t targetPgn   = static_cast<uint32_t>(frame.getByte(5))
                                 | (static_cast<uint32_t>(frame.getByte(6)) << 8)
                                 | (static_cast<uint32_t>(frame.getByte(7)) << 16);

            // Key encodes both SA and DA to handle concurrent sessions from the same SA
            uint32_t key = tpSessionKey(sa, da);
            J1939Session& session  = m_sessions[key];
            session.pgn            = targetPgn;
            session.expectedSize   = size;
            session.expectedPackets = totalPkts;
            session.receivedPackets = 0;
            session.data           = QByteArray();
            session.frames         = {frame};
            session.sa             = sa;
            session.da             = da;
            return DecodeStatus::Consumed;
        }

        // CTS (17), End of Message Ack (19), Connection Abort (255)
        uint32_t key = tpSessionKey(sa, da);
        if (controlByte == 255 && m_sessions.contains(key)) { // Abort
            m_sessions.remove(key);
        }
        return m_sessions.contains(key) ? DecodeStatus::Consumed : DecodeStatus::Ignored;

    } else if (pgn == 0x00EB00) { // TP.DT
        uint32_t key = tpSessionKey(sa, da);
        if (m_sessions.contains(key)) {
            J1939Session& session = m_sessions[key];

            // Validate sequence number (J1939-21: DT byte 0 = packet number, 1-based)
            uint8_t seqNum = frame.getByte(0);
            if (seqNum != static_cast<uint8_t>(session.receivedPackets + 1)) {
                m_sessions.remove(key);
                return DecodeStatus::Ignored;
            }

            session.receivedPackets++;
            session.frames.append(frame);

            // Guard against malformed DT frames with DLC < 8
            const int limit = qMin(8, static_cast<int>(frame.getLength()));
            for (int i = 1; i < limit && session.data.size() < session.expectedSize; ++i)
                session.data.append(frame.getByte(i));

            if (session.receivedPackets >= session.expectedPackets ||
                session.data.size()     >= session.expectedSize) {

                outMsg.payload   = session.data;
                outMsg.rawFrames = session.frames;
                outMsg.protocol  = "J1939";
                outMsg.timestamp = static_cast<uint64_t>(session.frames.first().getFloatTimestamp() * 1000000.0);
                outMsg.id        = session.pgn;
                outMsg.type      = MessageType::Request;
                outMsg.name      = pgnName(session.pgn);

                // Metadata from the TP.CM header frame
                uint32_t cmId = session.frames.first().getId();
                outMsg.metadata["Priority"]     = (cmId >> 26) & 0x7;
                outMsg.metadata["Reserved"]     = (cmId >> 25) & 1;
                outMsg.metadata["Data Page"]    = (cmId >> 24) & 1;
                outMsg.metadata["PDU Format"]   = (cmId >> 16) & 0xFF;
                outMsg.metadata["PDU Specific"] = (cmId >>  8) & 0xFF;
                outMsg.metadata["Source Address"] = cmId & 0xFF;

                m_sessions.remove(key);
                return DecodeStatus::Completed;
            }
            return DecodeStatus::Consumed;
        }
    } else {
        // Single-packet PGN
        uint8_t pf     = static_cast<uint8_t>((id >> 16) & 0xFF);
        uint8_t ps     = static_cast<uint8_t>((id >>  8) & 0xFF);
        bool    isPDU1 = pf < 240;

        outMsg.payload = QByteArray();
        for (int i = 0; i < frame.getLength(); ++i)
            outMsg.payload.append(frame.getByte(i));

        outMsg.rawFrames = {frame};
        outMsg.protocol  = "J1939";
        outMsg.timestamp = static_cast<uint64_t>(frame.getFloatTimestamp() * 1000000.0);
        outMsg.id        = pgn;
        outMsg.type      = MessageType::Request;
        outMsg.name      = pgnName(pgn);
        outMsg.description = isPDU1
            ? QString("PDU1 (Peer-to-Peer) from SA [0x%1] to DA [0x%2]")
                  .arg(sa, 2, 16, QChar('0'))
                  .arg(ps, 2, 16, QChar('0'))
            : QString("PDU2 (Broadcast) from SA [0x%1]")
                  .arg(sa, 2, 16, QChar('0'));

        outMsg.metadata["Priority"]     = (id >> 26) & 0x7;
        outMsg.metadata["Reserved"]     = (id >> 25) & 1;
        outMsg.metadata["Data Page"]    = (id >> 24) & 1;
        outMsg.metadata["PDU Format"]   = pf;
        outMsg.metadata["PDU Specific"] = ps;
        outMsg.metadata["Source Address"] = sa;

        return DecodeStatus::Completed;
    }

    return DecodeStatus::Ignored;
}

void J1939Decoder::reset() {
    m_sessions.clear();
}

uint32_t J1939Decoder::tpSessionKey(uint8_t sa, uint8_t da) noexcept
{
    return (static_cast<uint32_t>(sa) << 8) | da;
}

uint32_t J1939Decoder::extractPgn(uint32_t id) {
    uint8_t pf  = static_cast<uint8_t>((id >> 16) & 0xFF);
    uint8_t ps  = static_cast<uint8_t>((id >>  8) & 0xFF);
    uint8_t dp  = static_cast<uint8_t>((id >> 24) & 1);
    uint8_t edp = static_cast<uint8_t>((id >> 25) & 1);

    uint32_t pgn = (static_cast<uint32_t>(edp) << 17)
                 | (static_cast<uint32_t>(dp)  << 16)
                 | (static_cast<uint32_t>(pf)  <<  8);
    if (pf >= 240) {
        pgn |= ps;
    }
    return pgn;
}
