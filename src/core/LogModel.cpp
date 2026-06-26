/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>
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


#include "LogModel.h"

LogModel::LogModel(Backend &backend)
{
    connect(&backend, &Backend::onLogMessage, this, &LogModel::onLogMessage);
}

LogModel::~LogModel()
{
    _items.clear();
}

void LogModel::clear()
{
    beginResetModel();
    _items.clear();
    _items.reserve(MaxLogItems);
    endResetModel();
}

QModelIndex LogModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return QModelIndex();
    } else {
        return createIndex(row, column, (quintptr)0);
    }
}

QModelIndex LogModel::parent(const QModelIndex &child) const
{
    (void) child;
    return QModelIndex();
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    (void) parent;
    return _items.size();
}

int LogModel::columnCount(const QModelIndex &parent) const
{
    (void) parent;
    return column_count;
}

bool LogModel::hasChildren(const QModelIndex &parent) const
{
    return !parent.isValid();
}

QVariant LogModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole) {

        if (orientation == Qt::Horizontal) {
            switch (section) {
                case column_time:
                    return QString(tr("Time"));
                case column_level:
                    return QString(tr("Level"));
                case column_text:
                    return QString(tr("Message"));
            }
        }

    }
    else if (role == Qt::TextAlignmentRole) {
        switch (section) {
        case column_time:
            return static_cast<int>(Qt::AlignRight) + static_cast<int>(Qt::AlignVCenter);
        case column_level:
            return static_cast<int>(Qt::AlignCenter) + static_cast<int>(Qt::AlignVCenter);
        case column_text:
            return static_cast<int>(Qt::AlignLeft) + static_cast<int>(Qt::AlignVCenter);
        default:
            return QVariant();
        }
    }

    return QVariant();
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case column_time:
            return static_cast<int>(Qt::AlignRight) + static_cast<int>(Qt::AlignVCenter);
        case column_level:
            return static_cast<int>(Qt::AlignCenter) + static_cast<int>(Qt::AlignVCenter);
        case column_text:
            return static_cast<int>(Qt::AlignLeft) + static_cast<int>(Qt::AlignVCenter);
        default:
            return QVariant();
        }
    }

    if (role == Qt::DisplayRole) {
        if (!index.isValid()) {
            return QVariant();
        }

        if (index.row() < 0 || index.row() >= _items.size()) {
            return QVariant();
        }
        const LogItem &item = _items.at(index.row());
        switch (index.column()) {
            case column_time:
                return item.timeStr;
            case column_level:
                return logLevelText(item.level);
            case column_text:
                return item.text;
            default:
                return QVariant();
        }
    }

    if(role == Qt::ToolTipRole) {
        if (index.row() < 0 || index.row() >= _items.size()) {
            return QVariant();
        }
        const LogItem &item = _items.at(index.row());
        const QString &src = (index.column() == column_text) ? item.text :
                             (index.column() == column_time) ? item.timeStr :
                             logLevelText(item.level);
        if (src.length() <= 30) { return src; }
        QString wrapped = src;
        for (int i = 30; i < wrapped.length(); i += 31) {
            wrapped.insert(i, '\n');
        }
        return wrapped;
    }
    return QVariant();
}

void LogModel::onLogMessage(const QDateTime dt, const log_level_t level, const QString msg)
{
    // Trim oldest entries if at capacity
    if (_items.size() >= MaxLogItems) {
        int removeCount = MaxLogItems / 5; // remove 20% at once to avoid frequent trimming
        beginRemoveRows(QModelIndex(), 0, removeCount - 1);
        _items.erase(_items.begin(), _items.begin() + removeCount);
        endRemoveRows();
    }

    LogItem item;
    item.timeStr = dt.toString("hh:mm:ss");
    item.level = level;
    item.text = msg;

    beginInsertRows(QModelIndex(), _items.size(), _items.size());
    _items.append(std::move(item));
    endInsertRows();
}

QString LogModel::logLevelText(log_level_t level)
{
    switch (level) {
        case log_level_debug: return tr("debug");
        case log_level_info: return tr("info");
        case log_level_warning: return tr("warning");
        case log_level_error: return tr("error");
        case log_level_critical: return tr("critical");
        case log_level_fatal: return tr("fatal");
        default: return "";
    }
}

