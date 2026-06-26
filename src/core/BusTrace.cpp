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

#include "BusTrace.h"
#include <QMutexLocker>
#include <QFile>
#include <QTextStream>
#include <QDataStream>
#include <QtEndian>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/CanDbSignal.h"


BusTrace::BusTrace(Backend &backend, QObject *parent, int flushInterval)
  : QObject(parent),
    _backend(backend),
    _maxSize(50000),
    _isTimerRunning(false),
    _mutex(),
    _timerMutex(),
    _flushTimer(this)
{
    clear();
    _flushTimer.setSingleShot(true);
    _flushTimer.setInterval(flushInterval);
    connect(&_flushTimer, &QTimer::timeout, this, &BusTrace::flushQueue);
}

unsigned long BusTrace::size()
{
    QMutexLocker locker(&_mutex);
    return _dataRowsUsed;
}

void BusTrace::clear()
{
    QMutexLocker locker(&_mutex);
    emit beforeClear();
    _data.resize(pool_chunk_size);
    _dataRowsUsed = 0;
    _newRows = 0;
    _muxCache.clear();
    emit afterClear();
}

BusMessage BusTrace::getMessage(int idx)
{
    QMutexLocker locker(&_mutex);
    if (idx >= (_dataRowsUsed + _newRows)) {
        return BusMessage();
    } else {
        return _data[idx];
    }
}

QVector<BusMessage> BusTrace::getSnapshot(int maxCount)
{
    QMutexLocker locker(&_mutex);
    const int total = _dataRowsUsed + _newRows;
    const int start = (maxCount > 0 && maxCount < total) ? total - maxCount : 0;
    QVector<BusMessage> result;
    result.reserve(total - start);
    for (int i = start; i < total; i++)
    {
        result.append(_data.at(i));
    }
    return result;
}

void BusTrace::enqueueMessage(const BusMessage &msg, bool more_to_follow)
{
    QMutexLocker locker(&_mutex);

    int idx = _dataRowsUsed + _newRows;
    if (idx>=_data.size()) {
        _data.resize(_data.size() + pool_chunk_size);
    }

    _data[idx] = msg;
    _newRows++;

    if (!more_to_follow) {
        startTimer();
    }

    emit messageEnqueued(idx);
}

void BusTrace::flushQueue()
{
    {
        QMutexLocker locker(&_timerMutex);
        _isTimerRunning = false;
    }

    QMutexLocker locker(&_mutex);
    if (_newRows) {
        emit beforeAppend(_newRows);

        // see if we have muxed messages. cache muxed values, if any.
        MeasurementSetup &setup = _backend.getSetup();
        for (int i=_dataRowsUsed; i<_dataRowsUsed + _newRows; i++) {
            BusMessage &msg = _data[i];
            CanDbMessage *dbmsg = setup.findDbMessage(msg);
            if (dbmsg && dbmsg->getMuxer()) {
                for (auto *signal : dbmsg->getSignals()) {
                    if (signal->isMuxed() && signal->isPresentInMessage(msg)) {
                        _muxCache[signal] = signal->extractRawDataFromMessage(msg);
                    }
                }
            }
        }

        _dataRowsUsed += _newRows;
        _newRows = 0;
        emit afterAppend();

        // Hard limit check - prune if we exceed maxSize
        if (_dataRowsUsed > _maxSize) {
            int toRemove = _maxSize / 10; // Remove 10% when limit hit
            if (toRemove > 0) {
                emit beforeRemove(toRemove);
                _data.remove(0, toRemove);
                _dataRowsUsed -= toRemove;
                emit afterRemove(toRemove);
            }
        }
    }
}

void BusTrace::setMaxSize(int maxSize)
{
    QMutexLocker locker(&_mutex);
    _maxSize = maxSize;
}

void BusTrace::startTimer()
{
    QMutexLocker locker(&_timerMutex);
    if (!_isTimerRunning) {
        _isTimerRunning = true;
        QMetaObject::invokeMethod(&_flushTimer, "start", Qt::QueuedConnection);
    }
}

