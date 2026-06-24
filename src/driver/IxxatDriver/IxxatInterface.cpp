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

#include "IxxatInterface.h"
#include "IxxatDriver.h"

#include <chrono>

#include <QMutexLocker>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"

#define INIT_CAN_BITRATES
#include <vcinpl2.h>

static VCIID toVciid(IxxatDeviceId id)
{
    VCIID vciid;
    vciid.AsInt64 = id;
    return vciid;
}

IxxatInterface::IxxatInterface(IxxatDriver *driver, IxxatDeviceId deviceId, uint32_t canNo, QString name)
  : BusInterface(reinterpret_cast<CanDriver*>(driver)),
    _deviceId(deviceId),
    _canNo(canNo),
    _hDevice(nullptr),
    _hCanCtl(nullptr),
    _hCanChn(nullptr),
    _isOpen(false),
    _name(name),
    _bitrate(500000)
{
    memset(&_stats, 0, sizeof(_stats));
    memset(&_offset_stats, 0, sizeof(_offset_stats));
}

IxxatInterface::~IxxatInterface()
{
    if (_isOpen) {
        close();
    }
}

QString IxxatInterface::getName() const
{
    return _name;
}

void IxxatInterface::setName(QString name)
{
    _name = name;
}

QList<CanTiming> IxxatInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;
    QList<unsigned> bitrates({10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000});
    unsigned i = 0;
    for (unsigned br : bitrates) {
        retval << CanTiming(i++, br, 0, 875);
    }
    return retval;
}

static const CANBTP &bitrateToBtp(unsigned bitrate)
{
    switch (bitrate) {
        case 10000:   return CAN_BITRATE_10KB;
        case 20000:   return CAN_BITRATE_20KB;
        case 50000:   return CAN_BITRATE_50KB;
        case 100000:  return CAN_BITRATE_100KB;
        case 125000:  return CAN_BITRATE_125KB;
        case 250000:  return CAN_BITRATE_250KB;
        case 800000:  return CAN_BITRATE_800KB;
        case 1000000: return CAN_BITRATE_1000KB;
        case 500000:
        default:      return CAN_BITRATE_500KB;
    }
}

void IxxatInterface::applyConfig(const MeasurementInterface &mi)
{
    if (!mi.doConfigure()) {
        log_info(QString("IxxatInterface %1: not managed by cangaroo, skipping configuration").arg(_name));
        return;
    }

    _bitrate = mi.bitrate();
    log_info(QString("IxxatInterface %1: configuration stored, bitrate=%2").arg(_name).arg(_bitrate));
}

unsigned IxxatInterface::getBitrate()
{
    return _bitrate;
}

uint32_t IxxatInterface::getCapabilities()
{
    return BusInterface::capability_listen_only;
}

