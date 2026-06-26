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

#include "SetupDialogTreeModel.h"

#include "driver/BusInterface.h"
#include "core/DBC/CanOpenDb.h"
#include "core/DBC/LinDb.h"

SetupDialogTreeModel::SetupDialogTreeModel(Backend *backend, QObject *parent)
  : QAbstractItemModel(parent),
    _backend(backend),
    _rootItem(0)
{
}

SetupDialogTreeModel::~SetupDialogTreeModel()
{
    if (_rootItem) {
        delete _rootItem;
    }
}

QVariant SetupDialogTreeModel::data(const QModelIndex &index, int role) const
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());

    if (item) {
        if (role == Qt::DisplayRole) {
            return item->dataDisplayRole(index);
        }

        if (role == Qt::CheckStateRole
            && item->getType() == SetupDialogTreeItem::type_interface
            && index.column() == column_device)
        {
            return item->intf->isEnabled() ? Qt::Checked : Qt::Unchecked;
        }
    }

    return QVariant();
}

bool SetupDialogTreeModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());

    if (item
        && role == Qt::CheckStateRole
        && item->getType() == SetupDialogTreeItem::type_interface
        && index.column() == column_device)
    {
        item->intf->setEnabled(qvariant_cast<Qt::CheckState>(value) == Qt::Checked);
        emit dataChanged(index, index, {Qt::CheckStateRole});
        return true;
    }

    return false;
}

Qt::ItemFlags SetupDialogTreeModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractItemModel::flags(index);

    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    if (item
        && item->getType() == SetupDialogTreeItem::type_interface
        && index.column() == column_device)
    {
        f |= Qt::ItemIsUserCheckable;
    }

    return f;
}

QVariant SetupDialogTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    (void) orientation;

    if (role==Qt::DisplayRole) {
        switch (section) {
        case column_device: return tr("Device");
        case column_driver: return tr("Driver");
        case column_bitrate: return tr("Bitrate");
        case column_filename: return tr("Filename");
        case column_path: return tr("Path");
            default: return "";
        }
    } else {
        return QVariant();
    }
}

QModelIndex SetupDialogTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return QModelIndex();
    } else {
        SetupDialogTreeItem *parentItem = itemOrRoot(parent);
        SetupDialogTreeItem *childItem = parentItem->child(row);
        return childItem ? createIndex(row, column, childItem) : QModelIndex();
    }
}

QModelIndex SetupDialogTreeModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) { return QModelIndex(); }

    SetupDialogTreeItem *childItem = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    SetupDialogTreeItem *parentItem = childItem->getParentItem();

    return (parentItem == _rootItem) ? QModelIndex() : createIndex(parentItem->row(), 0, parentItem);
}

int SetupDialogTreeModel::rowCount(const QModelIndex &parent) const
{
    int retval = 0;
    if (parent.column() <= 0) {
        SetupDialogTreeItem *item = itemOrRoot(parent);
        if (item) {
            retval = item->getChildCount();
        }
    }
    return retval;
}

int SetupDialogTreeModel::columnCount(const QModelIndex &parent) const
{
    (void) parent;
    return column_count;
}

QModelIndex SetupDialogTreeModel::indexOfItem(const SetupDialogTreeItem *item) const
{
    return createIndex(item->row(), 0, (void*)item);
}

SetupDialogTreeItem *SetupDialogTreeModel::addNetwork()
{
    QString s;
    int i=0;
    while (true) {
        s = tr("Network ") + QString("%1").arg(++i);
        if (_rootItem->setup->getNetworkByName(s)==0) {
            break;
        }
    }

    beginInsertRows(QModelIndex(), _rootItem->getChildCount(), _rootItem->getChildCount());
    MeasurementNetwork *network = _rootItem->setup->createNetwork();
    network->setName(s);
    SetupDialogTreeItem *item = loadNetwork(_rootItem, *network);
    endInsertRows();

    return item;
}

void SetupDialogTreeModel::deleteNetwork(const QModelIndex &index)
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    if (!item) return;

    beginRemoveRows(index.parent(), index.row(), index.row());
    _rootItem->removeChild(item);
    _rootItem->setup->removeNetwork(item->network);
    delete item;
    endRemoveRows();
}