void BusTrace::saveCanDump(QFile &file)
{
    QMutexLocker locker(&_mutex);
    QTextStream stream(&file);
    for (unsigned int i=0; i<size(); i++) {
        BusMessage *msg = &_data[i];
        QString line;
        line.append(QStringLiteral("(%1) ").arg(msg->getFloatTimestamp(), 0, 'f', 6));
        line.append(_backend.getInterfaceName(msg->getInterfaceId()));

        int idWidth = msg->isExtended() ? 8 : 3;
        QString idHex = QString::number(msg->getId(), 16).toUpper().rightJustified(idWidth, QLatin1Char('0'));

        if (msg->isErrorFrame()) {
            // Error frame: error flag in ID, 8 bytes of zero data
            QString errId = QString::number(0x20000000 | msg->getId(), 16).toUpper().rightJustified(8, QLatin1Char('0'));
            line.append(QStringLiteral(" %1#0000000000000000").arg(errId));
        } else if (msg->isFD()) {
            // CANFD: use ## separator with flags byte (bit 0 = BRS, bit 1 = ESI)
            uint8_t flags = msg->isBRS() ? 1 : 0;
            line.append(QStringLiteral(" %1##%2").arg(idHex).arg(flags));
            for (int i=0; i<msg->getLength(); i++) {
                line.append(QString::number(msg->getByte(i), 16).toUpper().rightJustified(2, QLatin1Char('0')));
            }
        } else if (msg->isRTR()) {
            // RTR: #R followed by DLC
            line.append(QStringLiteral(" %1#R%2").arg(idHex).arg(msg->getLength()));
        } else {
            line.append(QStringLiteral(" %1#").arg(idHex));
            for (int i=0; i<msg->getLength(); i++) {
                line.append(QString::number(msg->getByte(i), 16).toUpper().rightJustified(2, QLatin1Char('0')));
            }
        }
        stream << line << Qt::endl;
    }
}

