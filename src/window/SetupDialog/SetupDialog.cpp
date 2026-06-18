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

#include "SetupDialog.h"
#include "ui_SetupDialog.h"
#include <QItemSelectionModel>
#include <QMenu>
#include <QFileDialog>
#include <QTreeWidget>
#include <QMessageBox>

#include "core/Backend.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementInterface.h"
#include "core/DBC/LinDb.h"
#include "driver/BusInterface.h"
#include "driver/CanDriver.h"

#include "SetupDialogTreeModel.h"
#include "SetupDialogTreeItem.h"

#include "SelectCanInterfacesDialog.h"

SetupDialog::SetupDialog(Backend &backend, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SetupDialog),
    _backend(&backend),
    _isReflashNetworks(false),
    _currentNetwork(0)
{
    ui->setupUi(this);

    QIcon icon(":/assets/cangaroo.png");
    setWindowIcon(icon);

    _actionAddInterface = new QAction("Add...", this);
    _actionDeleteInterface = new QAction("Delete", this);
    _actionAddCanDb = new QAction("Add...", this);
    _actionDeleteCanDb = new QAction("Delete", this);
    _actionDeleteLinDb = new QAction("Delete", this);
    _actionReloadCanDbs = new QAction("Reload", this);

    model = new SetupDialogTreeModel(_backend, this);

    ui->treeView->setModel(model);
    ui->interfacesTreeView->setModel(model);
    ui->candbsTreeView->setModel(model);

    for (int i=0; i<model->columnCount(); i++) {
        ui->treeView->setColumnHidden(i, true);
        ui->interfacesTreeView->setColumnHidden(i, true);
        ui->candbsTreeView->setColumnHidden(i, true);
    }

    ui->treeView->setColumnHidden(SetupDialogTreeModel::column_device, false);

    ui->interfacesTreeView->setColumnHidden(SetupDialogTreeModel::column_device, false);
    ui->interfacesTreeView->setColumnHidden(SetupDialogTreeModel::column_driver, false);
    ui->interfacesTreeView->setColumnHidden(SetupDialogTreeModel::column_bitrate, false);

    ui->candbsTreeView->setColumnHidden(SetupDialogTreeModel::column_filename, false);
    ui->candbsTreeView->setColumnHidden(SetupDialogTreeModel::column_path, false);

    connect(ui->treeView, &QTreeView::customContextMenuRequested, this, &SetupDialog::treeViewContextMenu);
    connect(ui->edNetworkName, &QLineEdit::textChanged, this, &SetupDialog::edNetworkNameChanged);

    connect(ui->treeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &SetupDialog::treeViewSelectionChanged);
    connect(ui->candbsTreeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { updateButtons(); });
    connect(ui->interfacesTreeView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { updateButtons(); });

    connect(ui->btReloadDatabases, &QPushButton::released, this, &SetupDialog::executeReloadCanDbs);
    connect(ui->btRefreshNetworks, &QPushButton::released, this, &SetupDialog::on_btRefreshNetworks_clicked);

    connect(_actionAddCanDb, &QAction::triggered, this, &SetupDialog::executeAddCanDb);
    connect(_actionDeleteCanDb, &QAction::triggered, this, &SetupDialog::executeDeleteCanDb);
    connect(_actionDeleteLinDb, &QAction::triggered, this, &SetupDialog::executeDeleteLinDb);

    connect(_actionAddInterface, &QAction::triggered, this, &SetupDialog::executeAddInterface);
    connect(_actionDeleteInterface, &QAction::triggered, this, &SetupDialog::executeDeleteInterface);


    emit backend.onSetupDialogCreated(*this);
}

SetupDialog::~SetupDialog()
{
    delete ui;
    delete model;
}

void SetupDialog::addPage(QWidget *widget)
{
    ui->stackedWidget->addWidget(widget);
}

void SetupDialog::displayPage(QWidget *widget)
{
    ui->stackedWidget->setCurrentWidget(widget);
}

bool SetupDialog::showSetupDialog(MeasurementSetup &setup)
{
    _isReflashNetworks = false;

    model->load(setup);
    ui->treeView->expandAll();

    QModelIndex first = model->index(0, 0, QModelIndex());
    ui->treeView->setCurrentIndex(first);

    updateButtons();
    bool result = exec()==QDialog::Accepted;
    model->unload();
    return result;
}