void IxxatInterface::open()
{
    VCIID vciid = toVciid(_deviceId);
    HANDLE hDevice = nullptr;

    HRESULT hr = vciDeviceOpen(vciid, &hDevice);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: vciDeviceOpen failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        return;
    }

    HANDLE hCanCtl = nullptr;
    hr = canControlOpen(hDevice, _canNo, &hCanCtl);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canControlOpen failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        vciDeviceClose(hDevice);
        return;
    }

    CANCAPABILITIES2 caps;
    memset(&caps, 0, sizeof(caps));
    hr = canControlGetCaps(hCanCtl, &caps);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canControlGetCaps failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        canControlClose(hCanCtl);
        vciDeviceClose(hDevice);
        return;
    }

    if (caps.dwTscClkFreq != 0) {
        _tscClkFreq = caps.dwTscClkFreq;
        _tscDivisor = caps.dwTscDivisor ? caps.dwTscDivisor : 1;
    }

    UINT8 bOpMode = CAN_OPMODE_UNDEFINED;
    if (caps.dwFeatures & CAN_FEATURE_STDANDEXT) {
        bOpMode |= CAN_OPMODE_STANDARD;
        bOpMode |= CAN_OPMODE_EXTENDED;
    } else if (caps.dwFeatures & CAN_FEATURE_STDOREXT) {
        bOpMode |= CAN_OPMODE_STANDARD;
    }
    if (caps.dwFeatures & CAN_FEATURE_ERRFRAME) {
        bOpMode |= CAN_OPMODE_ERRFRAME;
    }

    CANBTP sBtpSdr;
    memcpy(&sBtpSdr, &bitrateToBtp(_bitrate), sizeof(sBtpSdr));
    CANBTP sBtpFdr;
    memcpy(&sBtpFdr, &CAN_BITRATE_NONE, sizeof(sBtpFdr));

    hr = canControlInitialize(hCanCtl,
            bOpMode,
            CAN_EXMODE_DISABLED,
            CAN_FILTER_PASS,
            CAN_FILTER_PASS,
            0,
            0,
            &sBtpSdr,
            &sBtpFdr);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canControlInitialize failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        canControlClose(hCanCtl);
        vciDeviceClose(hDevice);
        return;
    }

    hr = canControlStart(hCanCtl, TRUE);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canControlStart failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        canControlClose(hCanCtl);
        vciDeviceClose(hDevice);
        return;
    }

    HANDLE hCanChn = nullptr;
    hr = canChannelOpen(hDevice, _canNo, FALSE, &hCanChn);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canChannelOpen failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        canControlStart(hCanCtl, FALSE);
        canControlClose(hCanCtl);
        vciDeviceClose(hDevice);
        return;
    }

    hr = canChannelInitialize(hCanChn, 1024, 1, 128, 1, 0, CAN_FILTER_PASS);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canChannelInitialize failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        canChannelClose(hCanChn);
        canControlStart(hCanCtl, FALSE);
        canControlClose(hCanCtl);
        vciDeviceClose(hDevice);
        return;
    }

    hr = canChannelActivate(hCanChn, TRUE);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canChannelActivate failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        canChannelClose(hCanChn);
        canControlStart(hCanCtl, FALSE);
        canControlClose(hCanCtl);
        vciDeviceClose(hDevice);
        return;
    }

    // CANMSG2.dwTime is relative to channel activation (starts at 0).
    // Add the wall-clock baseline captured here to produce Unix-epoch µs,
    // consistent with TX echo timestamps produced by sendMessage().
    _channelOpenTime_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    _hDevice = hDevice;
    _hCanCtl = hCanCtl;
    _hCanChn = hCanChn;
    _isOpen = true;
    log_info(QString("IxxatInterface %1: opened").arg(_name));
}

bool IxxatInterface::isOpen()
{
    return _isOpen;
}

void IxxatInterface::close()
{
    if (_hCanChn) {
        canChannelActivate(static_cast<HANDLE>(_hCanChn), FALSE);
        canChannelClose(static_cast<HANDLE>(_hCanChn));
        _hCanChn = nullptr;
    }
    if (_hCanCtl) {
        canControlStart(static_cast<HANDLE>(_hCanCtl), FALSE);
        canControlClose(static_cast<HANDLE>(_hCanCtl));
        _hCanCtl = nullptr;
    }
    if (_hDevice) {
        vciDeviceClose(static_cast<HANDLE>(_hDevice));
        _hDevice = nullptr;
    }
    _isOpen = false;
    log_info(QString("IxxatInterface %1: closed").arg(_name));
}