void BusTrace::saveVectorAsc(QFile &file)
{
    QMutexLocker locker(&_mutex);
    QTextStream stream(&file);

    if (_data.length()<1) {
        return;
    }


    auto firstMessage = _data.first();
    double t_start = firstMessage.getFloatTimestamp();

    QLocale locale_c(QLocale::C);
    QString dt_start = locale_c.toString(firstMessage.getDateTime(), "ddd MMM dd hh:mm:ss.zzz ap yyyy");

    stream << "date " << dt_start << Qt::endl;
    stream << "base hex  timestamps absolute" << Qt::endl;
    stream << "internal events logged" << Qt::endl;
    stream << "// version 8.5.0" << Qt::endl;
    stream << "Begin Triggerblock " << dt_start << Qt::endl;
    stream << "   0.000000 Start of measurement" << Qt::endl;

    // Build sequential channel numbers from interface IDs
    QMap<BusInterfaceId, int> channelMap;
    int nextChannel = 1;
    for (unsigned int i = 0; i < size(); i++) {
        BusInterfaceId ifaceId = _data[i].getInterfaceId();
        if (!channelMap.contains(ifaceId)) {
            channelMap[ifaceId] = nextChannel++;
        }
    }

    // Enhanced LIN checksum (LIN 2.x): sum of PID + data bytes, carry-folded, inverted
    auto linEnhancedChecksum = [](uint8_t frameId, const BusMessage &m) -> uint8_t
    {
        uint8_t id = frameId & 0x3Fu;
        uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1u;
        uint8_t p1 = (~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5))) & 1u;
        uint16_t sum = id | (p0 << 6u) | (p1 << 7u);
        for (int i = 0; i < m.getLength(); ++i)
        {
            sum += m.getByte(i);
            if (sum > 255) sum -= 255;
        }
        return static_cast<uint8_t>(~sum & 0xFFu);
    };

    for (unsigned int i=0; i<size(); i++) {
        BusMessage &msg = _data[i];

        double t_current = msg.getFloatTimestamp();
        int channel = channelMap.value(msg.getInterfaceId(), 1);
        QString dir = msg.isRX() ? QStringLiteral("Rx") : QStringLiteral("Tx");

        if (msg.busType() == BusType::LIN)
        {
            uint8_t linId = static_cast<uint8_t>(msg.getId() & 0x3Fu);
            QString idHex = QString::number(linId, 16).rightJustified(2, QLatin1Char('0'));
            QString line;
            if (msg.isErrorFrame())
            {
                line = QStringLiteral("%1 %2  LIN %3 %4   LIN_ChecksumError")
                    .arg(t_current - t_start, 11, 'f', 6)
                    .arg(channel)
                    .arg(idHex)
                    .arg(dir);
            }
            else
            {
                uint8_t cs = linEnhancedChecksum(linId, msg);
                line = QStringLiteral("%1 %2  LIN %3 %4  d %5 %6checksum = %7 header_time = 0 full_time = 0")
                    .arg(t_current - t_start, 11, 'f', 6)
                    .arg(channel)
                    .arg(idHex)
                    .arg(dir)
                    .arg(msg.getLength())
                    .arg(msg.getDataHexString())
                    .arg(cs, 2, 16, QLatin1Char('0'));
            }
            stream << line << Qt::endl;
            continue;
        }

        if (msg.isErrorFrame()) {
            QString line = QStringLiteral("%1 %2  ErrorFrame")
                .arg(t_current - t_start, 11, 'f', 6)
                .arg(channel);
            stream << line << Qt::endl;
            continue;
        }

        QString id_hex_str = QString::number(msg.getId(), 16);
        QString id_dec_str = QString::number(msg.getId());
        if (msg.isExtended()) {
            id_hex_str.append(QLatin1Char('x'));
            id_dec_str.append(QLatin1Char('x'));
        }

        QString dataHex = msg.getDataHexString();

        QString line;
        if (msg.isFD()) {
            // Vector ASC CANFD format:
            // timestamp CANFD channel Rx/Tx ID_hex flags 0 0 DLC DataLength data
            // flags: bit 0 = BRS, bit 2 = ESI
            int flags = 0;
            if (msg.isBRS()) { flags |= 0x1; }

            line = QStringLiteral("%1 CANFD %2 %3 %4 %5 0 0 %6 %7 %8")
                .arg(t_current - t_start, 11, 'f', 6)
                .arg(channel, 3)
                .arg(dir)
                .arg(id_hex_str, 15)
                .arg(flags)
                .arg(msg.getLength())
                .arg(msg.getLength())
                .arg(dataHex);
        } else {
            line = QStringLiteral("%1 %2  %3 %4   %5 %6 %7  Length = 0 BitCount = 0 ID = %8")
                .arg(t_current - t_start, 11, 'f', 6)
                .arg(channel)
                .arg(id_hex_str, -15)
                .arg(dir)
                .arg(QLatin1Char(msg.isRTR() ? 'r' : 'd'))
                .arg(msg.getLength())
                .arg(dataHex)
                .arg(id_dec_str);
        }

        stream << line << Qt::endl;
    }

    stream << "End TriggerBlock" << Qt::endl;
}

