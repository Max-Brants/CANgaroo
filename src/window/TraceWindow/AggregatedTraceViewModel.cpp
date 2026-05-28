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

#include "AggregatedTraceViewModel.h"
#include <QColor>
#include <QDateTime>
#include <QSettings>
#include <QSet>
#include "core/ThemeManager.h"

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/LinFrame.h"

AggregatedTraceViewModel::AggregatedTraceViewModel(Backend &backend)
    : BaseTraceViewModel(backend), _protocolManager(&backend)
{
    _rootItem = new AggregatedTraceViewItem(0);
    QSettings s;
    _protocolManager.config().enableUds29Bit = s.value("decoder/uds29Bit", true).toBool();
    connect(backend.getTrace(), &BusTrace::beforeAppend, this, &AggregatedTraceViewModel::beforeAppend);
    connect(backend.getTrace(), &BusTrace::beforeClear, this, &AggregatedTraceViewModel::beforeClear);
    connect(backend.getTrace(), &BusTrace::afterClear, this, &AggregatedTraceViewModel::afterClear);

    connect(&backend, &Backend::onSetupChanged, this, &AggregatedTraceViewModel::onSetupChanged);

    // Periodically repaint so stale-message fade updates without user interaction
    connect(&_fadeTimer, &QTimer::timeout, this, [this]()
    {
        int rows = _rootItem->childCount();
        if (rows > 0)
        {
            _fadeNowMs = QDateTime::currentMSecsSinceEpoch();
            emit dataChanged(index(0, 0, QModelIndex()),
                             index(rows - 1, columnCount(QModelIndex()) - 1, QModelIndex()),
                             {Qt::ForegroundRole});
        }
    });
    _fadeTimer.start(200);
}

QString AggregatedTraceViewModel::formatProtocolPayload(const QByteArray &payload)
{
    if (payload.isEmpty()) {
        return QString();
    }

    static const char hex[] = "0123456789ABCDEF";
    const int size = payload.size();
    QString result(size * 3 - 1, QLatin1Char(' '));
    QChar *out = result.data();

    for (int i = 0; i < size; ++i) {
        const uint8_t b = static_cast<uint8_t>(payload.at(i));
        if (i > 0) {
            ++out;
        }
        *out++ = QLatin1Char(hex[b >> 4]);
        *out++ = QLatin1Char(hex[b & 0x0F]);
    }

    return result;
}

void AggregatedTraceViewModel::createItem(const PendingMessageEntry &entry)
{
    const BusMessage &msg = entry.msg;
    AggregatedTraceViewItem *item = new AggregatedTraceViewItem(_rootItem);
    item->_lastmsg = msg;
    item->_hasProtocolMsg = entry.hasProtocolMessage;
    if (entry.hasProtocolMessage) {
        item->_lastProtocolMsg = entry.protocolMessage;
    }

    if (msg.busType() == BusType::LIN) {
        LinFrame *linFrame = backend()->findLinFrame(msg);
        if (linFrame) {
            for (int i = 0; i < linFrame->signalList().size(); ++i) {
                item->appendChild(new AggregatedTraceViewItem(item));
            }
        }
    } else {
        CanDbMessage *dbmsg = backend()->findDbMessage(msg);
        if (dbmsg) {
            for (int i = 0; i < dbmsg->getSignals().length(); i++) {
                item->appendChild(new AggregatedTraceViewItem(item));
            }
        }
    }

    _rootItem->appendChild(item);
    _map[makeUniqueKey(msg)] = item;
}

void AggregatedTraceViewModel::updateItem(const PendingMessageEntry &entry)
{
    const BusMessage &msg = entry.msg;
    AggregatedTraceViewItem *item = _map.value(makeUniqueKey(msg));
    if (item) {
        item->_prevmsg = item->_lastmsg;
        item->_lastmsg = msg;
        item->_hasProtocolMsg = entry.hasProtocolMessage;
        if (entry.hasProtocolMessage) {
            item->_lastProtocolMsg = entry.protocolMessage;
        }
    }
}

void AggregatedTraceViewModel::onUpdateModel()
{

    if (!_pendingMessageInserts.isEmpty()) {
        beginInsertRows(QModelIndex(), _rootItem->childCount(), _rootItem->childCount()+_pendingMessageInserts.size()-1);
        for (const auto &entry : _pendingMessageInserts) {
            createItem(entry);
        }
        endInsertRows();
        _pendingMessageInserts.clear();
    }

    if (!_pendingMessageUpdates.isEmpty()) {
        QSet<int> updatedRows;
        for (const auto &entry : _pendingMessageUpdates) {
            AggregatedTraceViewItem *item = _map.value(makeUniqueKey(entry.msg));
            if (item) {
                updateItem(entry);
                updatedRows.insert(item->row());
            }
        }

        for (auto r : updatedRows) {
            AggregatedTraceViewItem *item = _rootItem->child(r);
            if (item) {
                QModelIndex msgIdx = createIndex(r, 0, item);
                emit dataChanged(msgIdx, msgIdx.sibling(r, column_count - 1));

                if (item->childCount() > 0) {
                    QModelIndex firstChild = index(0, 0, msgIdx);
                    QModelIndex lastChild = index(item->childCount() - 1, column_count - 1, msgIdx);
                    emit dataChanged(firstChild, lastChild);
                }
            }
        }
        _pendingMessageUpdates.clear();
    }
}

