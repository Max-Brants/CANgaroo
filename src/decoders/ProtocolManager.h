#pragma once

#include "IDecoder.h"
#include <QVector>
#include <memory>

struct ProtocolConfig {
    bool enableUds29Bit = true;
};

class Backend;

class ProtocolManager {
public:
    explicit ProtocolManager(Backend *backend = nullptr);
    ~ProtocolManager() = default;

    /**
     * @brief Processes a CAN frame through all registered decoders.
     * @return The status of the decoding process.
     */
    DecodeStatus processFrame(const BusMessage& frame, ProtocolMessage& outMsg);

    void reset();
    
    ProtocolConfig& config() { return m_config; }

private:
    std::shared_ptr<IDecoder> m_udsDecoder;
    std::shared_ptr<IDecoder> m_j1939Decoder;
    std::shared_ptr<IDecoder> m_canOpenDecoder;
    ProtocolConfig m_config;
    uint32_t m_msgCounter = 0;
};
