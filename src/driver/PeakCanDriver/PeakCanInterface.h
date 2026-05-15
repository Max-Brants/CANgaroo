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

#pragma once

#include "../BusInterface.h"

#include <QMutex>

// TPCANHandle is typedef'd as uint16_t in PCANBasic.h.
// Forward-declare it here so PCANBasic.h is only needed in the .cpp.
using TPCANHandle = uint16_t;

class PeakCanDriver;

class PeakCanInterface : public BusInterface {
public:
    PeakCanInterface(PeakCanDriver *driver, TPCANHandle channel, QString name);
    ~PeakCanInterface() override;

    QString getName() const override;
    void setName(QString name);

    QList<CanTiming> getAvailableBitrates() override;

    void applyConfig(const MeasurementInterface &mi) override;

    unsigned getBitrate() override;
    uint32_t getCapabilities() override;

    void open() override;
    bool isOpen() override;
    void close() override;

    void sendMessage(const BusMessage &msg) override;
    bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) override;

    bool updateStatistics() override;
    void resetStatistics() override;
    uint32_t getState() override;
    int getNumRxFrames() override;
    int getNumRxErrors() override;
    int getNumRxOverruns() override;
    int getNumTxFrames() override;
    int getNumTxErrors() override;
    int getNumTxDropped() override;

    TPCANHandle getChannel() const;

private:
    TPCANHandle _channel;
    void       *_rxEvent;   // HANDLE — stored as void* to avoid pulling in windows.h
    bool        _isOpen;
    QString     _name;
    unsigned    _bitrate;
    uint64_t    _channelOpenTime_us{0};

    struct {
        uint64_t rx_count;
        int      rx_errors;
        uint64_t rx_overruns;
        uint64_t tx_count;
        int      tx_errors;
        uint64_t tx_dropped;
    } _stats, _offset_stats;

    QMutex _txMutex;
    QList<BusMessage> _txMsgList;
};
