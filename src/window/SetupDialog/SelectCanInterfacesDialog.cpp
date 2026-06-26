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

#include "SelectCanInterfacesDialog.h"
#include "ui_SelectCanInterfacesDialog.h"
#include "core/Backend.h"

SelectCanInterfacesDialog::SelectCanInterfacesDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::SelectCanInterfacesDialog)
{
    ui->setupUi(this);
    ui->treeWidget->setHeaderLabels(QStringList() << "Device" << "Driver" << "Description");
}

SelectCanInterfacesDialog::~SelectCanInterfacesDialog()
{
    delete ui;
}

bool SelectCanInterfacesDialog::selectInterfaces(Backend &backend, BusInterfaceIdList &selectedInterfaces, const BusInterfaceIdList &excludeInterfaces)
{
    ui->treeWidget->clear();

    BusInterfaceIdList allInterfaces;
    for (auto intf : backend.getInterfaceList()) {
        if (!excludeInterfaces.contains(intf)) {
            allInterfaces.append(intf);
        }
    }


    for (auto intf : allInterfaces) {
        QTreeWidgetItem *treeItem = new QTreeWidgetItem(ui->treeWidget);
        treeItem->setText(0, backend.getInterfaceName(intf));
        treeItem->setText(1, backend.getDriverName(intf));
        treeItem->setText(2, "");
    }

    if (exec()==QDialog::Accepted) {
        selectedInterfaces.clear();
        for (const auto &idx : ui->treeWidget->selectionModel()->selectedRows()) {
            if (idx.isValid()) {
                selectedInterfaces.append(allInterfaces[idx.row()]);
            }
        }
        return true;
    } else {
        return false;
    }
}
