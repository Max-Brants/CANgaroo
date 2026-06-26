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
#include "core/DBC/CanOpenDb.h"

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

GraphSignal::GraphSignal(const CanOpenObjectEntry *entry, quint8 nodeId)
    : _data(SdoData{entry, nodeId})
{
    Q_ASSERT(entry != nullptr);
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
    if (isSdo()) {
        const auto &d = std::get<SdoData>(_data);
        // Visualizations (legend labels, tooltips, CSV headers) all key off this name, so the
        // node has to be in it - otherwise the same object read from two different nodes is
        // impossible to tell apart on a chart.
        return QStringLiteral("%1 (node %2)").arg(d.entry->name).arg(d.nodeId);
    }
    return std::get<CanData>(_data).signal->name();
}

QString GraphSignal::unit() const
{
    if (isBusLoad())
        return QStringLiteral("%");
    if (isLin())
        return std::get<LinData>(_data).signal->unit();
    if (isSdo())
        return {};
    return std::get<CanData>(_data).signal->getUnit();
}

double GraphSignal::minValue() const
{
    if (isBusLoad())
        return 0.0;
    if (isLin())
        return std::get<LinData>(_data).signal->minValue();
    if (isSdo())
        return 0.0;
    return std::get<CanData>(_data).signal->getMinimumValue();
}

double GraphSignal::maxValue() const
{
    if (isBusLoad())
        return 100.0;
    if (isLin())
        return std::get<LinData>(_data).signal->maxValue();
    if (isSdo())
        return 0.0;
    return std::get<CanData>(_data).signal->getMaximumValue();
}

QString GraphSignal::parentName() const
{
    if (isBusLoad())
        return QStringLiteral("Bus Load");
    if (isLin()) {
        return std::get<LinData>(_data).frame->name();
    }
    if (isSdo()) {
        return QStringLiteral("SDO node %1").arg(std::get<SdoData>(_data).nodeId);
    }
    auto *msg = std::get<CanData>(_data).signal->getParentMessage();
    return msg ? msg->getName() : QString();
}

QString GraphSignal::comment() const
{
    if (isBusLoad() || isLin() || isSdo())
        return {};
    return std::get<CanData>(_data).signal->comment();
}

bool GraphSignal::isLin() const noexcept
{
    return std::holds_alternative<LinData>(_data);
}

bool GraphSignal::isSdo() const noexcept
{
    return std::holds_alternative<SdoData>(_data);
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
    if (isSdo()) {
        const auto &d = std::get<SdoData>(_data);
        if (msg.isExtended() || msg.getId() != (0x580u + d.nodeId) || msg.getLength() < 8)
            return false;
        const uint8_t cs = msg.getByte(0);
        const quint16 respIndex = static_cast<quint16>(msg.getByte(1)) | (static_cast<quint16>(msg.getByte(2)) << 8);
        const uint8_t respSubIndex = msg.getByte(3);
        // Upload (read) response: scs == 2 (cs bits 7-5), abort uses cs == 0x80 and won't match.
        return ((cs & 0xE0) == 0x40) && respIndex == d.entry->index && respSubIndex == d.entry->subIndex;
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
    if (isSdo()) {
        const auto &d = std::get<SdoData>(_data);
        const quint8 width = CanOpenDb::expeditedSdoByteWidth(*d.entry);
        QByteArray data;
        for (int i = 0; i < width; ++i)
            data.append(static_cast<char>(msg.getByte(4 + i)));
        return CanOpenDb::decodeNumericValue(*d.entry, data);
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

const CanOpenObjectEntry *GraphSignal::asSdoEntry() const noexcept
{
    if (!isSdo()) return nullptr;
    return std::get<SdoData>(_data).entry;
}

quint8 GraphSignal::sdoNodeId() const noexcept
{
    if (!isSdo()) return 0;
    return std::get<SdoData>(_data).nodeId;
}

void GraphSignal::setSdoNodeId(quint8 nodeId)
{
    if (!isSdo()) return;
    std::get<SdoData>(_data).nodeId = nodeId;
}

int GraphSignal::sdoPollIntervalMs() const noexcept
{
    if (!isSdo()) return 0;
    return std::get<SdoData>(_data).pollIntervalMs;
}

void GraphSignal::setSdoPollIntervalMs(int ms)
{
    if (!isSdo()) return;
    std::get<SdoData>(_data).pollIntervalMs = qMax(20, ms);
}