void BusTrace::saveVectorMdf(QFile &file)
{
    QMutexLocker locker(&_mutex);
    if (_dataRowsUsed == 0) return;

    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::DoublePrecision);

    // Record layout per CAN frame:
    //   timestamp (float64, 8) + CAN_ID (uint32, 4) + DLC (uint8, 1)
    //   + Dir (uint8, 1) + DataBytes (8 bytes) = 22 bytes
    const quint32 recordSize = 22;
    const quint64 recordCount = _dataRowsUsed;

    // --- Helper: TXBLOCK size (24-byte header + text + NUL, 8-byte aligned) ---
    auto txSize = [](const QByteArray &t) -> quint64 {
        return ((24 + t.size() + 1) + 7) & ~7ULL;
    };

    // --- Pre-compute all block sizes ---
    const quint64 szId = 64;
    const quint64 szHd = 88;   // 24 + 6*8 + 16
    const quint64 szFh = 56;   // 24 + 2*8 + 16
    const quint64 szDg = 64;   // 24 + 4*8 + 8
    const quint64 szCg = 104;  // 24 + 6*8 + 32
    const quint64 szCn = 160;  // 24 + 8*8 + 72

    QByteArray txFhStr   = "CANgaroo MDF4 export";
    QByteArray txCgStr   = "CAN";
    QByteArray txTime    = "t";
    QByteArray txTimeSec = "s";
    QByteArray txId      = "CAN_ID";
    QByteArray txDlc     = "DLC";
    QByteArray txDir     = "Dir";
    QByteArray txData    = "DataBytes";

    // --- Pre-compute all block offsets ---
    quint64 off = 0;
    quint64 oId         = off; off += szId;
    quint64 oHd         = off; off += szHd;
    quint64 oFh         = off; off += szFh;
    quint64 oTxFh       = off; off += txSize(txFhStr);
    quint64 oDg         = off; off += szDg;
    quint64 oCg         = off; off += szCg;
    quint64 oTxCg       = off; off += txSize(txCgStr);
    quint64 oCnTime     = off; off += szCn;
    quint64 oTxTime     = off; off += txSize(txTime);
    quint64 oTxTimeSec  = off; off += txSize(txTimeSec);
    quint64 oCnId       = off; off += szCn;
    quint64 oTxId       = off; off += txSize(txId);
    quint64 oCnDlc      = off; off += szCn;
    quint64 oTxDlc      = off; off += txSize(txDlc);
    quint64 oCnDir      = off; off += szCn;
    quint64 oTxDir      = off; off += txSize(txDir);
    quint64 oCnData     = off; off += szCn;
    quint64 oTxData     = off; off += txSize(txData);
    quint64 oDt         = off;

    (void)oId; // suppress unused
    (void)oHd;

    // --- Helpers ---
    auto writeBlockHeader = [&ds](const char *id, quint64 length, quint64 linkCount) {
        ds.writeRawData(id, 4);
        ds << (quint32)0;
        ds << length;
        ds << linkCount;
    };

    auto writePad = [&ds](int bytes) {
        for (int i = 0; i < bytes; i++) ds << (quint8)0;
    };

    auto writeTx = [&](const QByteArray &text) {
        quint64 sz = txSize(text);
        writeBlockHeader("##TX", sz, 0);
        ds.writeRawData(text.constData(), text.size());
        ds << (quint8)0;
        int pad = sz - 24 - text.size() - 1;
        writePad(pad);
    };

    auto writeCn = [&](quint64 nextCn, quint64 nameOff, quint64 unitOff,
                       quint8 cnType, quint8 syncType, quint8 dataType,
                       quint32 byteOff, quint32 bitCount) {
        writeBlockHeader("##CN", szCn, 8);
        ds << nextCn;          // cn_cn_next
        ds << (quint64)0;     // cn_composition
        ds << nameOff;         // cn_tx_name
        ds << (quint64)0;     // cn_si_source
        ds << (quint64)0;     // cn_cc_conversion
        ds << (quint64)0;     // cn_data
        ds << unitOff;         // cn_md_unit
        ds << (quint64)0;     // cn_md_comment
        // Data section (72 bytes)
        ds << cnType;          // cn_type
        ds << syncType;        // cn_sync_type
        ds << dataType;        // cn_data_type
        ds << (quint8)0;       // cn_bit_offset
        ds << byteOff;         // cn_byte_offset
        ds << bitCount;        // cn_bit_count
        ds << (quint32)0;      // cn_flags
        ds << (quint32)0;      // cn_inval_bit_pos
        ds << (quint8)0;       // cn_precision
        ds << (quint8)0;       // reserved
        ds << (quint16)0;      // cn_attachment_count
        double zero = 0.0;
        for (int i = 0; i < 6; i++) ds << zero; // range/limit fields
    };

    // ===== IDBLOCK (64 bytes, no standard block header) =====
    ds.writeRawData("MDF     ", 8);
    ds.writeRawData("4.10    ", 8);
    ds.writeRawData("CANgaroo", 8);
    ds << (quint32)0;          // reserved
    ds << (quint16)410;        // version number
    ds << (quint16)0;          // reserved
    ds << (quint16)0;          // unfinalized flags
    ds << (quint16)0;          // custom unfin flags
    writePad(28);

    // ===== HDBLOCK (88 bytes) =====
    quint64 startTimeNs = static_cast<quint64>(_data[0].getTimestamp_us()) * 1000ULL;
    writeBlockHeader("##HD", szHd, 6);
    ds << oDg;                 // hd_dg_first
    ds << oFh;                 // hd_fh_first
    ds << (quint64)0;         // hd_ch_first
    ds << (quint64)0;         // hd_at_first
    ds << (quint64)0;         // hd_ev_first
    ds << (quint64)0;         // hd_md_comment
    ds << startTimeNs;         // start_time_ns
    ds << (qint16)0;           // tz_offset_min
    ds << (qint16)0;           // dst_offset_min
    ds << (quint8)2;           // time_flags (local time)
    ds << (quint8)0;           // time_class
    ds << (quint8)0;           // flags
    ds << (quint8)0;           // reserved

    // ===== FHBLOCK (56 bytes) =====
    writeBlockHeader("##FH", szFh, 2);
    ds << (quint64)0;         // fh_fh_next
    ds << oTxFh;               // fh_md_comment
    ds << startTimeNs;         // time_ns
    ds << (qint16)0;           // tz_offset_min
    ds << (qint16)0;           // dst_offset_min
    ds << (quint8)2;           // time_flags
    writePad(3);               // reserved

    // ===== TX: FH comment =====
    writeTx(txFhStr);

    // ===== DGBLOCK (64 bytes) =====
    writeBlockHeader("##DG", szDg, 4);
    ds << (quint64)0;         // dg_dg_next
    ds << oCg;                 // dg_cg_first
    ds << oDt;                 // dg_data
    ds << (quint64)0;         // dg_md_comment
    ds << (quint8)0;           // rec_id_size
    writePad(7);               // reserved

    // ===== CGBLOCK (104 bytes) =====
    writeBlockHeader("##CG", szCg, 6);
    ds << (quint64)0;         // cg_cg_next
    ds << oCnTime;             // cg_cn_first
    ds << oTxCg;               // cg_tx_acq_name
    ds << (quint64)0;         // cg_si_acq_source
    ds << (quint64)0;         // cg_sr_first
    ds << (quint64)0;         // cg_md_comment
    ds << (quint64)0;         // record_id
    ds << recordCount;         // cycle_count
    ds << (quint16)0;          // flags
    ds << (quint16)0;          // path_separator
    ds << (quint32)0;          // reserved
    ds << recordSize;          // data_bytes
    ds << (quint32)0;          // inval_bytes

    // ===== TX: CG name =====
    writeTx(txCgStr);

    // ===== CN channels (chained via cn_cn_next) =====
    // cn_type: 0=fixed, 2=master; cn_sync_type: 1=time; cn_data_type: 0=uint_le, 4=real_le, 10=byte_array
    writeCn(oCnId,  oTxTime,    oTxTimeSec, 2, 1, 4,  0,  64); // timestamp
    writeTx(txTime);
    writeTx(txTimeSec);

    writeCn(oCnDlc, oTxId,      0,          0, 0, 0,  8,  32); // CAN_ID
    writeTx(txId);

    writeCn(oCnDir, oTxDlc,     0,          0, 0, 0,  12,  8); // DLC
    writeTx(txDlc);

    writeCn(oCnData, oTxDir,    0,          0, 0, 0,  13,  8); // Dir
    writeTx(txDir);

    writeCn(0,       oTxData,   0,          0, 0, 10, 14, 64); // DataBytes
    writeTx(txData);

    // ===== DTBLOCK =====
    writeBlockHeader("##DT", 24 + recordCount * recordSize, 0);

    double t_start = _data[0].getFloatTimestamp();
    for (int i = 0; i < _dataRowsUsed; i++) {
        BusMessage &msg = _data[i];
        ds << (msg.getFloatTimestamp() - t_start); // 8 bytes
        ds << msg.getRawId();                       // 4 bytes
        ds << msg.getLength();                      // 1 byte
        ds << static_cast<quint8>(msg.isRX() ? 0 : 1); // 1 byte
        for (int j = 0; j < 8; j++) {              // 8 bytes
            ds << msg.getByte(j);
        }
    }
}

