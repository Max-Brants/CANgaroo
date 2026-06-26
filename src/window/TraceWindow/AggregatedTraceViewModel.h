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

#include <QAbstractItemModel>
#include <QMap>
#include <QList>
#include <QTimer>
#include <sys/time.h>

#include "BaseTraceViewModel.h"
#include "core/BusMessage.h"
#include "driver/BusInterface.h"
#include "decoders/ProtocolManager.h"

#include "AggregatedTraceViewItem.h"


class BusTrace;

class AggregatedTraceViewModel : public BaseTraceViewModel
{
    Q_OBJECT

public:
    using unique_key_t = uint64_t;
    using CanIdMap = QMap<unique_key_t, AggregatedTraceViewItem*>;

public:
    AggregatedTraceViewModel(Backend &backend);

    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent) const override;

    BusMessage getMessage(const QModelIndex &index) const override;

private:
    struct PendingMessageEntry {
        BusMessage msg;
        bool hasProtocolMessage = false;
        DecodeStatus decodeStatus = DecodeStatus::Ignored;
        ProtocolMessage protocolMessage;
    };

    CanIdMap _map;
    AggregatedTraceViewItem *_rootItem;
    QTimer _fadeTimer;
    qint64 _fadeNowMs = 0;
    QList<PendingMessageEntry> _pendingMessageUpdates;
    QMap<unique_key_t, PendingMessageEntry> _pendingMessageInserts;
    ProtocolManager _protocolManager;

    unique_key_t makeUniqueKey(const BusMessage &msg) const;
    void createItem(const PendingMessageEntry &entry);
    void updateItem(const PendingMessageEntry &entry);
    static QString formatProtocolPayload(const QByteArray &payload);
protected:
    QVariant data_DisplayRole(const QModelIndex &index, int role) const override;
    QVariant data_TextColorRole(const QModelIndex &index, int role) const override;
    QVariant data_ChangedBytesRole(const QModelIndex &index) const override;

private slots:
    void onUpdateModel();
    void onSetupChanged();

    void beforeAppend(int num_messages);
    void beforeClear();
    void afterClear();
};
