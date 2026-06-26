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

#pragma once

#include "core/ConfigurableWidget.h"
#include "driver/CanDriver.h"

#include <QByteArray>
#include <QList>
#include <QString>

class Backend;
class BusInterface;
class QVBoxLayout;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

struct LinDiagRequest
{
    QString        name;
    BusInterfaceId interfaceId {InvalidBusInterfaceId};
    uint8_t        nad         {0};
    QByteArray     data;
};

class LinControlWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    explicit LinControlWindow(QWidget *parent, Backend &backend);
    ~LinControlWindow();

    bool saveXML(Backend &backend, QDomDocument &doc, QDomElement &root) override;
    bool loadXML(Backend &backend, QDomElement &el) override;

protected:
    void retranslateUi() override;

private slots:
    void rebuildRows();
    void clearRows();
    void onAddDiagRequest();
    void onEditDiagRequest();
    void onRemoveDiagRequest();
    void onSendDiagRequest(QListWidgetItem *item);

private:
    Backend &_backend;

    // Interface rows (scroll area)
    QWidget     *_rowContainer  {nullptr};
    QVBoxLayout *_rowLayout     {nullptr};
    QLabel      *_placeholder   {nullptr};

    // Diagnostic request list
    QList<LinDiagRequest> _diagRequests;
    QListWidget           *_diagList   {nullptr};
    QPushButton           *_btnAdd     {nullptr};
    QPushButton           *_btnEdit    {nullptr};
    QPushButton           *_btnRemove  {nullptr};
};
