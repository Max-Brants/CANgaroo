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

#include "LinFrame.h"
#include "LinSignal.h"

LinFrame::LinFrame(LinDb *parent)
    : _parent(parent)
{
}

LinFrame::~LinFrame()
{
    qDeleteAll(_signals);
}

uint8_t LinFrame::id() const { return _id; }
void    LinFrame::setId(uint8_t id) { _id = id; }

QString LinFrame::name() const { return _name; }
void    LinFrame::setName(const QString &name) { _name = name; }

QString LinFrame::publisher() const { return _publisher; }
void    LinFrame::setPublisher(const QString &publisher) { _publisher = publisher; }

uint8_t LinFrame::length() const { return _length; }
void    LinFrame::setLength(uint8_t length) { _length = length; }

void LinFrame::addSignal(LinSignal *signal)
{
    _signals.append(signal);
}

LinSignal *LinFrame::findSignal(const QString &name) const
{
    for (LinSignal *s : _signals)
    {
        if (s->name() == name)
            return s;
    }
    return nullptr;
}

const LinSignalList &LinFrame::signalList() const
{
    return _signals;
}
