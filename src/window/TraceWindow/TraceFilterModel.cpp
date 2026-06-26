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

#include "TraceFilterModel.h"
#include "BaseTraceViewModel.h"

#include <QRegularExpression>
#include <QSortFilterProxyModel>

#include "core/BusMessage.h"

TraceFilterModel::TraceFilterModel(QObject *parent)
    : QSortFilterProxyModel{parent},
    _filterText("")
{
   setRecursiveFilteringEnabled(false);
   setDynamicSortFilter(false);
}


void TraceFilterModel::setFilterText(QString filtertext)
{
    _filterText = filtertext;
    QRegularExpression re(filtertext, QRegularExpression::CaseInsensitiveOption);
    _regexValid = re.isValid();
    if (_regexValid)
    {
        _cachedRegex = re;
    }
}

void TraceFilterModel::setShowTx(bool show)
{
    _showTx = show;
}

void TraceFilterModel::setShowRx(bool show)
{
    _showRx = show;
}

void TraceFilterModel::setHiddenMessageIds(const QSet<uint32_t> &ids)
{
    _hiddenMessageIds = ids;
}

void TraceFilterModel::setHiddenLinFrameIds(const QSet<uint8_t> &ids)
{
    _hiddenLinFrameIds = ids;
}

void TraceFilterModel::setHiddenInterfaces(const QSet<BusInterfaceId> &ids)
{
    _hiddenInterfaces = ids;
}

bool TraceFilterModel::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const
{
    // Get the underlying BaseTraceViewModel to access the BusMessage directly
    auto *baseModel = qobject_cast<BaseTraceViewModel *>(sourceModel());
    QSortFilterProxyModel *proxySource = nullptr;
    if (!baseModel)
    {
        proxySource = qobject_cast<QSortFilterProxyModel *>(sourceModel());
        if (proxySource)
        {
            baseModel = qobject_cast<BaseTraceViewModel *>(proxySource->sourceModel());
        }
    }

    if (baseModel)
    {
        QModelIndex baseIdx;
        if (proxySource)
        {
            QModelIndex proxyIdx = proxySource->index(source_row, 0, source_parent);
            baseIdx = proxySource->mapToSource(proxyIdx);
        }
        else
        {
            baseIdx = sourceModel()->index(source_row, 0, source_parent);
        }
        BusMessage msg = baseModel->getMessage(baseIdx);

        if (!_showTx && !msg.isRX())
        {
            return false;
        }
        if (!_showRx && msg.isRX())
        {
            return false;
        }
        if (msg.busType() == BusType::LIN)
        {
            if (_hiddenLinFrameIds.contains(static_cast<uint8_t>(msg.getId())))
            {
                return false;
            }
        }
        else if (_hiddenMessageIds.contains(msg.getId()))
        {
            return false;
        }
        if (_hiddenInterfaces.contains(msg.getInterfaceId()))
        {
            return false;
        }
    }

    // Text/regex filter
    if (_filterText.length() == 0)
    {
        return true;
    }

    static const int columns[] = {
        BaseTraceViewModel::column_canid,
        BaseTraceViewModel::column_name,
        BaseTraceViewModel::column_channel,
        BaseTraceViewModel::column_sender,
        BaseTraceViewModel::column_type,
    };

    for (int col : columns)
    {
        QModelIndex idx = sourceModel()->index(source_row, col, source_parent);
        QString datastr = sourceModel()->data(idx).toString();

        if (_regexValid)
        {
            if (datastr.contains(_cachedRegex))
            {
                return true;
            }
        }
        else
        {
            if (datastr.contains(_filterText, Qt::CaseInsensitive))
            {
                return true;
            }
        }
    }

    return false;
}
