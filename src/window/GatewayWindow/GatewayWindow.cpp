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

#include "GatewayWindow.h"
#include "ui_GatewayWindow.h"

#include <QComboBox>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QDomDocument>
#include <QDomElement>

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/BusMessage.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "core/DBC/CanDb.h"
#include "core/DBC/CanDbMessage.h"
#include "driver/BusInterface.h"

GatewayWindow::GatewayWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , ui(new Ui::GatewayWindow)
    , _backend(backend)
{
    ui->setupUi(this);

    ui->tblRules->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->tblRules->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->tblRules->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->tblRules->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ui->tblRules->verticalHeader()->setVisible(false);

    connect(&backend, &Backend::onSetupChanged, this, &GatewayWindow::onSetupChanged);
    connect(&backend, &Backend::beginMeasurement, this, &GatewayWindow::onSetupChanged);
    connect(&backend, &Backend::endMeasurement,   this, &GatewayWindow::onSetupChanged);

    connect(backend.getTrace(), &BusTrace::messageEnqueued,
            this, &GatewayWindow::onMessageEnqueued,
            Qt::DirectConnection);

    // btnAdd and btnRemove are auto-connected by setupUi via connectSlotsByName
    connect(ui->btnOk, &QPushButton::clicked, this, &GatewayWindow::hide);

    connect(ui->cbEnable, &QCheckBox::toggled, ui->widgetControls, &QWidget::setEnabled);
    ui->widgetControls->setEnabled(ui->cbEnable->isChecked());

    connect(ui->tblRules, &QTableWidget::itemSelectionChanged, this, [this]() {
        ui->btnRemove->setEnabled(ui->tblRules->currentRow() >= 0);
    });

    onSetupChanged();
}

GatewayWindow::~GatewayWindow()
{
    delete ui;
}

void GatewayWindow::retranslateUi()
{
    ui->retranslateUi(this);
}

void GatewayWindow::populateInterfaceCombo(QComboBox *cb, BusInterfaceId selected)
{
    for (auto *net : _backend.getSetup().getNetworks()) {
        for (auto *mi : net->interfaces()) {
            BusInterface *intf = _backend.getInterfaceById(mi->busInterface());
            if (!intf || intf->busType() != BusType::CAN)
                continue;
            const QString label = net->name() + ": " + intf->getName();
            cb->addItem(label, QVariant(mi->busInterface()));
        }
    }

    if (selected != InvalidBusInterfaceId) {
        for (int i = 0; i < cb->count(); ++i) {
            if (static_cast<BusInterfaceId>(cb->itemData(i).toUInt()) == selected) {
                cb->setCurrentIndex(i);
                break;
            }
        }
    }
}

void GatewayWindow::onSetupChanged()
{
    refreshInterfaceCombos();
    refreshMessageCombo();

    // Refresh inline combos in existing table rows, preserving selected interface
    const int rows = ui->tblRules->rowCount();
    for (int row = 0; row < rows; ++row) {
        for (int col : {2, 3}) {
            auto *cb = qobject_cast<QComboBox *>(ui->tblRules->cellWidget(row, col));
            if (!cb) continue;
            const auto prevId = static_cast<BusInterfaceId>(cb->currentData().toUInt());
            cb->blockSignals(true);
            cb->clear();
            populateInterfaceCombo(cb, prevId);
            cb->blockSignals(false);
        }
    }
}

void GatewayWindow::refreshInterfaceCombos()
{
    const auto srcId  = static_cast<BusInterfaceId>(ui->cbSource->currentData().toUInt());
    const auto destId = static_cast<BusInterfaceId>(ui->cbDest->currentData().toUInt());

    ui->cbSource->blockSignals(true);
    ui->cbDest->blockSignals(true);
    ui->cbSource->clear();
    ui->cbDest->clear();

    populateInterfaceCombo(ui->cbSource, srcId);
    populateInterfaceCombo(ui->cbDest,   destId);

    ui->cbSource->blockSignals(false);
    ui->cbDest->blockSignals(false);
}

void GatewayWindow::refreshMessageCombo()
{
    const QString current = ui->cbMessage->currentText();
    ui->cbMessage->blockSignals(true);
    ui->cbMessage->clear();

    for (auto *net : _backend.getSetup().getNetworks()) {
        for (const auto &db : net->_canDbs) {
            if (!db) continue;
            const CanDbMessageList msgs = db->getMessageList();
            for (auto it = msgs.cbegin(); it != msgs.cend(); ++it) {
                const CanDbMessage *msg = *it;
                if (!msg) continue;
                const QString label = msg->getName()
                    + " (0x" + QString::number(msg->getRaw_id(), 16).toUpper() + ")";
                ui->cbMessage->addItem(label, msg->getRaw_id());
            }
        }
    }

    const int idx = ui->cbMessage->findText(current);
    if (idx >= 0)
        ui->cbMessage->setCurrentIndex(idx);

    ui->cbMessage->blockSignals(false);
}