void AggregatedTraceViewModel::onSetupChanged()
{
    beginResetModel();
    _protocolManager.reset();
    QSettings s;
    _protocolManager.config().enableUds29Bit = s.value("decoder/uds29Bit", true).toBool();
    for (AggregatedTraceViewItem *item : _map.values()) {
        item->removeChildren();
        const BusMessage &lastMsg = item->_lastmsg;
        if (lastMsg.busType() == BusType::LIN) {
            LinFrame *linFrame = backend()->findLinFrame(lastMsg);
            if (linFrame) {
                for (int i = 0; i < linFrame->signalList().size(); ++i) {
                    item->appendChild(new AggregatedTraceViewItem(item));
                }
            }
        } else {
            CanDbMessage *dbmsg = backend()->findDbMessage(lastMsg);
            if (dbmsg) {
                for (int i = 0; i < dbmsg->getSignals().length(); i++) {
                    item->appendChild(new AggregatedTraceViewItem(item));
                }
            }
        }
    }
    endResetModel();
}

void AggregatedTraceViewModel::beforeAppend(int num_messages)
{
    BusTrace *trace = backend()->getTrace();
    int start_id = trace->size();

    for (int i=start_id; i<start_id + num_messages; i++) {
        PendingMessageEntry entry;
        entry.msg = trace->getMessage(i);

        ProtocolMessage pmsg;
        if (_protocolManager.processFrame(entry.msg, pmsg) == DecodeStatus::Completed) {
            entry.hasProtocolMessage = true;
            entry.protocolMessage = pmsg;
        }

        unique_key_t key = makeUniqueKey(entry.msg);
        if (_map.contains(key) || _pendingMessageInserts.contains(key)) {
            _pendingMessageUpdates.append(entry);
        } else {
            _pendingMessageInserts[key] = entry;
        }
    }

    onUpdateModel();
}

void AggregatedTraceViewModel::beforeClear()
{
    beginResetModel();
    delete _rootItem;
    _map.clear();
    _protocolManager.reset();
    _rootItem = new AggregatedTraceViewItem(0);
}

void AggregatedTraceViewModel::afterClear()
{
    endResetModel();
}

AggregatedTraceViewModel::unique_key_t AggregatedTraceViewModel::makeUniqueKey(const BusMessage &msg) const
{
    // Build a stable key that keeps frame-format variants separate so rows don't overwrite each other.
    // [63] RX, [62:47] interface, [46] bus type (LIN), [45] extended, [44] RTR,
    // [43] FD, [42] BRS, [41] LIN sleep, [40] LIN wakeup, [39:29] reserved, [28:0] raw CAN/LIN id.
    return  static_cast<uint64_t>(msg.isRX()) << 63
          | (static_cast<uint64_t>(msg.getInterfaceId()) & 0xFFFFull) << 47
          | static_cast<uint64_t>(msg.busType() == BusType::LIN) << 46
          | static_cast<uint64_t>(msg.isExtended()) << 45
          | static_cast<uint64_t>(msg.isRTR()) << 44
          | static_cast<uint64_t>(msg.isFD()) << 43
          | static_cast<uint64_t>(msg.isBRS()) << 42
          | static_cast<uint64_t>(msg.isLinSleepFrame()) << 41
          | static_cast<uint64_t>(msg.isLinWakeupFrame()) << 40
          | (static_cast<uint64_t>(msg.getRawId()) & 0x1FFFFFFFull);
}

QModelIndex AggregatedTraceViewModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    }

    const AggregatedTraceViewItem *parentItem = parent.isValid() ? static_cast<AggregatedTraceViewItem*>(parent.internalPointer()) : _rootItem;
    const AggregatedTraceViewItem *childItem = parentItem->child(row);

    if (childItem) {
        return createIndex(row, column, const_cast<AggregatedTraceViewItem*>(childItem));
    } else {
        return QModelIndex();
    }
}

QModelIndex AggregatedTraceViewModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return QModelIndex();
    }

    AggregatedTraceViewItem *childItem = static_cast<AggregatedTraceViewItem*>(index.internalPointer());
    AggregatedTraceViewItem *parentItem = childItem->parent();

    if (parentItem == _rootItem) {
        return QModelIndex();
    }

    return createIndex(parentItem->row(), 0, parentItem);
}

int AggregatedTraceViewModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0) {
        return 0;
    }

    AggregatedTraceViewItem *parentItem;
    if (parent.isValid()) {
        parentItem = static_cast<AggregatedTraceViewItem*>(parent.internalPointer());
    } else {
        parentItem = _rootItem;
    }
    return parentItem->childCount();
}

BusMessage AggregatedTraceViewModel::getMessage(const QModelIndex &index) const
{
    if (!index.isValid()) return BusMessage();
    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem*>(index.internalPointer());
    if (item == _rootItem) return BusMessage();
    return (item->parent() == _rootItem) ? item->_lastmsg : item->parent()->_lastmsg;
}

QVariant AggregatedTraceViewModel::data_DisplayRole(const QModelIndex &index, int role) const
{
    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem *>(index.internalPointer());
    if (!item) { return QVariant(); }

    if (index.column() == column_index) {
        return (item->parent() == _rootItem) ? QVariant(static_cast<uint32_t>(index.row() + 1)) : QVariant();
    }

    if (item->parent() == _rootItem) { // BusMessage row
        if (item->_hasProtocolMsg) {
            const ProtocolMessage &pmsg = item->_lastProtocolMsg;
            switch (index.column()) {
                case column_type:
                    if (!pmsg.protocol.isEmpty()) {
                        return pmsg.protocol.toUpper();
                    }
                    break;
                case column_name:
                    if (!pmsg.name.isEmpty()) {
                        return pmsg.name;
                    }
                    break;
                case column_comment:
                    if (!pmsg.description.isEmpty()) {
                        return pmsg.description;
                    }
                    break;
                case column_data:
                {
                    const QString payload = formatProtocolPayload(pmsg.payload);
                    if (!payload.isEmpty()) {
                        return payload;
                    }
                    break;
                }
                case column_sender:
                    if (pmsg.protocol.compare(QStringLiteral("uds"), Qt::CaseInsensitive) == 0) {
                        return (pmsg.type == MessageType::Request) ? tr("Tester") : tr("ECU");
                    }
                    if (pmsg.protocol.compare(QStringLiteral("canopen"), Qt::CaseInsensitive) == 0) {
                        return pmsg.metadata.value(QStringLiteral("Sender")).toString();
                    }
                    break;
                default:
                    break;
            }
        }
        return data_DisplayRole_Message(index, role, item->_lastmsg, item->_prevmsg);
    } else { // CanSignal Row
        return data_DisplayRole_Signal(index, role, item->parent()->_lastmsg);
    }
}

QVariant AggregatedTraceViewModel::data_ChangedBytesRole(const QModelIndex &index) const
{
    if (index.column() != column_data) { return QVariant(); }

    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem *>(index.internalPointer());
    if (!item || item->parent() != _rootItem) { return QVariant(); }

    const BusMessage &cur  = item->_lastmsg;
    const BusMessage &prev = item->_prevmsg;
    if (prev.getLength() == 0) { return QVariant(); }

    uint64_t mask = 0;
    const int len = qMin(cur.getLength(), prev.getLength());
    for (int i = 0; i < len; ++i) {
        if (cur.getData()[i] != prev.getData()[i])
            mask |= (1ULL << i);
    }
    for (int i = len; i < cur.getLength(); ++i)
        mask |= (1ULL << i);

    return QVariant::fromValue(mask);
}

QVariant AggregatedTraceViewModel::data_TextColorRole(const QModelIndex &index, int role) const
{
    (void) role;
    bool isDark = ThemeManager::instance().isDarkMode();

    AggregatedTraceViewItem *item = static_cast<AggregatedTraceViewItem *>(index.internalPointer());
    if (!item) { return QVariant(); }

    const BusMessage &msg = (item->parent() == _rootItem)
        ? item->_lastmsg
        : item->parent()->_lastmsg;

    // Fade stale messages via alpha based on time since last reception.
    // Precondition: message timestamps must be Unix-epoch microseconds so that
    // getTimestamp_ms() is directly comparable to currentMSecsSinceEpoch().
    qint64 now_ms = _fadeNowMs > 0 ? _fadeNowMs : QDateTime::currentMSecsSinceEpoch();
    double diff_sec = (now_ms - msg.getTimestamp_ms()) / 1000.0;

    int alpha = 255 - static_cast<int>(diff_sec * 58);
    alpha = qBound(80, alpha, 255);

    QColor color = msg.isErrorFrame()
        ? (isDark ? QColor(255, 100, 100) : QColor(Qt::red))
        : ThemeManager::instance().colors().text;

    color.setAlpha(alpha);
    return color;
}
