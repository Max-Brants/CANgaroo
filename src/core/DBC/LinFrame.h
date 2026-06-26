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
#include <QString>
#include <QList>

class LinDb;
class LinSignal;

using LinSignalList = QList<LinSignal*>;

class LinFrame
{
public:
    explicit LinFrame(LinDb *parent);
    ~LinFrame();

    uint8_t id() const;
    void setId(uint8_t id);

    QString name() const;
    void setName(const QString &name);

    QString publisher() const;
    void setPublisher(const QString &publisher);

    uint8_t length() const;
    void setLength(uint8_t length);

    void addSignal(LinSignal *signal);
    LinSignal *findSignal(const QString &name) const;
    const LinSignalList &signalList() const;

private:
    LinDb        *_parent;
    uint8_t       _id     {0};
    QString       _name;
    QString       _publisher;
    uint8_t       _length {0};
    LinSignalList _signals;
};
