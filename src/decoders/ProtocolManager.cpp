#include "ProtocolManager.h"
#include "UdsDecoder.h"
#include "J1939Decoder.h"
#include "CanOpenDecoder.h"

ProtocolManager::ProtocolManager(Backend *backend) {
    m_udsDecoder = std::make_shared<UdsDecoder>();
    m_j1939Decoder = std::make_shared<J1939Decoder>();
    m_canOpenDecoder = std::make_shared<CanOpenDecoder>(backend);
}

DecodeStatus ProtocolManager::processFrame(const BusMessage& frame, ProtocolMessage& outMsg) {
    DecodeStatus status = DecodeStatus::Ignored;

    if (frame.isExtended()) {
        status = m_j1939Decoder->tryDecode(frame, outMsg);

        if (status == DecodeStatus::Ignored && m_config.enableUds29Bit) {
            status = m_udsDecoder->tryDecode(frame, outMsg);
        }
    } else {
        status = m_canOpenDecoder->tryDecode(frame, outMsg);
        if (status == DecodeStatus::Ignored) {
            status = m_udsDecoder->tryDecode(frame, outMsg);
        }
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
    m_canOpenDecoder->reset();
}
