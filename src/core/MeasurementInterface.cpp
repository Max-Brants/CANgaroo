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

#include "MeasurementInterface.h"

#include "core/Backend.h"
#include "driver/CanDriver.h"
#include "driver/BusInterface.h"


MeasurementInterface::MeasurementInterface()
  : _busType(BusType::CAN),
    _busif(0),
    _doConfigure(true),
    _enabled(true),
    _bitrate(500000),
    _samplePoint(875),
    _isCanFD(false),
    _fdBitrate(2000000),
    _fdSamplePoint(875),

    _isListenOnlyMode(false),
    _isOneShotMode(false),
    _isTripleSampling(false),
    _doAutoRestart(false),
    _autoRestartMs(100),

    _isCustomBitrate(false),
    _isCustomFdBitrate(false),

    _CustomBitrate(0x023407),
    _CustomFdBitrate(0x011508),

    _linBaudRate(19200),
    _linProtocolVersion(LinProtocolVersion::V2_2A),
    _linNodeMode(LinNodeMode::Master),
    _linListenOnly(false),
    _linChecksumClassic(false),
    _linWakeupOnBus(false)
{

}

bool MeasurementInterface::loadXML(Backend &backend, QDomElement &el)
{
    (void) backend;

    _savedDriverName = el.attribute("driver");
    _savedInterfaceName = el.attribute("name");

    _busType = (el.attribute("bus-type", "can") == "lin") ? BusType::LIN : BusType::CAN;

    _doConfigure = el.attribute("configure", "0").toInt() != 0;

    _bitrate = el.attribute("bitrate", "500000").toInt();
    _samplePoint = el.attribute("sample-point", "875").toInt();
    _isCanFD = el.attribute("can-fd", "0").toInt() != 0;
    _fdBitrate = el.attribute("bitrate-fd", "500000").toInt();
    _fdSamplePoint = el.attribute("sample-point-fd", "875").toInt();

    _isListenOnlyMode = el.attribute("listen-only", "0").toInt() != 0;
    _isOneShotMode = el.attribute("one-shot", "0").toInt() != 0;
    _isTripleSampling = el.attribute("triple-sampling", "0").toInt() != 0;
    _doAutoRestart = el.attribute("auto-restart", "0").toInt() != 0;
    _autoRestartMs = el.attribute("auto-restart-time", "100").toInt();

    _isCustomBitrate = el.attribute("is-custom-bitrate", "0").toInt() != 0;
    _isCustomFdBitrate = el.attribute("is-custom-fdbitrate", "0").toInt() != 0;

    _CustomBitrate = el.attribute("custom-bitrate", "0").toInt();
    _CustomFdBitrate = el.attribute("custom-fdbitrate", "0").toInt();
    _enabled = el.attribute("enabled", "1").toInt() != 0;

    _linBaudRate = el.attribute("lin-baudrate", "19200").toUInt();
    _linProtocolVersion = static_cast<LinProtocolVersion>(el.attribute("lin-protocol", "4").toInt());
    _linNodeMode = static_cast<LinNodeMode>(el.attribute("lin-node-mode", "0").toInt());
    _linListenOnly = el.attribute("lin-listen-only", "0").toInt() != 0;
    _linChecksumClassic = el.attribute("lin-checksum-classic", "0").toInt() != 0;
    _linWakeupOnBus = el.attribute("lin-wakeup-on-bus", "0").toInt() != 0;
    _linLdfPath = el.attribute("lin-ldf-path", "");
    _linScheduleTable = el.attribute("lin-schedule-table", "");
    _linSlaveNode = el.attribute("lin-slave-node", "");
    _linTimebaseMs = static_cast<uint8_t>(el.attribute("lin-timebase-ms", "5").toUInt());
    _linJitterUs   = static_cast<uint16_t>(el.attribute("lin-jitter-us", "0").toUInt());

    _linFrameDefaults.clear();
    const QDomElement fdsEl = el.firstChildElement(QStringLiteral("lin-frame-defaults"));
    if (!fdsEl.isNull())
    {
        QDomElement fdEl = fdsEl.firstChildElement(QStringLiteral("frame"));
        while (!fdEl.isNull())
        {
            const uint8_t id   = static_cast<uint8_t>(fdEl.attribute(QStringLiteral("id"), QStringLiteral("0")).toUInt());
            const QByteArray d = QByteArray::fromHex(fdEl.attribute(QStringLiteral("data")).toLatin1());
            _linFrameDefaults.insert(id, d);
            fdEl = fdEl.nextSiblingElement(QStringLiteral("frame"));
        }
    }

    return true;
}

