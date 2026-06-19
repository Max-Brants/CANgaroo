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

#include "PeakCanInterface.h"
#include "PeakCanDriver.h"

#include <chrono>

#include <QMutexLocker>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"

#include <windows.h>
#include <PCANBasic.h>

PeakCanInterface::PeakCanInterface(PeakCanDriver *driver, TPCANHandle channel, QString name)
  : BusInterface(reinterpret_cast<CanDriver*>(driver)),
    _channel(channel),
    _rxEvent(nullptr),
    _isOpen(false),
    _name(name),
    _bitrate(500000)
{
    memset(&_stats, 0, sizeof(_stats));
    memset(&_offset_stats, 0, sizeof(_offset_stats));
}

PeakCanInterface::~PeakCanInterface()
{
    if (_isOpen) {
        close();
    }
}

QString PeakCanInterface::getName() const
{
    return _name;
}

void PeakCanInterface::setName(QString name)
{
    _name = name;
}

QList<CanTiming> PeakCanInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;
    QList<unsigned> bitrates({5000, 10000, 20000, 33333, 47000, 50000,
                               83333, 95000, 100000, 125000, 250000,
                               500000, 800000, 1000000});
    unsigned i = 0;
    for (unsigned br : bitrates) {
        retval << CanTiming(i++, br, 0, 875);
    }
    return retval;
}

// Map a numeric bitrate to a PCAN baud rate constant.
static TPCANBaudrate bitrateToTPCAN(unsigned bitrate)
{
    switch (bitrate) {
        case 1000000: return PCAN_BAUD_1M;
        case  800000: return PCAN_BAUD_800K;
        case  500000: return PCAN_BAUD_500K;
        case  250000: return PCAN_BAUD_250K;
        case  125000: return PCAN_BAUD_125K;
        case  100000: return PCAN_BAUD_100K;
        case   95000: return PCAN_BAUD_95K;
        case   83333: return PCAN_BAUD_83K;
        case   50000: return PCAN_BAUD_50K;
        case   47000: return PCAN_BAUD_47K;
        case   33333: return PCAN_BAUD_33K;
        case   20000: return PCAN_BAUD_20K;
        case   10000: return PCAN_BAUD_10K;
        case    5000: return PCAN_BAUD_5K;
        default:      return PCAN_BAUD_500K;
    }
}

void PeakCanInterface::applyConfig(const MeasurementInterface &mi)
{
    if (!mi.doConfigure()) {
        log_info(QString("PeakCanInterface %1: not managed by cangaroo, skipping configuration").arg(_name));
        return;
    }
    _bitrate = mi.bitrate();
    log_info(QString("PeakCanInterface %1: configuration stored, bitrate=%2").arg(_name).arg(_bitrate));
}

unsigned PeakCanInterface::getBitrate()
{
    return _bitrate;
}

uint32_t PeakCanInterface::getCapabilities()
{
    return BusInterface::capability_listen_only;
}

void PeakCanInterface::open()
{
    TPCANStatus status = CAN_Initialize(_channel, bitrateToTPCAN(_bitrate),
                                        0, 0, 0);
    if (status != PCAN_ERROR_OK) {
        char errText[256] = {};
        CAN_GetErrorText(status, 0x09, errText); // 0x09 = English
        log_error(QString("PeakCanInterface %1: CAN_Initialize failed: %2")
                      .arg(_name, errText));
        return;
    }

    // Set up a receive event so readMessage() can block with a timeout
    // instead of busy-polling.
    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    status = CAN_SetValue(_channel, PCAN_RECEIVE_EVENT, &hEvent, sizeof(hEvent));
    if (status != PCAN_ERROR_OK) {
        log_error(QString("PeakCanInterface %1: PCAN_RECEIVE_EVENT setup failed")
                      .arg(_name));
        CloseHandle(hEvent);
        CAN_Uninitialize(_channel);
        return;
    }
    _rxEvent = hEvent;

    // Record the wall-clock time at channel open so that device-relative
    // PCAN timestamps (which start at 0) can be converted to Unix epoch µs.
    _channelOpenTime_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    _isOpen = true;
    log_info(QString("PeakCanInterface %1: opened").arg(_name));
}

bool PeakCanInterface::isOpen()
{
    return _isOpen;
}

void PeakCanInterface::close()
{
    if (_rxEvent) {
        CloseHandle(static_cast<HANDLE>(_rxEvent));
        _rxEvent = nullptr;
    }
    CAN_Uninitialize(_channel);
    _isOpen = false;
    log_info(QString("PeakCanInterface %1: closed").arg(_name));
}

