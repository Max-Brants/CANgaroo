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

#pragma once

#include <QAbstractItemModel>
#include <QDateTime>
#include <QList>
#include "core/Backend.h"

class LogItem {
public:
    QString timeStr;
    QString text;
    log_level_t level;
};

class LogModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum {
        column_time,
        column_level,
        column_text,
        column_count
    };

public:
    LogModel(Backend &backend);
    ~LogModel() override;

  void clear();

    QModelIndex index(int row, int column, const QModelIndex &parent) const override;
    QModelIndex parent(const QModelIndex &child) const override;

    int rowCount(const QModelIndex &parent) const override;
    int columnCount(const QModelIndex &parent) const override;
    bool hasChildren(const QModelIndex &parent) const override;

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex &index, int role) const override;

public slots:
    void onLogMessage(const QDateTime dt, const log_level_t level, const QString msg);

private:
    static const int MaxLogItems = 5000;
    QList<LogItem> _items;

    static QString logLevelText(log_level_t level);
};