bool MeasurementInterface::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    (void) xml;

    if (_isResolved)
    {
        _savedDriverName = backend.getDriverName(_busif);
        _savedInterfaceName = backend.getInterfaceName(_busif);
    }

    root.setAttribute("bus-type", _busType == BusType::LIN ? "lin" : "can");
    root.setAttribute("type", _busType == BusType::LIN ? "lin" : "can");
    root.setAttribute("driver", _savedDriverName);
    root.setAttribute("name", _savedInterfaceName);

    root.setAttribute("configure", _doConfigure ? 1 : 0);

    root.setAttribute("bitrate", _bitrate);
    root.setAttribute("sample-point", _samplePoint);
    root.setAttribute("can-fd", _isCanFD ? 1 : 0);
    root.setAttribute("bitrate-fd", _fdBitrate);
    root.setAttribute("sample-point-fd", _fdSamplePoint);

    root.setAttribute("listen-only", _isListenOnlyMode ? 1 : 0);
    root.setAttribute("one-shot", _isOneShotMode ? 1 : 0);
    root.setAttribute("triple-sampling", _isTripleSampling ? 1 : 0);
    root.setAttribute("auto-restart", _doAutoRestart ? 1 : 0);
    root.setAttribute("auto-restart-time", _autoRestartMs);

    root.setAttribute("is-custom-bitrate", _isCustomBitrate ? 1 : 0);
    root.setAttribute("is-custom-fdbitrate", _isCustomFdBitrate ? 1 : 0);

    root.setAttribute("custom-bitrate", _CustomBitrate);
    root.setAttribute("custom-fdbitrate", _CustomFdBitrate);
    root.setAttribute("enabled", _enabled ? 1 : 0);

    root.setAttribute("lin-baudrate", _linBaudRate);
    root.setAttribute("lin-protocol", static_cast<int>(_linProtocolVersion));
    root.setAttribute("lin-node-mode", static_cast<int>(_linNodeMode));
    root.setAttribute("lin-listen-only", _linListenOnly ? 1 : 0);
    root.setAttribute("lin-checksum-classic", _linChecksumClassic ? 1 : 0);
    root.setAttribute("lin-wakeup-on-bus", _linWakeupOnBus ? 1 : 0);
    root.setAttribute("lin-ldf-path", _linLdfPath);
    root.setAttribute("lin-schedule-table", _linScheduleTable);
    root.setAttribute("lin-slave-node", _linSlaveNode);
    root.setAttribute("lin-timebase-ms", _linTimebaseMs);
    root.setAttribute("lin-jitter-us",   _linJitterUs);

    if (!_linFrameDefaults.isEmpty())
    {
        QDomElement fdsEl = xml.createElement(QStringLiteral("lin-frame-defaults"));
        for (auto it = _linFrameDefaults.constBegin(); it != _linFrameDefaults.constEnd(); ++it)
        {
            QDomElement fdEl = xml.createElement(QStringLiteral("frame"));
            fdEl.setAttribute(QStringLiteral("id"),   it.key());
            fdEl.setAttribute(QStringLiteral("data"), QString::fromLatin1(it.value().toHex()));
            fdsEl.appendChild(fdEl);
        }
        root.appendChild(fdsEl);
    }

    return true;
}

unsigned MeasurementInterface::bitrate() const
{
    return _bitrate;
}

void MeasurementInterface::setBitrate(unsigned bitrate)
{
    _bitrate = bitrate;
}

BusInterfaceId MeasurementInterface::busInterface() const
{
    return _busif;
}

void MeasurementInterface::setBusInterface(BusInterfaceId busif)
{
    _busif = busif;
}

void MeasurementInterface::cloneFrom(MeasurementInterface &origin)
{
    *this = origin;
}

bool MeasurementInterface::doConfigure() const
{
    return _doConfigure;
}

void MeasurementInterface::setDoConfigure(bool doConfigure)
{
    _doConfigure = doConfigure;
}

bool MeasurementInterface::isListenOnlyMode() const
{
    return _isListenOnlyMode;
}

void MeasurementInterface::setListenOnlyMode(bool isListenOnlyMode)
{
    _isListenOnlyMode = isListenOnlyMode;
}

bool MeasurementInterface::isOneShotMode() const
{
    return _isOneShotMode;
}

void MeasurementInterface::setOneShotMode(bool isOneShotMode)
{
    _isOneShotMode = isOneShotMode;
}

bool MeasurementInterface::isTripleSampling() const
{
    return _isTripleSampling;
}

void MeasurementInterface::setTripleSampling(bool isTripleSampling)
{
    _isTripleSampling = isTripleSampling;
}

bool MeasurementInterface::isCanFD() const
{
    return _isCanFD;
}

void MeasurementInterface::setCanFD(bool isCanFD)
{
    _isCanFD = isCanFD;
}

int MeasurementInterface::samplePoint() const
{
    return _samplePoint;
}

void MeasurementInterface::setSamplePoint(int samplePoint)
{
    _samplePoint = samplePoint;
}

unsigned MeasurementInterface::fdBitrate() const
{
    return _fdBitrate;
}

void MeasurementInterface::setFdBitrate(unsigned fdBitrate)
{
    _fdBitrate = fdBitrate;
}

unsigned MeasurementInterface::fdSamplePoint() const
{
    return _fdSamplePoint;
}

