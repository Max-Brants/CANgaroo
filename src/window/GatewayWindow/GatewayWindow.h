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
#include "driver/BusInterface.h"

#include <QList>
#include <QString>

class QComboBox;

namespace Ui { class GatewayWindow; }

class Backend;

struct GatewayRule
{
    uint32_t       rawId;
    QString        name;
    BusInterfaceId sourceId;
    BusInterfaceId destId;
};

class GatewayWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    explicit GatewayWindow(QWidget *parent, Backend &backend);
    ~GatewayWindow() override;

    bool saveXML(Backend &backend, QDomDocument &doc, QDomElement &root) override;
    bool loadXML(Backend &backend, QDomElement &el) override;

protected:
    void retranslateUi() override;

private slots:
    void onSetupChanged();
    void onMessageEnqueued(int idx);
    void on_btnAdd_clicked();
    void on_btnRemove_clicked();

private:
    void refreshInterfaceCombos();
    void refreshMessageCombo();
    void populateInterfaceCombo(QComboBox *cb, BusInterfaceId selected = InvalidBusInterfaceId);
    void addRuleRow(const GatewayRule &rule);
    BusInterfaceId resolveInterfaceName(const QString &name) const;

    Ui::GatewayWindow *ui;
    Backend           &_backend;
    QList<GatewayRule> _rules;
};
