#include "ProtocolManager.h"
#include "UdsDecoder.h"
#include "J1939Decoder.h"

ProtocolManager::ProtocolManager() {
    m_udsDecoder = std::make_shared<UdsDecoder>();
    m_j1939Decoder = std::make_shared<J1939Decoder>();
}

DecodeStatus ProtocolManager::processFrame(const BusMessage& frame, ProtocolMessage& outMsg) {
    DecodeStatus status = DecodeStatus::Ignored;

    // LIN diagnostic frames (ISO 17987): 0x3C = master request, 0x3D = slave response
    // On-bus layout: [NAD, PCI, SID, data...] = 8 bytes. Strip NAD and pass the rest
    // (PCI + payload) to the UDS decoder as an ISO 15765-2 single-frame transport PDU.
    // The firmware adds PCI automatically based on payload length; callers supply [SID, data...].
    if (frame.busType() == BusType::LIN)
    {
        const uint32_t linId = frame.getId();
        if ((linId == 0x3C || linId == 0x3D) && frame.getLength() >= 3)
        {
            BusMessage synthetic;
            synthetic.setInterfaceId(frame.getInterfaceId());
            synthetic.setTimestamp_us(frame.getTimestamp_us());
            synthetic.setRX(frame.isRX());
            const uint8_t synLen = static_cast<uint8_t>(frame.getLength() - 1u);
            synthetic.setLength(synLen);
            for (uint8_t i = 0; i < synLen; ++i)
                synthetic.setByte(i, frame.getByte(static_cast<uint8_t>(i + 1u)));

            status = m_udsDecoder->tryDecode(synthetic, outMsg);
            if (status == DecodeStatus::Completed)
            {
                outMsg.rawFrames = {frame};
                outMsg.timestamp = static_cast<uint64_t>(frame.getFloatTimestamp() * 1000000.0);
                outMsg.globalIndex = ++m_msgCounter;
            }
        }
        return status;
    }

    if (frame.isExtended()) {
        // 29-bit ID: Try J1939 first
        status = m_j1939Decoder->tryDecode(frame, outMsg);
        
        // Only try UDS if J1939 ignored it and 29-bit UDS is explicitly enabled
        if (status == DecodeStatus::Ignored && m_config.enableUds29Bit) {
            status = m_udsDecoder->tryDecode(frame, outMsg);
        }
    } else {
        // 11-bit ID: Try UDS
        status = m_udsDecoder->tryDecode(frame, outMsg);
    }

    if (status == DecodeStatus::Completed) {
        outMsg.globalIndex = ++m_msgCounter;
    }

    return status;
}

void ProtocolManager::reset() {
    m_msgCounter = 0;
    m_udsDecoder->reset();
    m_j1939Decoder->reset();
}