void BusTrace::savePcap(QFile &file)
{
    // PCAP file format with LINKTYPE_CAN_SOCKETCAN (227).
    // Standard CAN frames are stored as a 16-byte SocketCAN can_frame.
    // CAN FD frames are stored as a 72-byte SocketCAN canfd_frame.
    //
    // CAN ID flags (upper bits of the 32-bit can_id word):
    //   0x80000000  CAN_EFF_FLAG  — extended (29-bit) frame
    //   0x40000000  CAN_RTR_FLAG  — remote transmission request
    //   0x20000000  CAN_ERR_FLAG  — error frame
    //
    // canfd_frame flags byte:
    //   0x01  CANFD_BRS  — bit rate switch
    //   0x02  CANFD_ESI  — error state indicator

    static const quint32 PCAP_MAGIC       = 0xA1B2C3D4;
    static const quint16 PCAP_VERSION_MAJ = 2;
    static const quint16 PCAP_VERSION_MIN = 4;
    static const quint32 SNAPLEN          = 72; // max canfd_frame size
    static const quint32 LINKTYPE_CAN_SOCKETCAN = 227;

    static const quint32 CAN_EFF_FLAG = 0x80000000;
    static const quint32 CAN_RTR_FLAG = 0x40000000;
    static const quint32 CAN_ERR_FLAG = 0x20000000;

    static const quint8  CANFD_BRS = 0x01;
    static const quint8  CANFD_ESI = 0x02;
    Q_UNUSED(CANFD_ESI);

    QMutexLocker locker(&_mutex);

    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::LittleEndian);

    // Global header (24 bytes)
    ds << PCAP_MAGIC;
    ds << PCAP_VERSION_MAJ;
    ds << PCAP_VERSION_MIN;
    ds << qint32(0);   // thiszone: UTC
    ds << quint32(0);  // sigfigs
    ds << SNAPLEN;
    ds << LINKTYPE_CAN_SOCKETCAN;

    for (unsigned int i = 0; i < size(); i++)
    {
        BusMessage &msg = _data[i];

        int64_t ts_us  = msg.getTimestamp_us();
        quint32 ts_sec  = static_cast<quint32>(ts_us / 1000000);
        quint32 ts_usec = static_cast<quint32>(ts_us % 1000000);

        quint32 can_id = msg.getId();
        if (msg.isExtended())   can_id |= CAN_EFF_FLAG;
        if (msg.isRTR())        can_id |= CAN_RTR_FLAG;
        if (msg.isErrorFrame()) can_id |= CAN_ERR_FLAG;

        if (msg.isFD())
        {
            // canfd_frame: 4 + 1 + 1 + 2 + 64 = 72 bytes
            quint32 frame_len = 72;
            quint8 len   = msg.getLength();
            quint8 flags = 0;
            if (msg.isBRS()) flags |= CANFD_BRS;

            ds << ts_sec << ts_usec << frame_len << frame_len;
            ds << qToBigEndian(can_id);
            ds << len;
            ds << flags;
            ds << quint8(0) << quint8(0); // __res0, __res1
            for (int j = 0; j < 64; j++) {
                ds << (j < len ? msg.getByte(j) : quint8(0));
            }
        }
        else
        {
            // can_frame: 4 + 1 + 3 + 8 = 16 bytes
            quint32 frame_len = 16;
            quint8 dlc = msg.getLength();
            if (dlc > 8) dlc = 8;

            ds << ts_sec << ts_usec << frame_len << frame_len;
            ds << qToBigEndian(can_id);
            ds << dlc;
            ds << quint8(0) << quint8(0) << dlc; // pad, res0, res1
            for (int j = 0; j < 8; j++)
            {
                ds << (j < dlc ? msg.getByte(j) : quint8(0));
            }
        }
    }
}

