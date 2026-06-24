/*

  Copyright (c) 2026 Max Brants

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

// VCIID is a union of LUID/INT64 in vcitype.h. We only ever need it as an
// opaque 64-bit identity here, so store it as int64_t and avoid pulling the
// IXXAT VCI SDK headers into every translation unit that includes this file;
// vcinpl2.h is included only in the .cpp.
using IxxatDeviceId = int64_t;

class IxxatDriver;

class IxxatInterface : public BusInterface {
public:
    IxxatInterface(IxxatDriver *driver, IxxatDeviceId deviceId, uint32_t canNo, QString name);
    ~IxxatInterface() override;

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

    IxxatDeviceId getDeviceId() const;
    uint32_t getCanNo() const;

private:
    IxxatDeviceId _deviceId;
    uint32_t      _canNo;

    void *_hDevice;
    void *_hCanCtl;
    void *_hCanChn;

    bool        _isOpen;
    QString     _name;
    unsigned    _bitrate;
    uint64_t    _channelOpenTime_us{0};

    // timestamp counter clock frequency / divisor, read from the controller's
    // capabilities at open() time, used to convert CANMSG2.dwTime ticks to µs.
    uint32_t _tscClkFreq{1000000};
    uint32_t _tscDivisor{1};

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