void GatewayWindow::addRuleRow(const GatewayRule &rule)
{
    const int row = ui->tblRules->rowCount();
    ui->tblRules->insertRow(row);

    auto *nameItem = new QTableWidgetItem(rule.name);
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    ui->tblRules->setItem(row, 0, nameItem);

    auto *idItem = new QTableWidgetItem(
        "0x" + QString::number(rule.rawId, 16).toUpper());
    idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
    ui->tblRules->setItem(row, 1, idItem);

    auto *srcCombo = new QComboBox(ui->tblRules);
    populateInterfaceCombo(srcCombo, rule.sourceId);
    ui->tblRules->setCellWidget(row, 2, srcCombo);

    auto *dstCombo = new QComboBox(ui->tblRules);
    populateInterfaceCombo(dstCombo, rule.destId);
    ui->tblRules->setCellWidget(row, 3, dstCombo);

    connect(srcCombo, &QComboBox::currentIndexChanged, this, [this, row, srcCombo](int) {
        if (row < _rules.size())
            _rules[row].sourceId = static_cast<BusInterfaceId>(srcCombo->currentData().toUInt());
    });
    connect(dstCombo, &QComboBox::currentIndexChanged, this, [this, row, dstCombo](int) {
        if (row < _rules.size())
            _rules[row].destId = static_cast<BusInterfaceId>(dstCombo->currentData().toUInt());
    });
}

void GatewayWindow::on_btnAdd_clicked()
{
    if (ui->cbMessage->count() == 0 || ui->cbSource->count() == 0 || ui->cbDest->count() == 0)
        return;

    GatewayRule rule;
    rule.rawId    = ui->cbMessage->currentData().toUInt();
    rule.name     = ui->cbMessage->currentText();
    rule.sourceId = static_cast<BusInterfaceId>(ui->cbSource->currentData().toUInt());
    rule.destId   = static_cast<BusInterfaceId>(ui->cbDest->currentData().toUInt());

    _rules.append(rule);
    addRuleRow(rule);
}

void GatewayWindow::on_btnRemove_clicked()
{
    const int row = ui->tblRules->currentRow();
    if (row < 0 || row >= _rules.size())
        return;

    ui->tblRules->removeRow(row);
    _rules.removeAt(row);
}

BusInterfaceId GatewayWindow::resolveInterfaceName(const QString &name) const
{
    for (int i = 0; i < ui->cbSource->count(); ++i)
        if (ui->cbSource->itemText(i) == name)
            return static_cast<BusInterfaceId>(ui->cbSource->itemData(i).toUInt());
    return InvalidBusInterfaceId;
}

bool GatewayWindow::saveXML(Backend &backend, QDomDocument &doc, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, doc, root))
        return false;

    root.setAttribute("enabled", ui->cbEnable->isChecked() ? 1 : 0);

    for (int row = 0; row < ui->tblRules->rowCount(); ++row) {
        const auto *srcCb = qobject_cast<QComboBox *>(ui->tblRules->cellWidget(row, 2));
        const auto *dstCb = qobject_cast<QComboBox *>(ui->tblRules->cellWidget(row, 3));
        if (!srcCb || !dstCb || row >= _rules.size())
            continue;

        QDomElement ruleEl = doc.createElement("rule");
        ruleEl.setAttribute("rawid",  QString("0x%1").arg(_rules.at(row).rawId, 0, 16).toUpper());
        ruleEl.setAttribute("name",   _rules.at(row).name);
        ruleEl.setAttribute("source", srcCb->currentText());
        ruleEl.setAttribute("dest",   dstCb->currentText());
        root.appendChild(ruleEl);
    }

    return true;
}

bool GatewayWindow::loadXML(Backend &backend, QDomElement &el)
{
    if (!ConfigurableWidget::loadXML(backend, el))
        return false;

    ui->cbEnable->setChecked(el.attribute("enabled", "0").toInt() != 0);

    _rules.clear();
    while (ui->tblRules->rowCount() > 0)
        ui->tblRules->removeRow(0);

    onSetupChanged();  // populate combos so resolveInterfaceName works

    const QDomNodeList ruleNodes = el.elementsByTagName("rule");
    for (int i = 0; i < ruleNodes.count(); ++i) {
        const QDomElement ruleEl = ruleNodes.at(i).toElement();

        GatewayRule rule;
        rule.rawId    = ruleEl.attribute("rawid").toUInt(nullptr, 16);
        rule.name     = ruleEl.attribute("name");
        rule.sourceId = resolveInterfaceName(ruleEl.attribute("source"));
        rule.destId   = resolveInterfaceName(ruleEl.attribute("dest"));

        _rules.append(rule);
        addRuleRow(rule);
    }

    return true;
}

void GatewayWindow::onMessageEnqueued(int idx)
{
    if (!ui->cbEnable->isChecked())
        return;

    const BusMessage msg = _backend.getTrace()->getMessage(idx);
    if (!msg.isRX())
        return;

    for (const auto &rule : std::as_const(_rules)) {
        if (rule.rawId != msg.getRawId())
            continue;
        if (rule.sourceId != msg.getInterfaceId())
            continue;

        BusInterface *intf = _backend.getInterfaceById(rule.destId);
        if (intf && intf->isOpen())
            intf->sendMessage(msg);
    }
}
