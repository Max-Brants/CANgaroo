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

#ifndef TRACEFILTER_H
#define TRACEFILTER_H

#include <QSet>
#include <QSortFilterProxyModel>
#include <QRegularExpression>

#include "driver/CanDriver.h"

class TraceFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit TraceFilterModel(QObject *parent = nullptr);

    void setShowTx(bool show);
    void setShowRx(bool show);
    void setHiddenMessageIds(const QSet<uint32_t> &ids);
    void setHiddenLinFrameIds(const QSet<uint8_t> &ids);
    void setHiddenInterfaces(const QSet<BusInterfaceId> &ids);

public slots:
    void setFilterText(QString filtertext);

private:
    QString _filterText;
    QRegularExpression _cachedRegex;
    bool _regexValid = false;

    bool _showTx = true;
    bool _showRx = true;
    QSet<uint32_t> _hiddenMessageIds;
    QSet<uint8_t>  _hiddenLinFrameIds;
    QSet<BusInterfaceId> _hiddenInterfaces;

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;
};

#endif // TRACEFILTER_H
