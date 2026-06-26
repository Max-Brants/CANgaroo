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

#include <QThread>
#include <QObject>
#include <atomic>
#include "driver/CanDriver.h"
#include "driver/BusInterface.h"

//class QThread;
#include "core/BusMessage.h"
class Backend;

class BusListener : public QObject
{
    Q_OBJECT

public:
    explicit BusListener(QObject *parent, Backend &backend, BusInterface &intf);
    virtual ~BusListener();

    BusInterfaceId getInterfaceId();
    BusInterface &getInterface();

signals:
    void messageReceived(const BusMessage &msg);

public slots:
    void run();

    void startThread();
    void requestStop();
    void waitFinish();

private:
    Backend &_backend;
    BusInterface &_intf;
    std::atomic<bool> _shouldBeRunning;
    std::atomic<bool> _openComplete;
    QThread *_thread;
};