void PeakCanInterface::sendMessage(const BusMessage &msg)
{
    if (!_isOpen) {
        log_error(QString("PeakCanInterface %1: cannot send, interface not open").arg(_name));
        return;
    }

    TPCANMsg frame;
    memset(&frame, 0, sizeof(frame));

    frame.ID      = msg.getId();
    frame.MSGTYPE = msg.isExtended() ? PCAN_MESSAGE_EXTENDED : PCAN_MESSAGE_STANDARD;

    if (msg.isRTR()) {
        frame.MSGTYPE = static_cast<TPCANMessageType>(frame.MSGTYPE | PCAN_MESSAGE_RTR);
    }

    frame.LEN = (msg.getLength() > 8) ? 8 : static_cast<BYTE>(msg.getLength());
    if (!msg.isRTR()) {
        for (int i = 0; i < frame.LEN; i++) {
            frame.DATA[i] = msg.getByte(i);
        }
    }

    TPCANStatus status = CAN_Write(_channel, &frame);
    if (status != PCAN_ERROR_OK) {
        char errText[256] = {};
        CAN_GetErrorText(status, 0x09, errText);
        log_error(QString("PeakCanInterface %1: CAN_Write failed: %2").arg(_name, errText));
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

bool PeakCanInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
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

    // The PCAN receive event only signals "queue is non-empty"; it does not
    // re-signal once per queued frame, and an auto-reset event does not
    // accumulate multiple pulses. Reading a single frame per wake-up can
    // therefore leave a growing backlog in the driver's receive queue under
    // bus load (frames eventually surface tens of seconds late). Drain the
    // queue completely on every wake, as PEAK's own samples do.
    DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(_rxEvent), timeout_ms);
    if (waitResult != WAIT_OBJECT_0) {
        return hasTx;
    }

    bool gotAny = false;

    while (true) {
        TPCANMsg       frame;
        TPCANTimestamp ts;

        TPCANStatus status = CAN_Read(_channel, &frame, &ts);
        if (status != PCAN_ERROR_OK) {
            if (!(status & PCAN_ERROR_QRCVEMPTY)) {
                _stats.rx_errors++;
            }
            break;
        }

        if (frame.MSGTYPE & PCAN_MESSAGE_STATUS) {
            // Bus status frame — update error counter, don't forward to trace
            _stats.rx_errors++;
            continue;
        }

        BusMessage msg;
        msg.setId(frame.ID);
        msg.setExtended((frame.MSGTYPE & PCAN_MESSAGE_EXTENDED) != 0);
        msg.setRTR((frame.MSGTYPE & PCAN_MESSAGE_RTR) != 0);
        msg.setErrorFrame((frame.MSGTYPE & PCAN_MESSAGE_STATUS) != 0);
        msg.setInterfaceId(getId());

        // PCAN timestamp is relative to channel open (starts at 0).
        // Add the wall-clock baseline captured at open() to produce Unix-epoch µs,
        // consistent with the TX echo timestamps produced by sendMessage().
        uint64_t device_us = ((uint64_t)ts.millis_overflow * 0x100000000ULL + ts.millis) * 1000ULL
                             + ts.micros;
        uint64_t total_us = _channelOpenTime_us + device_us;
        msg.setTimestamp(total_us / 1000000ULL, total_us % 1000000ULL);

        uint8_t len = (frame.LEN > 8) ? 8 : frame.LEN;
        msg.setLength(len);
        for (int i = 0; i < len; i++) {
            msg.setByte(i, frame.DATA[i]);
        }

        msglist.append(msg);
        _stats.rx_count++;
        addFrameBits(msg);
        gotAny = true;
    }

    return gotAny || hasTx;
}

bool PeakCanInterface::updateStatistics()
{
    return _isOpen;
}

void PeakCanInterface::resetStatistics()
{
    _offset_stats = _stats;
    BusInterface::resetStatistics();
}

uint32_t PeakCanInterface::getState()
{
    if (!_isOpen) {
        return state_stopped;
    }

    TPCANStatus status = CAN_GetStatus(_channel);

    if (status & PCAN_ERROR_BUSOFF)    return state_bus_off;
    if (status & PCAN_ERROR_BUSHEAVY)  return state_passive;
    if (status & PCAN_ERROR_BUSLIGHT)  return state_warning;

    return state_ok;
}

int PeakCanInterface::getNumRxFrames()  { return (int)(_stats.rx_count    - _offset_stats.rx_count);    }
int PeakCanInterface::getNumRxErrors()  { return       _stats.rx_errors   - _offset_stats.rx_errors;    }
int PeakCanInterface::getNumRxOverruns(){ return (int)(_stats.rx_overruns - _offset_stats.rx_overruns); }
int PeakCanInterface::getNumTxFrames()  { return (int)(_stats.tx_count    - _offset_stats.tx_count);    }
int PeakCanInterface::getNumTxErrors()  { return       _stats.tx_errors   - _offset_stats.tx_errors;    }
int PeakCanInterface::getNumTxDropped() { return (int)(_stats.tx_dropped  - _offset_stats.tx_dropped);  }

TPCANHandle PeakCanInterface::getChannel() const
{
    return _channel;
}
