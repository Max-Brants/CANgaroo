#ifndef CANDLEAPIINTERFACE_H
#define CANDLEAPIINTERFACE_H

#include "driver/BusInterface.h"
#include "core/MeasurementInterface.h"

#include <QList>
#include <QMutex>

#include <windows.h>

#include <string>

#include "CandleApiTiming.h"
#include "api/candle.h"

class CandleApiDriver;

class CandleApiInterface : public BusInterface
{
    Q_OBJECT
public:
    CandleApiInterface(CandleApiDriver *driver, candle_handle handle, uint8_t channel);
    ~CandleApiInterface() override;

    QString getName() const override;
    QString getDetailsStr() const override;

    void applyConfig(const MeasurementInterface &mi) override;

    unsigned getBitrate() override;

    uint32_t getCapabilities() override;
    QList<CanTiming> getAvailableBitrates() override;

    void open() override;
    bool isOpen() override;
    void close() override;

    void sendMessage(const BusMessage &msg) override;
    bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) override;

    bool updateStatistics() override;
    uint32_t getState() override;
    int getNumRxFrames() override;
    int getNumRxErrors() override;
    int getNumTxFrames() override;
    int getNumTxErrors() override;
    int getNumRxOverruns() override;
    int getNumTxDropped() override;

    QString getVersion();
    std::wstring getPath() const;
    uint8_t getChannel() const;

    bool matches(candle_handle dev, uint8_t channel) const;
private:

    uint64_t _hostOffsetStart;
    uint32_t _deviceTicksStart;
    uint8_t _channel;
    bool _isOpen;
    bool _isFdEnabled;

    candle_handle _handle;
    MeasurementInterface _settings;
    Backend &_backend;

    uint64_t _numRx;
    uint64_t _numTx;
    uint64_t _numTxErr;

    QList<CandleApiTiming> _timings;
    QList<CandleApiTiming> _fdTimings;

    bool setBitTiming(uint32_t bitrate, uint32_t samplePoint);
    bool setDataBitTiming(uint32_t bitrate, uint32_t samplePoint);

    QMutex _txMutex;
    QList<BusMessage> _txMsgList;
};

#endif // CANDLEAPIINTERFACE_H

