/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>

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

#include <QByteArray>
#include <QDomDocument>
#include <QMap>

#include "driver/CanDriver.h"
#include "driver/BusInterface.h"
#include "core/BusMessage.h"

class Backend;

enum class LinProtocolVersion { V1_3, V2_0, V2_1, V2_2, V2_2A };
enum class LinNodeMode       { Monitor, Master, Slave };

class MeasurementInterface
{
public:
    MeasurementInterface();

    BusType busType() const;
    void setBusType(BusType type);

    BusInterfaceId busInterface() const;
    void setBusInterface(BusInterfaceId busif);

    // LIN-specific configuration
    unsigned linBaudRate() const;
    void setLinBaudRate(unsigned baud);

    LinProtocolVersion linProtocolVersion() const;
    void setLinProtocolVersion(LinProtocolVersion ver);

    LinNodeMode linNodeMode() const;
    void setLinNodeMode(LinNodeMode mode);

    bool linListenOnly() const;
    void setLinListenOnly(bool val);

    bool linChecksumClassic() const;
    void setLinChecksumClassic(bool val);

    bool linWakeupOnBus() const;
    void setLinWakeupOnBus(bool val);

    QString linLdfPath() const;
    void setLinLdfPath(const QString &path);

    QString linScheduleTable() const;
    void setLinScheduleTable(const QString &table);

    uint8_t linScheduleTableIndex() const;
    void setLinScheduleTableIndex(uint8_t idx);

    QString linSlaveNode() const;
    void setLinSlaveNode(const QString &node);

    uint8_t linTimebaseMs() const;
    void setLinTimebaseMs(uint8_t ms);

    uint16_t linJitterUs() const;
    void setLinJitterUs(uint16_t us);

    const QMap<uint8_t, QByteArray> &linFrameDefaults() const;
    QMap<uint8_t, QByteArray>       &linFrameDefaultsRef();
    void setLinFrameDefaults(const QMap<uint8_t, QByteArray> &defaults);

    void cloneFrom(MeasurementInterface &origin);
    bool loadXML(Backend &backend, QDomElement &el);
    bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root);

    bool doConfigure() const;
    void setDoConfigure(bool doConfigure);

    bool isListenOnlyMode() const;
    void setListenOnlyMode(bool isListenOnlyMode);

    bool isOneShotMode() const;
    void setOneShotMode(bool isOneShotMode);

    bool isTripleSampling() const;
    void setTripleSampling(bool isTripleSampling);

    bool isCanFD() const;
    void setCanFD(bool isCanFD);

    unsigned bitrate() const;
    void setBitrate(unsigned bitrate);

    int samplePoint() const;
    void setSamplePoint(int samplePoint);

    unsigned fdBitrate() const;
    void setFdBitrate(unsigned fdBitrate);

    unsigned fdSamplePoint() const;
    void setFdSamplePoint(unsigned fdSamplePoint);

    bool doAutoRestart() const;
    void setAutoRestart(bool doAutoRestart);

    int autoRestartMs() const;
    void setAutoRestartMs(int autoRestartMs);

    bool isCustomBitrate() const;
    void setCustomBitrateEn(bool customBitrate);

    bool isCustomFdBitrate() const;
    void setCustomFdBitrateEn(bool customFdBitrate);

    bool isEnabled() const noexcept;
    void setEnabled(bool enabled) noexcept;

    uint32_t customBitrate() const;
    void setCustomBitrate(uint32_t customBitrate);

    uint32_t customFdBitrate() const;
    void setCustomFdBitrate(uint32_t customFdBitrate);

    bool isResolved() const noexcept;
    void setResolved(bool resolved) noexcept;
    QString savedDriverName() const;
    QString savedInterfaceName() const;

private:
    BusType        _busType;
    BusInterfaceId _busif;

    bool    _isResolved {false};
    QString _savedDriverName;
    QString _savedInterfaceName;

    bool _doConfigure;
    bool _enabled;

    unsigned _bitrate;
    unsigned _samplePoint;

    bool _isCanFD;
    unsigned _fdBitrate;
    unsigned _fdSamplePoint;

    bool _isListenOnlyMode;
    bool _isOneShotMode;
    bool _isTripleSampling;
    bool _doAutoRestart;
    int _autoRestartMs;

    bool _isCustomBitrate;
    bool _isCustomFdBitrate;

    uint32_t _CustomBitrate;
    uint32_t _CustomFdBitrate;

    // LIN
    unsigned           _linBaudRate;
    LinProtocolVersion _linProtocolVersion;
    LinNodeMode        _linNodeMode;
    bool               _linListenOnly;
    bool               _linChecksumClassic;
    bool               _linWakeupOnBus;
    QString            _linLdfPath;
    QString            _linScheduleTable;
    uint8_t            _linScheduleTableIndex {0};
    QString            _linSlaveNode;
    uint8_t            _linTimebaseMs {5};
    uint16_t           _linJitterUs   {0};

    QMap<uint8_t, QByteArray> _linFrameDefaults;
};
