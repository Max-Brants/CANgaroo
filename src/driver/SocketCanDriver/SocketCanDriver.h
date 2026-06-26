/*

  Copyright (c) 2015, 2016 Hubert Denkmair
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

#include <QString>
#include "core/Backend.h"
#include "driver/CanDriver.h"

class SocketCanInterface;
class SetupDialogInterfacePage;
class GenericCanSetupPage;

class SocketCanDriver: public CanDriver {
public:
    SocketCanDriver(Backend &backend);
    ~SocketCanDriver() override;

    QString getName() const override;
    bool update() override;

private:
    SocketCanInterface *createOrUpdateInterface(int index, QString name);
    GenericCanSetupPage *setupPage;
};