SetupDialogTreeItem *SetupDialogTreeModel::addCanDb(const QModelIndex &parent, pCanDb db)
{
    SetupDialogTreeItem *parentItem = static_cast<SetupDialogTreeItem*>(parent.internalPointer());
    if (!parentItem) { return 0; }

    SetupDialogTreeItem *item = 0;
    if (parentItem->network) {
        beginInsertRows(parent, rowCount(parent), rowCount(parent));
        parentItem->network->addCanDb(db);
        item = loadCanDb(*parentItem, db);
        endInsertRows();
    }
    return item;
}

void SetupDialogTreeModel::deleteCanDb(const QModelIndex &index)
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    if (!item) { return; }

    SetupDialogTreeItem *parentItem = item->getParentItem();
    if (parentItem && parentItem->network && parentItem->network->_canDbs.contains(item->candb)) {
        beginRemoveRows(index.parent(), item->row(), item->row());
        parentItem->network->_canDbs.removeAll(item->candb);
        item->getParentItem()->removeChild(item);
        delete item;
        endRemoveRows();
    }
}

SetupDialogTreeItem *SetupDialogTreeModel::addCanOpenDb(const QModelIndex &parent, pCanOpenDb db)
{
    SetupDialogTreeItem *parentItem = static_cast<SetupDialogTreeItem*>(parent.internalPointer());
    if (!parentItem) { return nullptr; }

    SetupDialogTreeItem *item = nullptr;
    if (parentItem->network) {
        beginInsertRows(parent, rowCount(parent), rowCount(parent));
        parentItem->network->addCanOpenDb(db);
        item = loadCanOpenDb(*parentItem, db);
        endInsertRows();
    }
    return item;
}

void SetupDialogTreeModel::deleteCanOpenDb(const QModelIndex &index)
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    if (!item) { return; }

    SetupDialogTreeItem *parentItem = item->getParentItem();
    if (parentItem && parentItem->network && parentItem->network->_canOpenDbs.contains(item->canOpenDb)) {
        beginRemoveRows(index.parent(), item->row(), item->row());
        parentItem->network->_canOpenDbs.removeAll(item->canOpenDb);
        item->getParentItem()->removeChild(item);
        delete item;
        endRemoveRows();
    }
}

SetupDialogTreeItem *SetupDialogTreeModel::addLinDb(const QModelIndex &parent, pLinDb db)
{
    SetupDialogTreeItem *parentItem = static_cast<SetupDialogTreeItem*>(parent.internalPointer());
    if (!parentItem) { return nullptr; }

    SetupDialogTreeItem *item = nullptr;
    if (parentItem->network) {
        beginInsertRows(parent, rowCount(parent), rowCount(parent));
        parentItem->network->addLinDb(db);
        item = loadLinDb(*parentItem, db);
        endInsertRows();
    }
    return item;
}

void SetupDialogTreeModel::deleteLinDb(const QModelIndex &index)
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    if (!item) { return; }

    SetupDialogTreeItem *parentItem = item->getParentItem();
    if (parentItem && parentItem->network && parentItem->network->_linDbs.contains(item->lindb)) {
        beginRemoveRows(index.parent(), item->row(), item->row());
        parentItem->network->_linDbs.removeAll(item->lindb);
        item->getParentItem()->removeChild(item);
        delete item;
        endRemoveRows();
    }
}

SetupDialogTreeItem *SetupDialogTreeModel::addInterface(const QModelIndex &parent, BusInterfaceId &interface)
{
    SetupDialogTreeItem *parentItem = static_cast<SetupDialogTreeItem*>(parent.internalPointer());
    if (!parentItem) { return 0; }

    SetupDialogTreeItem *item = 0;
    if (parentItem && parentItem->network) {
        beginInsertRows(parent, parentItem->getChildCount(), parentItem->getChildCount());
        MeasurementInterface *mi = parentItem->network->addBusInterface(interface);
        // Propagate the actual bus type (e.g. LIN) from the underlying interface
        BusInterface *canIntf = _backend->getInterfaceById(interface);
        if (canIntf)
            mi->setBusType(canIntf->busType());
        item = loadMeasurementInterface(*parentItem, mi);
        endInsertRows();
    }
    return item;
}

