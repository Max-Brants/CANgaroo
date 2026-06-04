/*

  Copyright (c) 2026 Patrick Felixberger

  This file is part of CANgaroo.

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

#include <variant>
#include <QString>
#include "core/BusMessage.h"

class CanDbSignal;
class LinSignal;
class LinFrame;

// Unified signal reference for graphing — wraps a CAN signal, LIN signal, or virtual bus-load signal.
class GraphSignal
{
public:
    explicit GraphSignal(CanDbSignal *signal);
    GraphSignal(LinSignal *signal, LinFrame *frame);
    GraphSignal(uint16_t interfaceId, unsigned bitrate, const QString &label);

    QString name() const;
    QString unit() const;
    double minValue() const;
    double maxValue() const;
    QString parentName() const;
    QString comment() const;
    bool isLin() const noexcept;
    bool isBusLoad() const noexcept;

    uint16_t busLoadInterfaceId() const noexcept;
    unsigned busLoadBitrate() const noexcept;

    bool isPresentInMessage(const BusMessage &msg) const;
    double extractPhysicalFromMessage(const BusMessage &msg) const;

    CanDbSignal *asCanSignal() const noexcept;
    LinSignal   *asLinSignal() const noexcept;

private:
    struct CanData     { CanDbSignal *signal; };
    struct LinData     { LinSignal *signal; LinFrame *frame; };
    struct BusLoadData { uint16_t interfaceId; unsigned bitrate; QString label; };
    std::variant<CanData, LinData, BusLoadData> _data;
};