void MeasurementInterface::setFdSamplePoint(unsigned fdSamplePoint)
{
    _fdSamplePoint = fdSamplePoint;
}

bool MeasurementInterface::doAutoRestart() const
{
    return _doAutoRestart;
}

void MeasurementInterface::setAutoRestart(bool doAutoRestart)
{
    _doAutoRestart = doAutoRestart;
}

int MeasurementInterface::autoRestartMs() const
{
    return _autoRestartMs;
}

void MeasurementInterface::setAutoRestartMs(int autoRestartMs)
{
    _autoRestartMs = autoRestartMs;
}

bool MeasurementInterface::isCustomBitrate() const
{
    return _isCustomBitrate;
}

void MeasurementInterface::setCustomBitrateEn(bool customBitrate)
{
    _isCustomBitrate = customBitrate;
}

bool MeasurementInterface::isCustomFdBitrate() const
{
    return _isCustomFdBitrate;
}

void MeasurementInterface::setCustomFdBitrateEn(bool customFdBitrate)
{
    _isCustomFdBitrate = customFdBitrate;
}

uint32_t MeasurementInterface::customBitrate() const
{
    return _CustomBitrate;
}

void MeasurementInterface::setCustomBitrate(uint32_t customBitrate)
{
    _CustomBitrate = customBitrate;
}

uint32_t MeasurementInterface::customFdBitrate() const
{
    return _CustomFdBitrate;
}

void MeasurementInterface::setCustomFdBitrate(uint32_t customFdBitrate)
{
    _CustomFdBitrate = customFdBitrate;
}

bool MeasurementInterface::isEnabled() const noexcept
{
    return _enabled;
}

void MeasurementInterface::setEnabled(bool enabled) noexcept
{
    _enabled = enabled;
}

BusType MeasurementInterface::busType() const { return _busType; }
void    MeasurementInterface::setBusType(BusType type) { _busType = type; }

bool    MeasurementInterface::isResolved() const noexcept { return _isResolved; }
void    MeasurementInterface::setResolved(bool resolved) noexcept { _isResolved = resolved; }
QString MeasurementInterface::savedDriverName() const { return _savedDriverName; }
QString MeasurementInterface::savedInterfaceName() const { return _savedInterfaceName; }

unsigned MeasurementInterface::linBaudRate() const { return _linBaudRate; }
void     MeasurementInterface::setLinBaudRate(unsigned baud) { _linBaudRate = baud; }

LinProtocolVersion MeasurementInterface::linProtocolVersion() const { return _linProtocolVersion; }
void               MeasurementInterface::setLinProtocolVersion(LinProtocolVersion ver) { _linProtocolVersion = ver; }

LinNodeMode MeasurementInterface::linNodeMode() const { return _linNodeMode; }
void        MeasurementInterface::setLinNodeMode(LinNodeMode mode) { _linNodeMode = mode; }

bool MeasurementInterface::linListenOnly() const { return _linListenOnly; }
void MeasurementInterface::setLinListenOnly(bool val) { _linListenOnly = val; }

bool MeasurementInterface::linChecksumClassic() const { return _linChecksumClassic; }
void MeasurementInterface::setLinChecksumClassic(bool val) { _linChecksumClassic = val; }

bool MeasurementInterface::linWakeupOnBus() const { return _linWakeupOnBus; }
void MeasurementInterface::setLinWakeupOnBus(bool val) { _linWakeupOnBus = val; }

QString MeasurementInterface::linLdfPath() const { return _linLdfPath; }
void    MeasurementInterface::setLinLdfPath(const QString &path) { _linLdfPath = path; }

QString MeasurementInterface::linScheduleTable() const { return _linScheduleTable; }
void    MeasurementInterface::setLinScheduleTable(const QString &table) { _linScheduleTable = table; }

uint8_t MeasurementInterface::linScheduleTableIndex() const { return _linScheduleTableIndex; }
void    MeasurementInterface::setLinScheduleTableIndex(uint8_t idx) { _linScheduleTableIndex = idx; }

QString MeasurementInterface::linSlaveNode() const { return _linSlaveNode; }
void    MeasurementInterface::setLinSlaveNode(const QString &node) { _linSlaveNode = node; }

uint8_t MeasurementInterface::linTimebaseMs() const { return _linTimebaseMs; }
void    MeasurementInterface::setLinTimebaseMs(uint8_t ms) { _linTimebaseMs = ms; }

uint16_t MeasurementInterface::linJitterUs() const { return _linJitterUs; }
void     MeasurementInterface::setLinJitterUs(uint16_t us) { _linJitterUs = us; }

const QMap<uint8_t, QByteArray> &MeasurementInterface::linFrameDefaults() const { return _linFrameDefaults; }
QMap<uint8_t, QByteArray>       &MeasurementInterface::linFrameDefaultsRef()    { return _linFrameDefaults; }
void MeasurementInterface::setLinFrameDefaults(const QMap<uint8_t, QByteArray> &defaults) { _linFrameDefaults = defaults; }
