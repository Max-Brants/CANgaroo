/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>

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

#include <atomic>
#include <cstdint>

#include <QObject>
#include <QString>

#include "CanDriver.h"
#include "CanTiming.h"
#include "core/BusMessage.h"

class MeasurementInterface;

class BusInterface: public QObject  {
    Q_OBJECT
public:
    enum {
        state_ok,
        state_warning,
        state_passive,
        state_bus_off,
        state_stopped,
        state_unknown,
        state_tx_success,
        state_tx_fail,
    };

    enum {
        // CAN capabilities
        capability_canfd                = 0x001,
        capability_listen_only          = 0x002,
        capability_triple_sampling      = 0x004,
        capability_one_shot             = 0x008,
        capability_auto_restart         = 0x010,
        capability_config_os            = 0x020,
        capability_custom_bitrate       = 0x040,
        capability_custom_canfd_bitrate = 0x080,
        // LIN capabilities
        capability_lin_master           = 0x100,
        capability_lin_slave            = 0x200,
        capability_lin_monitor          = 0x400,
    };

public:
    BusInterface(CanDriver *driver);
    virtual ~BusInterface();
    virtual CanDriver *getDriver();
    virtual QString getName() const = 0;
    virtual QString getDetailsStr() const;
    virtual BusType busType() const { return BusType::CAN; }

    virtual void applyConfig(const MeasurementInterface &mi) = 0;

    virtual unsigned getBitrate() = 0;

    virtual uint32_t getCapabilities();
    virtual QList<CanTiming> getAvailableBitrates();

    virtual void open();
    virtual void close();

    virtual bool isOpen();

    virtual void sendLinSleepWakeup(bool wakeup) { Q_UNUSED(wakeup) }
    virtual void sendMessage(const BusMessage &msg) = 0;
    virtual bool readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms) = 0;

    virtual bool updateStatistics();
    virtual void resetStatistics() { _totalBits = 0; }
    virtual uint32_t getState() = 0;
    virtual int getNumRxFrames() = 0;
    virtual int getNumRxErrors() = 0;
    virtual int getNumTxFrames() = 0;
    virtual int getNumTxErrors() = 0;
    virtual int getNumRxOverruns() = 0;
    virtual int getNumTxDropped() = 0;
    virtual uint64_t getNumBits();
    void addFrameBits(const BusMessage &msg);

    virtual QString getVersion();

    QString getStateText();

    BusInterfaceId getId() const;
    void setId(BusInterfaceId id);

private:
    BusInterfaceId _id;
    CanDriver *_driver;
    std::atomic<uint64_t> _totalBits;
};
