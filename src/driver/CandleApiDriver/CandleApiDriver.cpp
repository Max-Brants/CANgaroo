/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>

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
#include "api/candle_defs.h"
#include "CandleApiDriver.h"
#include "api/candle.h"

#include "CandleApiInterface.h"
#include "driver/GenericCanSetupPage.h"


CandleApiDriver::CandleApiDriver(Backend &backend)
  : CanDriver(backend),
    setupPage(new GenericCanSetupPage(0))
{
    QObject::connect(&backend, &Backend::onSetupDialogCreated, setupPage, &GenericCanSetupPage::onSetupDialogCreated);
}

QString CandleApiDriver::getName() const
{
    return "CandleAPI";
}

bool CandleApiDriver::update()
{
    deleteAllInterfaces();

    candle_list_handle clist;
    uint8_t num_devices;
    candle_handle dev;

    if (candle_list_scan(&clist)) {
        if (candle_list_length(clist, &num_devices)) {
            for (uint8_t i = 0; i < num_devices; i++) {
                if (candle_dev_get(clist, i, &dev)) {
                    /* Open the device temporarily to read channel count and capabilities */
                    if (!candle_dev_open(dev)) {
                        candle_dev_free(dev);
                        continue;
                    }

                    uint8_t num_channels = 0;
                    if (!candle_channel_count(dev, &num_channels) || num_channels == 0) {
                        candle_dev_close(dev);
                        candle_dev_free(dev);
                        continue;
                    }

                    std::wstring devPath(candle_dev_get_path(dev));

                    /* Create one interface per channel */
                    for (uint8_t ch = 0; ch < num_channels; ch++) {
                        CandleApiInterface *cif = findInterface(devPath, ch);

                        if (cif == nullptr) {
                            /* Create a new handle copy for each channel interface */
                            candle_handle ch_dev;
                            candle_dev_get(clist, i, &ch_dev);
                            /* Copy device path and state from original dev */
                            candle_device_t *src = (candle_device_t*)dev;
                            candle_device_t *dst = (candle_device_t*)ch_dev;
                            wcscpy(dst->path, src->path);
                            dst->state = src->state;
                            /* Copy the capabilities we queried during open */
                            dst->dconf = src->dconf;
                            memcpy(dst->ch_caps, src->ch_caps, sizeof(src->ch_caps));
                            dst->bt_const = src->bt_const;

                            cif = new CandleApiInterface(this, ch_dev, ch);
                            addInterface(cif);
                        }
                    }

                    candle_dev_close(dev);
                    candle_dev_free(dev);
                }
            }
        }
        candle_list_free(clist);
    }

    return true;
}

CandleApiInterface *CandleApiDriver::findInterface(const std::wstring &path, uint8_t channel)
{
    for (auto *intf : getInterfaces()) {
        CandleApiInterface *cif = dynamic_cast<CandleApiInterface*>(intf);
        if (cif && cif->getChannel() == channel && cif->getPath() == path) {
            return cif;
        }
    }
    return nullptr;
}


