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

#include "SetupDialogTreeItem.h"

#include <QAbstractItemModel>
#include "core/Backend.h"
#include "core/MeasurementSetup.h"

class SetupDialogTreeModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum {
        column_device,
        column_driver,
        column_bitrate,
        column_filename,
        column_path,
        column_count
    };

public:
    explicit SetupDialogTreeModel(Backend *backend, QObject *parent=0);
    virtual ~SetupDialogTreeModel();

    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QModelIndex indexOfItem(const SetupDialogTreeItem *item) const;

    void load(MeasurementSetup &setup);
    void unload();

    SetupDialogTreeItem *addNetwork();
    void deleteNetwork(const QModelIndex &index);
    int getNetworkCount();

    SetupDialogTreeItem *addCanDb(const QModelIndex &parent, pCanDb db);
    void deleteCanDb(const QModelIndex &index);

    SetupDialogTreeItem *addLinDb(const QModelIndex &parent, pLinDb db);
    void deleteLinDb(const QModelIndex &index);

    SetupDialogTreeItem *addCanOpenDb(const QModelIndex &parent, pCanOpenDb db);
    void deleteCanOpenDb(const QModelIndex &index);

    SetupDialogTreeItem *addInterface(const QModelIndex &parent, BusInterfaceId &interface);
    void deleteInterface(const QModelIndex &index);

private:
    Backend *_backend;
    SetupDialogTreeItem *_rootItem;
    SetupDialogTreeItem *itemOrRoot(const QModelIndex &index) const;

    SetupDialogTreeItem *loadNetwork(SetupDialogTreeItem *root, MeasurementNetwork &network);
    SetupDialogTreeItem *loadMeasurementInterface(SetupDialogTreeItem &parent, MeasurementInterface *intf);
    SetupDialogTreeItem *loadCanDb(SetupDialogTreeItem &parent, const pCanDb &db);
    SetupDialogTreeItem *loadCanOpenDb(SetupDialogTreeItem &parent, const pCanOpenDb &db);
    SetupDialogTreeItem *loadLinDb(SetupDialogTreeItem &parent, const pLinDb &db);
};
