/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>
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

#include "BusInterface.h"
#include "core/BusMessage.h"

#include <QList>

BusInterface::BusInterface(CanDriver *driver)
  :QObject(0), _id(-1), _driver(driver), _totalBits(0)
{
}

BusInterface::~BusInterface() {
}

CanDriver* BusInterface::getDriver() {
    return _driver;
}

QString BusInterface::getDetailsStr() const
{
    return "";
}

uint32_t BusInterface::getCapabilities()
{
    return 0;
}

QList<CanTiming> BusInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;
    retval << CanTiming(0,   10000, 0, 875) \
           << CanTiming(1,   20000, 0, 875) \
           << CanTiming(2,   50000, 0, 875) \
           << CanTiming(3,   83333, 0, 875) \
           << CanTiming(4,  100000, 0, 875) \
           << CanTiming(5,  125000, 0, 875) \
           << CanTiming(6,  250000, 0, 875) \
           << CanTiming(7,  500000, 0, 875) \
           << CanTiming(8,  800000, 0, 875) \
           << CanTiming(9, 1000000, 0, 875);
    return retval;
}

void BusInterface::open() {
}

void BusInterface::close() {
}

bool BusInterface::isOpen()
{
    return false;
}

bool BusInterface::updateStatistics()
{
    return false;
}

QString BusInterface::getStateText()
{
    switch (getState()) {
    case state_ok: return tr("ready");
    case state_warning: return tr("warning");
    case state_passive: return tr("error passive");
    case state_bus_off: return tr("bus off");
    case state_stopped: return tr("stopped");
    case state_unknown: return tr("unknown");
    case state_tx_success: return tr("tx success");
    case state_tx_fail: return tr("tx fail");
        default: return "";
    }
}

BusInterfaceId BusInterface::getId() const
{
    return _id;
}

void BusInterface::setId(BusInterfaceId id)
{
    _id = id;
}

uint64_t BusInterface::getNumBits()
{
    return _totalBits;
}

void BusInterface::addFrameBits(const BusMessage &msg)
{
    uint32_t bits;

    if (msg.busType() == BusType::LIN) {
        // Break(14) + Sync(10) + PID(10) + data bytes (10 each) + checksum(10)
        bits = 44 + static_cast<uint32_t>(msg.getLength()) * 10;
    } else {
        uint32_t dlc = msg.getLength();
        bits = 47 + (dlc * 8);
        if (msg.isExtended()) bits += 18 + 2;
        if (msg.isFD())       bits += 20;
        bits += bits / 5; // Approximate bit stuffing
    }

    _totalBits += bits;
}

QString BusInterface::getVersion()
{
    return "UnKnown";
}