void SetupDialog::treeViewSelectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    (void) selected;
    (void) deselected;

    _currentNetwork = 0;

    if (selected.isEmpty()) {
        ui->stackedWidget->setCurrentWidget(ui->emptyPage);
        updateButtons();
        return;
    }

    QModelIndex idx = selected.first().topLeft();
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem *>(idx.internalPointer());

    if (!item) {
        ui->stackedWidget->setCurrentWidget(ui->emptyPage);
        updateButtons();
        return;
    }

    _currentNetwork = item->network;

    if (item->network) {
        ui->edNetworkName->setText(item->network->name());
    }

    if (item) {
        switch (item->getType()) {

            case SetupDialogTreeItem::type_network:
                ui->stackedWidget->setCurrentWidget(ui->networkPage);
                break;

            case SetupDialogTreeItem::type_interface_root:
                ui->stackedWidget->setCurrentWidget(ui->interfacesPage);
                ui->interfacesTreeView->setRootIndex(getSelectedIndex());
                break;

            case SetupDialogTreeItem::type_interface:
                emit onShowInterfacePage(*this, item->intf);
                break;

            case SetupDialogTreeItem::type_candb_root:
                ui->stackedWidget->setCurrentWidget(ui->candbsPage);
                ui->candbsTreeView->setRootIndex(getSelectedIndex());
                break;

            default:
                ui->stackedWidget->setCurrentWidget(ui->emptyPage);
                break;
        }
    }

    updateButtons();
}

QModelIndex SetupDialog::getSelectedIndex()
{
    QModelIndexList list = ui->treeView->selectionModel()->selectedRows();
    if (list.isEmpty()) {
        return QModelIndex();
    } else {
        return list.first();
    }
}

SetupDialogTreeItem *SetupDialog::getSelectedItem()
{
    const QModelIndex index = getSelectedIndex();
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem *>(index.internalPointer());
    return item;
}

void SetupDialog::treeViewContextMenu(const QPoint &pos)
{
    (void) pos;

    QMenu contextMenu;

    SetupDialogTreeItem *item = getSelectedItem();
    if (item) {
        switch (item->getType()) {
            case SetupDialogTreeItem::type_interface_root:
                contextMenu.addAction(_actionAddInterface);
                break;
            case SetupDialogTreeItem::type_interface:
                contextMenu.addAction(_actionDeleteInterface);
                break;
            case SetupDialogTreeItem::type_candb_root:
                contextMenu.addAction(_actionAddCanDb);
                break;
            case SetupDialogTreeItem::type_candb:
                contextMenu.addAction(_actionDeleteCanDb);
                contextMenu.addAction(_actionReloadCanDbs);
                break;
            case SetupDialogTreeItem::type_lindb:
                contextMenu.addAction(_actionDeleteLinDb);
                break;
            default:
                break;
        }
    }


    QPoint globalPos = ui->treeView->mapToGlobal(pos);
    contextMenu.exec(globalPos);
}

void SetupDialog::edNetworkNameChanged()
{
    if (_currentNetwork) {
        _currentNetwork->setName(ui->edNetworkName->text());
        model->dataChanged(getSelectedIndex(), getSelectedIndex());
    }
}

void SetupDialog::addInterface(const QModelIndex &parent)
{
    SelectCanInterfacesDialog dlg(0);
    BusInterfaceIdList list;
    if (dlg.selectInterfaces(*_backend, list, _currentNetwork->getReferencedBusInterfaces())) {
        for (auto intf : list) {
            model->addInterface(parent, intf);
        }
    }

}

void SetupDialog::executeAddInterface()
{
    addInterface(ui->treeView->selectionModel()->currentIndex());
}

void SetupDialog::on_btAddInterface_clicked()
{
    addInterface(ui->treeView->selectionModel()->currentIndex());
}

void SetupDialog::executeDeleteInterface()
{
    model->deleteInterface(ui->treeView->selectionModel()->currentIndex());
}

void SetupDialog::on_btRemoveInterface_clicked()
{
    model->deleteInterface(ui->interfacesTreeView->selectionModel()->currentIndex());
}

void SetupDialog::reloadCanDbs(const QModelIndex &index)
{
    SetupDialogTreeItem *item = static_cast<SetupDialogTreeItem*>(index.internalPointer());
    if (!item || !item->network) return;

    QStringList errors;
    if (!item->network->reloadCanDbs(_backend, &errors)) {
        QMessageBox::warning(this, tr("Database Error"),
            tr("Failed to reload one or more database files:\n\n%1").arg(errors.join("\n")));
    }

    // Synchronize stale pointers in tree items
    SetupDialogTreeItem *root = (item->getType() == SetupDialogTreeItem::type_candb_root) ? item : item->getParentItem();
    if (root && root->getType() == SetupDialogTreeItem::type_candb_root) {
        for (int i=0; i < root->getChildCount(); ++i) {
            SetupDialogTreeItem *child = root->child(i);
            if (child->getType() == SetupDialogTreeItem::type_candb) {
                 // Find the updated pCanDb in the network by path
                 for (const auto &updatedDb : root->network->_canDbs) {
                     if (updatedDb->getPath() == child->candb->getPath()) {
                         child->candb = updatedDb;
                         break;
                     }
                 }
            }
        }
    }

    model->dataChanged(index, index);
}

