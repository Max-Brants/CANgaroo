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

#include <cstdint>
#include <span>
#include <QString>
#include <QMap>

class LinFrame;

using LinSignalValueTable = QMap<uint64_t, QString>;

class LinSignal
{
public:
    explicit LinSignal(LinFrame *parent);

    QString name() const;
    void setName(const QString &name);

    uint8_t bitOffset() const;
    void setBitOffset(uint8_t offset);

    uint8_t bitLength() const;
    void setBitLength(uint8_t length);

    QString publisher() const;
    void setPublisher(const QString &publisher);

    double factor() const;
    void setFactor(double factor);

    double offset() const;
    void setOffset(double offset);

    double minValue() const;
    void setMinValue(double min);

    double maxValue() const;
    void setMaxValue(double max);

    QString unit() const;
    void setUnit(const QString &unit);

    uint64_t initValue() const;
    void setInitValue(uint64_t value);

    QString getValueName(uint64_t value) const;
    void setValueName(uint64_t value, const QString &name);

    // Extract the raw bit field from a LIN frame payload (little-endian).
    uint64_t extractRawValue(std::span<const uint8_t> data) const;
    double   convertToPhysical(uint64_t rawValue) const;
    double   extractPhysicalValue(std::span<const uint8_t> data) const;

private:
    LinFrame *_parent;
    QString   _name;
    uint8_t   _bitOffset {0};
    uint8_t   _bitLength {0};
    QString   _publisher;
    double    _factor    {1.0};
    double    _offset    {0.0};
    double    _min       {0.0};
    double    _max       {0.0};
    QString   _unit;
    uint64_t  _initValue {0};
    LinSignalValueTable _valueTable;
};
