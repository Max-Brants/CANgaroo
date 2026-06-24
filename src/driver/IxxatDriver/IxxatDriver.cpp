/*

  Copyright (c) 2026 Max Brants

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

#include "IxxatDriver.h"
#include "IxxatInterface.h"
#include "core/Backend.h"
#include "driver/GenericCanSetupPage.h"

#include <vcinpl2.h>

IxxatDriver::IxxatDriver(Backend &backend)
  : CanDriver(backend),
    setupPage(new GenericCanSetupPage())
{
    QObject::connect(&backend, &Backend::onSetupDialogCreated, setupPage, &GenericCanSetupPage::onSetupDialogCreated);
    vciInitialize();
}

IxxatDriver::~IxxatDriver()
{
}

QString IxxatDriver::getName() const
{
    return "IXXAT";
}

bool IxxatDriver::update()
{
    deleteAllInterfaces();

    HANDLE hEnum = nullptr;
    HRESULT hr = vciEnumDeviceOpen(&hEnum);
    if (hr != VCI_OK) {
        log_error(QString("IxxatDriver: vciEnumDeviceOpen failed: 0x%1").arg((quint32)hr, 0, 16));
        return false;
    }

    VCIDEVICEINFO devInfo;
    while (vciEnumDeviceNext(hEnum, &devInfo) == VCI_OK) {
        HANDLE hDevice = nullptr;
        if (vciDeviceOpen(devInfo.VciObjectId, &hDevice) != VCI_OK) {
            continue;
        }

        VCIDEVICECAPS devCaps;
        memset(&devCaps, 0, sizeof(devCaps));
        if (vciDeviceGetCaps(hDevice, &devCaps) == VCI_OK) {
            QString desc = QString::fromLocal8Bit(devInfo.Description);
            for (UINT16 i = 0; i < devCaps.BusCtrlCount; i++) {
                if (VCI_BUS_TYPE(devCaps.BusCtrlTypes[i]) == VCI_BUS_CAN) {
                    QString name = QString("%1 (can%2)").arg(desc).arg(i);
                    createOrUpdateInterface(devInfo.VciObjectId.AsInt64, i, name);
                }
            }
        }

        vciDeviceClose(hDevice);
    }

    vciEnumDeviceClose(hEnum);

    return true;
}

IxxatInterface *IxxatDriver::createOrUpdateInterface(IxxatDeviceId deviceId, uint32_t canNo, QString name)
{
    for (auto *intf : getInterfaces()) {
        IxxatInterface *iif = dynamic_cast<IxxatInterface*>(intf);
        if (iif && iif->getDeviceId() == deviceId && iif->getCanNo() == canNo) {
            iif->setName(name);
            return iif;
        }
    }

    IxxatInterface *iif = new IxxatInterface(this, deviceId, canNo, name);
    addInterface(iif);
    return iif;
}