void IxxatInterface::sendMessage(const BusMessage &msg)
{
    if (!_isOpen) {
        log_error(QString("IxxatInterface %1: cannot send, interface not open").arg(_name));
        return;
    }

    uint8_t dlc = msg.getLength();
    if (dlc > CAN_SLEN_MAX) dlc = CAN_SLEN_MAX;

    CANMSG2 canMsg;
    memset(&canMsg, 0, sizeof(canMsg));
    canMsg.dwTime = 0;
    canMsg.dwMsgId = msg.getId();
    canMsg.uMsgInfo.Bytes.bType = CAN_MSGTYPE_DATA;
    canMsg.uMsgInfo.Bytes.bFlags = CAN_MAKE_MSGFLAGS(dlc, 0, 0, msg.isRTR() ? 1 : 0, msg.isExtended() ? 1 : 0);
    canMsg.uMsgInfo.Bytes.bFlags2 = 0;
    canMsg.uMsgInfo.Bytes.bAccept = 0;

    if (!msg.isRTR()) {
        for (int i = 0; i < dlc; i++) {
            canMsg.abData[i] = msg.getByte(i);
        }
    }

    HRESULT hr = canChannelPostMessage(static_cast<HANDLE>(_hCanChn), &canMsg);
    if (hr != VCI_OK) {
        log_error(QString("IxxatInterface %1: canChannelPostMessage failed: 0x%2").arg(_name).arg((quint32)hr, 0, 16));
        _stats.tx_errors++;
    } else {
        _stats.tx_count++;
        addFrameBits(msg);

        BusMessage txMsg = msg;
        txMsg.setRX(false);
        auto now = std::chrono::system_clock::now().time_since_epoch();
        txMsg.setTimestamp_us(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        QMutexLocker lock(&_txMutex);
        _txMsgList.append(txMsg);
    }
}

bool IxxatInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    if (!_isOpen) {
        return false;
    }

    // Enqueue tx messages
    {
        QMutexLocker lock(&_txMutex);
        msglist.append(_txMsgList);
        _txMsgList.clear();
    }
    bool hasTx = !msglist.isEmpty();
    if (hasTx)
    {
        timeout_ms = 1;
    }

    CANMSG2 canMsg;
    HRESULT hr = canChannelReadMessage(static_cast<HANDLE>(_hCanChn), timeout_ms, &canMsg);
    if (hr != VCI_OK) {
        return hasTx;
    }

    if (canMsg.uMsgInfo.Bytes.bType == CAN_MSGTYPE_ERROR) {
        _stats.rx_errors++;
        return hasTx;
    }

    if (canMsg.uMsgInfo.Bytes.bType != CAN_MSGTYPE_DATA) {
        return hasTx;
    }

    BusMessage msg;
    msg.setId(canMsg.dwMsgId & CAN_MSGID_MASK29);
    msg.setExtended(canMsg.uMsgInfo.Bits.ext != 0);
    msg.setRTR(canMsg.uMsgInfo.Bits.rtr != 0);
    msg.setErrorFrame(false);
    msg.setInterfaceId(getId());

    // dwTime is in timestamp-counter ticks relative to channel activation.
    // Convert to µs using the controller's tsc clock frequency / divisor
    // (cached at open()), then add the wall-clock baseline.
    uint64_t dev_us = (static_cast<uint64_t>(canMsg.dwTime) * _tscDivisor * 1000000ULL) / _tscClkFreq;
    uint64_t total_us = _channelOpenTime_us + dev_us;
    msg.setTimestamp(total_us / 1000000ULL, total_us % 1000000ULL);

    uint8_t len = canMsg.uMsgInfo.Bits.edl
                    ? CAN_EDLC_TO_LEN(canMsg.uMsgInfo.Bits.dlc)
                    : CAN_SDLC_TO_LEN(canMsg.uMsgInfo.Bits.dlc);
    if (len > 8) len = 8;
    msg.setLength(len);
    for (int i = 0; i < len; i++) {
        msg.setByte(i, canMsg.abData[i]);
    }

    msglist.append(msg);
    _stats.rx_count++;
    addFrameBits(msg);
    return true;
}

bool IxxatInterface::updateStatistics()
{
    return _isOpen;
}

void IxxatInterface::resetStatistics()
{
    _offset_stats = _stats;
    BusInterface::resetStatistics();
}

uint32_t IxxatInterface::getState()
{
    if (!_isOpen) {
        return state_stopped;
    }

    CANLINESTATUS2 status;
    memset(&status, 0, sizeof(status));
    HRESULT hr = canControlGetStatus(static_cast<HANDLE>(_hCanCtl), &status);
    if (hr != VCI_OK) {
        return state_unknown;
    }

    if (status.dwStatus & CAN_STATUS_BUSOFF)  return state_bus_off;
    if (status.dwStatus & CAN_STATUS_ERRLIM)  return state_warning;

    return state_ok;
}

int IxxatInterface::getNumRxFrames()
{
    return (int)(_stats.rx_count - _offset_stats.rx_count);
}

int IxxatInterface::getNumRxErrors()
{
    return _stats.rx_errors - _offset_stats.rx_errors;
}

int IxxatInterface::getNumRxOverruns()
{
    return (int)(_stats.rx_overruns - _offset_stats.rx_overruns);
}

int IxxatInterface::getNumTxFrames()
{
    return (int)(_stats.tx_count - _offset_stats.tx_count);
}

int IxxatInterface::getNumTxErrors()
{
    return _stats.tx_errors - _offset_stats.tx_errors;
}

int IxxatInterface::getNumTxDropped()
{
    return (int)(_stats.tx_dropped - _offset_stats.tx_dropped);
}

IxxatDeviceId IxxatInterface::getDeviceId() const
{
    return _deviceId;
}

uint32_t IxxatInterface::getCanNo() const
{
    return _canNo;
}
