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

#include <atomic>

#include <QMutex>

class QCanBusDevice;
class ZsCanFdDriver;

class ZsCanFdInterface : public BusInterface {
public:
    ZsCanFdInterface(ZsCanFdDriver *driver, QString deviceName, QString description);
    ~ZsCanFdInterface() override;

    QString getName() const override;
    void setName(QString name);

    QList<CanTiming> getAvailableBitrates() override;

    void applyConfig(const MeasurementInterface &mi) override;

    bool supportsCanFD();

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

    QString getDeviceName() const;

private:
    QString           _deviceName;
    QString           _name;
    QCanBusDevice    *_device;
    unsigned          _bitrate;
    unsigned          _fdBitrate;
    bool              _listenOnly;
    bool              _isCanFD;
    std::atomic<bool> _isOpen{false};

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