void SetupDialog::executeReloadCanDbs()
{
    reloadCanDbs(ui->treeView->selectionModel()->currentIndex());
}

void SetupDialog::addCanDb(const QModelIndex &parent, const QString &filename)
{
    // Check for duplicates
    if (_currentNetwork) {
        for (const auto &existingDb : _currentNetwork->_canDbs) {
            if (existingDb->getPath() == filename) {
                QMessageBox::StandardButton reply;
                reply = QMessageBox::question(this, tr("Duplicate Database"),
                    tr("The file is already loaded:\n%1\n\nDo you want to reload it?").arg(filename),
                    QMessageBox::Yes | QMessageBox::No);

                if (reply == QMessageBox::Yes) {
                    // Find the index of the existing database in the model to remove it first
                    SetupDialogTreeItem *root = static_cast<SetupDialogTreeItem*>(parent.internalPointer());
                    if (root && root->getType() == SetupDialogTreeItem::type_candb_root) {
                        for (int i=0; i < root->getChildCount(); ++i) {
                            SetupDialogTreeItem *child = root->child(i);
                            if (child->getType() == SetupDialogTreeItem::type_candb && child->candb->getPath() == filename) {
                                model->deleteCanDb(model->indexOfItem(child));
                                break;
                            }
                        }
                    }

                    // Now load fresh
                    QString errorMsg;
                    pCanDb candb = _backend->loadCanDbFile(filename, &errorMsg);
                    if (candb) {
                        model->addCanDb(parent, candb);
                    } else {
                        QMessageBox::critical(this, tr("Reload Failed"),
                            tr("Failed to reload database:\n%1\n\nReason: %2").arg(filename, errorMsg));
                    }
                }
                return;
            }
        }
    }

    QString errorMsg;
    pCanDb candb = _backend->loadCanDbFile(filename, &errorMsg);
    if (candb) {
        model->addCanDb(parent, candb);
        if (!errorMsg.isEmpty()) {
            QMessageBox::warning(this, tr("Database Warning"), errorMsg);
        }
    } else {
        QMessageBox::critical(this, tr("Database Error"),
            tr("Failed to load CAN database file:\n%1\n\nReason: %2").arg(filename, errorMsg));
    }
}

void SetupDialog::on_btAddDatabase_clicked()
{
    // Detect bus type from the network's interfaces
    bool isLin = false;
    if (_currentNetwork) {
        for (auto *mi : _currentNetwork->interfaces()) {
            if (mi->busType() == BusType::LIN) {
                isLin = true;
                break;
            }
        }
    }

    QModelIndex parent = ui->treeView->selectionModel()->currentIndex();

    if (isLin) {
        QStringList files = QFileDialog::getOpenFileNames(this, "Load LIN Databases", "", "LIN Description Files (*.ldf)");
        for (const QString &filename : files) {
            addLinDb(parent, filename);
        }
    } else {
        QStringList files = QFileDialog::getOpenFileNames(this, "Load CAN Databases", "",
            "CAN Databases (*.dbc *.eds *.dcf);;Vector DBC Files (*.dbc);;CANopen EDS/DCF Files (*.eds *.dcf)");
        for (const QString &filename : files) {
            addCanDb(parent, filename);
        }
    }
}

