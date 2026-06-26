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

#pragma once

#include <stdint.h>
#include <QList>
#include <QString>

class Backend;
class BusInterface;

using BusInterfaceId = uint16_t;
using BusInterfaceIdList = QList<uint16_t>;

constexpr BusInterfaceId InvalidBusInterfaceId = 0xFFFF;

class CanDriver {
public:
    CanDriver(Backend &backend);
    virtual ~CanDriver();

    Backend &backend() const;

    void init(int id);

    int id();

    virtual QString getName() const = 0;
    virtual bool update() = 0;

    BusInterfaceIdList getInterfaceIds() const;
    QList<BusInterface*> getInterfaces() const;
    BusInterface *getInterfaceById(BusInterfaceId id);
    BusInterfaceId addInterface(BusInterface *intf);
    void deleteInterface(BusInterface *intf);
    void deleteAllInterfaces();

    BusInterface *getInterfaceByName(QString ifName);

private:
    Backend &_backend;
    int _id;
    QList<BusInterface*> _interfaces;

    void setId(int id);
};
