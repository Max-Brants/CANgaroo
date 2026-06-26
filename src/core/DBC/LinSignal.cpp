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

#include "LinSignal.h"

LinSignal::LinSignal(LinFrame *parent)
    : _parent(parent)
{
}

QString LinSignal::name() const { return _name; }
void    LinSignal::setName(const QString &name) { _name = name; }

uint8_t LinSignal::bitOffset() const { return _bitOffset; }
void    LinSignal::setBitOffset(uint8_t offset) { _bitOffset = offset; }

uint8_t LinSignal::bitLength() const { return _bitLength; }
void    LinSignal::setBitLength(uint8_t length) { _bitLength = length; }

QString LinSignal::publisher() const { return _publisher; }
void    LinSignal::setPublisher(const QString &publisher) { _publisher = publisher; }

double LinSignal::factor() const { return _factor; }
void   LinSignal::setFactor(double factor) { _factor = factor; }

double LinSignal::offset() const { return _offset; }
void   LinSignal::setOffset(double offset) { _offset = offset; }

double LinSignal::minValue() const { return _min; }
void   LinSignal::setMinValue(double min) { _min = min; }

double LinSignal::maxValue() const { return _max; }
void   LinSignal::setMaxValue(double max) { _max = max; }

QString LinSignal::unit() const { return _unit; }
void    LinSignal::setUnit(const QString &unit) { _unit = unit; }

uint64_t LinSignal::initValue() const { return _initValue; }
void     LinSignal::setInitValue(uint64_t value) { _initValue = value; }

QString LinSignal::getValueName(uint64_t value) const
{
    return _valueTable.value(value);
}

void LinSignal::setValueName(uint64_t value, const QString &name)
{
    _valueTable.insert(value, name);
}

uint64_t LinSignal::extractRawValue(std::span<const uint8_t> data) const
{
    // LIN signals are always little-endian (LSB-first).
    uint64_t raw = 0;
    for (int i = 0; i < _bitLength; ++i)
    {
        uint32_t bitPos = _bitOffset + i;
        uint8_t  byteIdx = static_cast<uint8_t>(bitPos / 8);
        uint8_t  bitIdx  = static_cast<uint8_t>(bitPos % 8);
        if (byteIdx < data.size() && ((data[byteIdx] >> bitIdx) & 1u))
            raw |= (uint64_t{1} << i);
    }
    return raw;
}

double LinSignal::convertToPhysical(uint64_t rawValue) const
{
    return static_cast<double>(rawValue) * _factor + _offset;
}

double LinSignal::extractPhysicalValue(std::span<const uint8_t> data) const
{
    return convertToPhysical(extractRawValue(data));
}