void SetupDialog::addLinDb(const QModelIndex &parent, const QString &filename)
{
    // After adding an LDF, auto-configure any LIN interfaces in the network
    // that have no LDF path set yet — avoids requiring the user to click each
    // interface item before starting measurement.
    auto applyLdfToUnassignedInterfaces = [this](const pLinDb &lindb)
    {
        if (!_currentNetwork)
            return;

        static const QHash<QString, LinProtocolVersion> versionMap = {
            {QStringLiteral("1.3"),  LinProtocolVersion::V1_3},
            {QStringLiteral("2.0"),  LinProtocolVersion::V2_0},
            {QStringLiteral("2.1"),  LinProtocolVersion::V2_1},
            {QStringLiteral("2.2"),  LinProtocolVersion::V2_2},
            {QStringLiteral("2.2A"), LinProtocolVersion::V2_2A},
        };

        for (MeasurementInterface *intf : _currentNetwork->interfaces())
        {
            if (intf->busType() != BusType::LIN || !intf->linLdfPath().isEmpty())
                continue;

            intf->setLinLdfPath(lindb->path());
            intf->setLinBaudRate(static_cast<unsigned>(lindb->speedBps()));
            intf->setLinTimebaseMs(static_cast<uint8_t>(qBound(0.0, lindb->masterTimebaseMs(), 255.0)));
            intf->setLinJitterUs(static_cast<uint16_t>(qBound(0.0, lindb->masterJitterMs() * 1000.0, 65535.0)));
            if (const auto it = versionMap.constFind(lindb->protocolVersion()); it != versionMap.constEnd())
                intf->setLinProtocolVersion(it.value());
        }
    };

    if (_currentNetwork) {
        for (const auto &existingDb : _currentNetwork->_linDbs) {
            if (existingDb->path() == filename) {
                QMessageBox::StandardButton reply =
                    QMessageBox::question(this, tr("Duplicate LDF"),
                        tr("The file is already loaded:\n%1\n\nDo you want to reload it?").arg(filename),
                        QMessageBox::Yes | QMessageBox::No);

                if (reply == QMessageBox::Yes) {
                    SetupDialogTreeItem *root = static_cast<SetupDialogTreeItem*>(parent.internalPointer());
                    if (root && root->getType() == SetupDialogTreeItem::type_candb_root) {
                        for (int i = 0; i < root->getChildCount(); ++i) {
                            SetupDialogTreeItem *child = root->child(i);
                            if (child->getType() == SetupDialogTreeItem::type_lindb && child->lindb->path() == filename) {
                                model->deleteLinDb(model->indexOfItem(child));
                                break;
                            }
                        }
                    }

                    QString errorMsg;
                    pLinDb lindb = _backend->loadLdf(filename, &errorMsg);
                    if (lindb) {
                        model->addLinDb(parent, lindb);
                        applyLdfToUnassignedInterfaces(lindb);
                    } else {
                        QMessageBox::critical(this, tr("Reload Failed"),
                            tr("Failed to reload LDF:\n%1\n\nReason: %2").arg(filename, errorMsg));
                    }
                }
                return;
            }
        }
    }

    QString errorMsg;
    pLinDb lindb = _backend->loadLdf(filename, &errorMsg);
    if (lindb) {
        model->addLinDb(parent, lindb);
        applyLdfToUnassignedInterfaces(lindb);
        if (!errorMsg.isEmpty()) {
            QMessageBox::warning(this, tr("LDF Warning"), errorMsg);
        }
    } else {
        QMessageBox::critical(this, tr("LDF Error"),
            tr("Failed to load LDF file:\n%1\n\nReason: %2").arg(filename, errorMsg));
    }
}

void SetupDialog::executeAddCanDb()
{
    on_btAddDatabase_clicked();
}

void SetupDialog::executeDeleteCanDb()
{
    model->deleteCanDb(getSelectedIndex());
}

void SetupDialog::executeDeleteLinDb()
{
    model->deleteLinDb(getSelectedIndex());
}

void SetupDialog::on_btRemoveDatabase_clicked()
{
    model->deleteCanDb(ui->candbsTreeView->selectionModel()->currentIndex());
}

void SetupDialog::updateButtons()
{
    ui->btRemoveDatabase->setEnabled(ui->candbsTreeView->selectionModel()->hasSelection());

//    ui->btReloadDatabases->setEnabled(ui->candbsTreeView->children.count() > 0);

    ui->btRemoveInterface->setEnabled(ui->interfacesTreeView->selectionModel()->hasSelection());

    SetupDialogTreeItem *item = getSelectedItem();
    ui->btRemoveNetwork->setEnabled(ui->treeView->selectionModel()->hasSelection() && item && (item->getType()==SetupDialogTreeItem::type_network));
}

void SetupDialog::on_btAddNetwork_clicked()
{
    QModelIndex idx = model->indexOfItem(model->addNetwork());
    ui->treeView->expand(idx);
    ui->treeView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
}

void SetupDialog::on_btRemoveNetwork_clicked()
{
    model->deleteNetwork(getSelectedIndex());
}

void SetupDialog::on_btRefreshNetworks_clicked()
{
    _backend->setDefaultSetup();
    model->load(_backend->getSetup());
    ui->treeView->expandAll();
    updateButtons();
    _isReflashNetworks = true;
}

bool SetupDialog::isReflashNetworks()
{
    return _isReflashNetworks;
}