// --- pcapng helpers (local to this TU) ---

// Write padding bytes so that the total bytes written from the start of
// the current block are aligned to a 4-byte boundary.
static void pcapngPad4(QDataStream &ds, quint32 unpadded)
{
    quint32 rem = unpadded % 4;
    if (rem) {
        for (quint32 p = 0; p < 4 - rem; p++)
            ds << quint8(0);
    }
}

static quint32 pcapngPadded(quint32 len)
{
    return (len + 3) & ~quint32(3);
}

// Write a pcapng option (type + length + value + pad).
static void pcapngWriteOption(QDataStream &ds, quint16 code, const QByteArray &value)
{
    ds << code;
    ds << quint16(value.size());
    ds.writeRawData(value.constData(), value.size());
    pcapngPad4(ds, value.size());
}

// Write opt_endofopt (code 0, length 0).
static void pcapngWriteEndOfOpt(QDataStream &ds)
{
    ds << quint16(0) << quint16(0);
}

void BusTrace::savePcapNg(QFile &file)
{
    // pcapng (IETF draft-tuexen-opsawg-pcapng) with LINKTYPE_CAN_SOCKETCAN.
    //
    // Block layout written by this function:
    //   1x  Section Header Block  (SHB)
    //   Nx  Interface Description Block  (IDB) — one per CAN interface
    //   Mx  Enhanced Packet Block  (EPB) — one per CAN message
    //
    // All multi-byte fields are little-endian (matching the SHB byte-order
    // magic 0x1A2B3C4D).

    static const quint32 BT_SHB = 0x0A0D0D0A;
    static const quint32 BT_IDB = 0x00000001;
    static const quint32 BT_EPB = 0x00000006;

    static const quint32 BYTE_ORDER_MAGIC = 0x1A2B3C4D;
    static const quint16 PCAPNG_VERSION_MAJ = 1;
    static const quint16 PCAPNG_VERSION_MIN = 0;

    static const quint32 LINKTYPE_CAN_SOCKETCAN = 227;
    static const quint32 SNAPLEN = 72;

    static const quint32 CAN_EFF_FLAG = 0x80000000;
    static const quint32 CAN_RTR_FLAG = 0x40000000;
    static const quint32 CAN_ERR_FLAG = 0x20000000;
    static const quint8  CANFD_BRS    = 0x01;

    QMutexLocker locker(&_mutex);

    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::LittleEndian);

    // --- Collect interfaces seen in the trace ---
    QMap<BusInterfaceId, int> ifaceIndexMap; // interfaceId → IDB index (0-based)
    QList<BusInterfaceId> ifaceOrder;
    for (unsigned int i = 0; i < size(); i++) {
        BusInterfaceId id = _data[i].getInterfaceId();
        if (!ifaceIndexMap.contains(id)) {
            ifaceIndexMap[id] = ifaceOrder.size();
            ifaceOrder.append(id);
        }
    }

    // --- Section Header Block (SHB) ---
    // Fixed body: byte-order magic (4) + major (2) + minor (2) + section length (8) = 16
    // Options: shb_userappl + opt_endofopt
    QByteArray appName = QByteArray("CANgaroo");
    // opt shb_userappl (code 4): 2+2+padded(value)
    quint32 shbOptLen = 4 + pcapngPadded(appName.size()) + 4; // userappl + endofopt
    quint32 shbTotalLen = 12 + 16 + shbOptLen + 4; // block-type(4)+len(4)+body+options+len(4)
    // But block type is part of the 12: type(4)+total_length(4)...+total_length(4)
    // SHB: type(4) + total_length(4) + bom(4) + maj(2) + min(2) + section_len(8) + options + total_length(4)
    shbTotalLen = 4 + 4 + 4 + 2 + 2 + 8 + shbOptLen + 4;

    ds << BT_SHB;
    ds << shbTotalLen;
    ds << BYTE_ORDER_MAGIC;
    ds << PCAPNG_VERSION_MAJ;
    ds << PCAPNG_VERSION_MIN;
    ds << quint64(0xFFFFFFFFFFFFFFFF); // section length: unspecified
    pcapngWriteOption(ds, 4, appName);  // shb_userappl
    pcapngWriteEndOfOpt(ds);
    ds << shbTotalLen;

    // --- Interface Description Blocks (IDB) ---
    for (int idx = 0; idx < ifaceOrder.size(); idx++) {
        QString name = _backend.getInterfaceName(ifaceOrder[idx]);
        QByteArray nameUtf8 = name.toUtf8();

        // Options: if_name (code 2) + opt_endofopt
        quint32 idbOptLen = 4 + pcapngPadded(nameUtf8.size()) + 4;
        // IDB: type(4) + total_length(4) + linktype(2) + reserved(2) + snaplen(4) + options + total_length(4)
        quint32 idbTotalLen = 4 + 4 + 2 + 2 + 4 + idbOptLen + 4;

        ds << BT_IDB;
        ds << idbTotalLen;
        ds << quint16(LINKTYPE_CAN_SOCKETCAN);
        ds << quint16(0); // reserved
        ds << SNAPLEN;
        pcapngWriteOption(ds, 2, nameUtf8); // if_name
        pcapngWriteEndOfOpt(ds);
        ds << idbTotalLen;
    }

    // --- Enhanced Packet Blocks (EPB) ---
    for (unsigned int i = 0; i < size(); i++) {
        BusMessage &msg = _data[i];

        int64_t ts_us = msg.getTimestamp_us();
        // pcapng timestamp: 64-bit value in units of the interface's ts_resol (default: microseconds)
        quint32 ts_high = static_cast<quint32>(ts_us >> 32);
        quint32 ts_low  = static_cast<quint32>(ts_us & 0xFFFFFFFF);

        quint32 can_id = msg.getId();
        if (msg.isExtended())   can_id |= CAN_EFF_FLAG;
        if (msg.isRTR())        can_id |= CAN_RTR_FLAG;
        if (msg.isErrorFrame()) can_id |= CAN_ERR_FLAG;

        quint32 ifaceIdx = ifaceIndexMap.value(msg.getInterfaceId(), 0);

        quint32 capturedLen, originalLen;
        if (msg.isFD()) {
            capturedLen = 72;
        } else {
            capturedLen = 16;
        }
        originalLen = capturedLen;

        // EPB: type(4) + total_length(4) + interface_id(4) + ts_high(4) + ts_low(4)
        //    + captured_len(4) + original_len(4) + packet_data(padded) + total_length(4)
        quint32 epbTotalLen = 4 + 4 + 4 + 4 + 4 + 4 + 4 + pcapngPadded(capturedLen) + 4;

        ds << BT_EPB;
        ds << epbTotalLen;
        ds << ifaceIdx;
        ds << ts_high;
        ds << ts_low;
        ds << capturedLen;
        ds << originalLen;

        // Packet data: SocketCAN frame
        ds << qToBigEndian(can_id);

        if (msg.isFD()) {
            quint8 len   = msg.getLength();
            quint8 flags = 0;
            if (msg.isBRS()) flags |= CANFD_BRS;
            ds << len;
            ds << flags;
            ds << quint8(0) << quint8(0);
            for (int j = 0; j < 64; j++) {
                ds << (j < len ? msg.getByte(j) : quint8(0));
            }
        } else {
            quint8 dlc = msg.getLength();
            if (dlc > 8) dlc = 8;
            ds << dlc;
            ds << quint8(0) << quint8(0) << quint8(0);
            for (int j = 0; j < 8; j++) {
                ds << (j < dlc ? msg.getByte(j) : quint8(0));
            }
        }

        // Pad packet data to 4-byte boundary (16 and 72 are both multiples of 4, so no padding needed,
        // but call it for correctness if frame sizes change in the future)
        pcapngPad4(ds, capturedLen);

        ds << epbTotalLen;
    }
}

bool BusTrace::getMuxedSignalFromCache(const CanDbSignal *signal, uint64_t *raw_value)
{
    auto it = _muxCache.constFind(signal);
    if (it != _muxCache.constEnd()) {
        *raw_value = it.value();
        return true;
    }
    return false;
}
