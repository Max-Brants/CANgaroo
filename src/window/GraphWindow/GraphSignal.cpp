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

#include "GraphSignal.h"
#include "core/DBC/CanDbSignal.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/LinSignal.h"
#include "core/DBC/LinFrame.h"

#include <span>
#include <QtGlobal>

GraphSignal::GraphSignal(CanDbSignal *signal)
    : _data(CanData{signal})
{
    Q_ASSERT(signal != nullptr);
}

GraphSignal::GraphSignal(LinSignal *signal, LinFrame *frame)
    : _data(LinData{signal, frame})
{
    Q_ASSERT(signal != nullptr);
    Q_ASSERT(frame != nullptr);
}

GraphSignal::GraphSignal(uint16_t interfaceId, unsigned bitrate, const QString &label)
    : _data(BusLoadData{interfaceId, bitrate, label})
{
}

QString GraphSignal::name() const
{
    if (isBusLoad())
        return std::get<BusLoadData>(_data).label;
    if (isLin())
        return std::get<LinData>(_data).signal->name();
    return std::get<CanData>(_data).signal->name();
}

QString GraphSignal::unit() const
{
    if (isBusLoad())
        return QStringLiteral("%");
    if (isLin())
        return std::get<LinData>(_data).signal->unit();
    return std::get<CanData>(_data).signal->getUnit();
}

double GraphSignal::minValue() const
{
    if (isBusLoad())
        return 0.0;
    if (isLin())
        return std::get<LinData>(_data).signal->minValue();
    return std::get<CanData>(_data).signal->getMinimumValue();
}

double GraphSignal::maxValue() const
{
    if (isBusLoad())
        return 100.0;
    if (isLin())
        return std::get<LinData>(_data).signal->maxValue();
    return std::get<CanData>(_data).signal->getMaximumValue();
}

QString GraphSignal::parentName() const
{
    if (isBusLoad())
        return QStringLiteral("Bus Load");
    if (isLin()) {
        return std::get<LinData>(_data).frame->name();
    }
    auto *msg = std::get<CanData>(_data).signal->getParentMessage();
    return msg ? msg->getName() : QString();
}

QString GraphSignal::comment() const
{
    if (isBusLoad() || isLin())
        return {};
    return std::get<CanData>(_data).signal->comment();
}

bool GraphSignal::isLin() const noexcept
{
    return std::holds_alternative<LinData>(_data);
}

bool GraphSignal::isBusLoad() const noexcept
{
    return std::holds_alternative<BusLoadData>(_data);
}

uint16_t GraphSignal::busLoadInterfaceId() const noexcept
{
    if (!isBusLoad()) return 0xFFFF;
    return std::get<BusLoadData>(_data).interfaceId;
}

unsigned GraphSignal::busLoadBitrate() const noexcept
{
    if (!isBusLoad()) return 0;
    return std::get<BusLoadData>(_data).bitrate;
}

bool GraphSignal::isPresentInMessage(const BusMessage &msg) const
{
    if (isBusLoad())
        return false; // handled separately in the decoder worker
    if (isLin()) {
        const auto &d = std::get<LinData>(_data);
        return msg.busType() == BusType::LIN
            && msg.getId() == static_cast<uint32_t>(d.frame->id());
    }
    return std::get<CanData>(_data).signal->isPresentInMessage(msg);
}

double GraphSignal::extractPhysicalFromMessage(const BusMessage &msg) const
{
    if (isBusLoad())
        return 0.0; // handled separately in the decoder worker
    if (isLin()) {
        const auto &d = std::get<LinData>(_data);
        std::span<const uint8_t> data(msg.getData(), msg.getLength());
        return d.signal->extractPhysicalValue(data);
    }
    return std::get<CanData>(_data).signal->extractPhysicalFromMessage(msg);
}

CanDbSignal *GraphSignal::asCanSignal() const noexcept
{
    if (!std::holds_alternative<CanData>(_data)) return nullptr;
    return std::get<CanData>(_data).signal;
}

LinSignal *GraphSignal::asLinSignal() const noexcept
{
    if (!isLin()) return nullptr;
    return std::get<LinData>(_data).signal;
}