void SetupDialogTreeModel::deleteInterface(const QModelIndex &index)
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    if (!item) { return; }

    SetupDialogTreeItem *parentItem = item->getParentItem();
    if (parentItem && parentItem->network && parentItem->network->interfaces().contains(item->intf)) {
        beginRemoveRows(index.parent(), item->row(), item->row());
        parentItem->network->removeInterface(item->intf);
        item->getParentItem()->removeChild(item);
        delete item;
        endRemoveRows();
    }
}

SetupDialogTreeItem *SetupDialogTreeModel::itemOrRoot(const QModelIndex &index) const
{
    return index.isValid() ? static_cast<SetupDialogTreeItem*>(index.internalPointer()) : _rootItem;
}

SetupDialogTreeItem *SetupDialogTreeModel::loadMeasurementInterface(SetupDialogTreeItem &parent, MeasurementInterface *intf)
{
    SetupDialogTreeItem *item = new SetupDialogTreeItem(SetupDialogTreeItem::type_interface, _backend, &parent);
    item->network = parent.network;
    item->intf = intf;
    parent.appendChild(item);
    return item;
}

SetupDialogTreeItem *SetupDialogTreeModel::loadCanDb(SetupDialogTreeItem &parent, const pCanDb &db)
{
    SetupDialogTreeItem *item = new SetupDialogTreeItem(SetupDialogTreeItem::type_candb, _backend, &parent);
    item->candb = db;
    parent.appendChild(item);
    return item;
}

SetupDialogTreeItem *SetupDialogTreeModel::loadCanOpenDb(SetupDialogTreeItem &parent, const pCanOpenDb &db)
{
    SetupDialogTreeItem *item = new SetupDialogTreeItem(SetupDialogTreeItem::type_canopendb, _backend, &parent);
    item->canOpenDb = db;
    parent.appendChild(item);
    return item;
}

SetupDialogTreeItem *SetupDialogTreeModel::loadLinDb(SetupDialogTreeItem &parent, const pLinDb &db)
{
    SetupDialogTreeItem *item = new SetupDialogTreeItem(SetupDialogTreeItem::type_lindb, _backend, &parent);
    item->lindb = db;
    parent.appendChild(item);
    return item;
}

SetupDialogTreeItem *SetupDialogTreeModel::loadNetwork(SetupDialogTreeItem *root, MeasurementNetwork &network)
{
    SetupDialogTreeItem *item_network = new SetupDialogTreeItem(SetupDialogTreeItem::type_network, _backend, root);
    item_network->network = &network;

    SetupDialogTreeItem *item_intf_root = new SetupDialogTreeItem(SetupDialogTreeItem::type_interface_root, _backend, item_network);
    item_intf_root->network = &network;
    item_network->appendChild(item_intf_root);

    SetupDialogTreeItem *item_candb_root = new SetupDialogTreeItem(SetupDialogTreeItem::type_candb_root, _backend, item_network);
    item_candb_root->network = &network;
    item_network->appendChild(item_candb_root);

    for (auto *intf : network.interfaces()) {
        loadMeasurementInterface(*item_intf_root, intf);
    }

    for (const auto &candb : network._canDbs) {
        loadCanDb(*item_candb_root, candb);
    }

    for (const auto &canOpenDb : network._canOpenDbs) {
        loadCanOpenDb(*item_candb_root, canOpenDb);
    }

    for (const auto &lindb : network._linDbs) {
        loadLinDb(*item_candb_root, lindb);
    }

    root->appendChild(item_network);
    return item_network;
}

void SetupDialogTreeModel::load(MeasurementSetup &setup)
{
    SetupDialogTreeItem *_newRoot = new SetupDialogTreeItem(SetupDialogTreeItem::type_root, 0);
    _newRoot->setup = &setup;

    for (auto *network : setup.getNetworks()) {
        loadNetwork(_newRoot, *network);
    }

    beginResetModel();
    SetupDialogTreeItem *_oldRoot = _rootItem;
    _rootItem = _newRoot;
    delete _oldRoot;
    endResetModel();
}

void SetupDialogTreeModel::unload()
{
    beginResetModel();
    if (_rootItem) {
        delete _rootItem;
        _rootItem = 0;
    }
    endResetModel();
}

