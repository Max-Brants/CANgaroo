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

#include "LinControlWindow.h"
#include "LinDiagRequestDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QDomDocument>
#include <QDomElement>

#include "core/Backend.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"
#include "core/BusMessage.h"

LinControlWindow::LinControlWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , _backend(backend)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(4, 4, 4, 4);
    outerLayout->setSpacing(6);

    // --- Interface rows (sleep / wakeup) ---
    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    _rowContainer = new QWidget(scrollArea);
    _rowLayout = new QVBoxLayout(_rowContainer);
    _rowLayout->setAlignment(Qt::AlignTop);
    _rowLayout->setSpacing(4);
    _rowLayout->setContentsMargins(0, 0, 0, 0);

    _placeholder = new QLabel(tr("No LIN interfaces configured"), _rowContainer);
    _placeholder->setAlignment(Qt::AlignCenter);
    _placeholder->setEnabled(false);
    _rowLayout->addWidget(_placeholder);

    scrollArea->setWidget(_rowContainer);
    outerLayout->addWidget(scrollArea);

    // --- Diagnostic request list ---
    auto *diagLabel = new QLabel(tr("Diagnostic Requests"), this);
    outerLayout->addWidget(diagLabel);

    auto *diagRow = new QHBoxLayout;

    _diagList = new QListWidget(this);
    _diagList->setSelectionMode(QAbstractItemView::SingleSelection);
    diagRow->addWidget(_diagList);

    auto *diagButtons = new QVBoxLayout;
    _btnAdd    = new QPushButton(tr("Add..."),  this);
    _btnEdit   = new QPushButton(tr("Edit..."), this);
    _btnRemove = new QPushButton(tr("Remove"),  this);
    _btnEdit->setEnabled(false);
    _btnRemove->setEnabled(false);
    diagButtons->addWidget(_btnAdd);
    diagButtons->addWidget(_btnEdit);
    diagButtons->addWidget(_btnRemove);
    diagButtons->addStretch();
    diagRow->addLayout(diagButtons);

    outerLayout->addLayout(diagRow);

    connect(&_backend, &Backend::beginMeasurement, this, &LinControlWindow::rebuildRows);
    connect(&_backend, &Backend::endMeasurement,   this, &LinControlWindow::clearRows);

    connect(_btnAdd,    &QPushButton::clicked, this, &LinControlWindow::onAddDiagRequest);
    connect(_btnEdit,   &QPushButton::clicked, this, &LinControlWindow::onEditDiagRequest);
    connect(_btnRemove, &QPushButton::clicked, this, &LinControlWindow::onRemoveDiagRequest);

    connect(_diagList, &QListWidget::currentRowChanged, this, [this](int row) {
        _btnEdit->setEnabled(row >= 0);
        _btnRemove->setEnabled(row >= 0);
    });
    connect(_diagList, &QListWidget::itemDoubleClicked,
            this, &LinControlWindow::onSendDiagRequest);

    if (_backend.isMeasurementRunning())
        rebuildRows();
}

LinControlWindow::~LinControlWindow() = default;

void LinControlWindow::retranslateUi()
{
    _placeholder->setText(tr("No LIN interfaces configured"));
}

void LinControlWindow::rebuildRows()
{
    clearRows();

    bool anyLin = false;

    for (MeasurementNetwork *network : _backend.getSetup().getNetworks())
    {
        for (MeasurementInterface *mi : network->interfaces())
        {
            if (mi->busType() != BusType::LIN)
                continue;

            BusInterface *iface = _backend.getInterfaceById(mi->busInterface());
            if (!iface)
                continue;

            anyLin = true;

            auto *row = new QWidget(_rowContainer);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(2, 2, 2, 2);

            auto *label = new QLabel(iface->getName(), row);
            label->setMinimumWidth(120);

            auto *sleepBtn  = new QPushButton(tr("Sleep"),  row);
            auto *wakeupBtn = new QPushButton(tr("Wakeup"), row);

            rowLayout->addWidget(label);
            rowLayout->addStretch();
            rowLayout->addWidget(sleepBtn);
            rowLayout->addWidget(wakeupBtn);

            _rowLayout->addWidget(row);

            connect(sleepBtn,  &QPushButton::clicked, this, [iface]() { iface->sendLinSleepWakeup(false); });
            connect(wakeupBtn, &QPushButton::clicked, this, [iface]() { iface->sendLinSleepWakeup(true);  });
        }
    }

    _placeholder->setVisible(!anyLin);
}

void LinControlWindow::clearRows()
{
    QLayoutItem *item;
    while ((item = _rowLayout->takeAt(0)) != nullptr)
    {
        if (QWidget *w = item->widget(); w && w != _placeholder)
            w->deleteLater();
        delete item;
    }

    _rowLayout->addWidget(_placeholder);
    _placeholder->setVisible(true);
}

void LinControlWindow::onAddDiagRequest()
{
    LinDiagRequestDialog dlg(this, _backend);
    if (dlg.exec() != QDialog::Accepted)
        return;

    LinDiagRequest req;
    req.name        = dlg.name();
    req.interfaceId = dlg.interfaceId();
    req.nad         = dlg.nad();
    req.data        = dlg.data();

    _diagRequests.append(req);
    const QString ifaceName = _backend.getInterfaceName(req.interfaceId);
    _diagList->addItem(ifaceName.isEmpty() ? req.name : QStringLiteral("[%1]   %2").arg(ifaceName, req.name));
}

void LinControlWindow::onEditDiagRequest()
{
    const int row = _diagList->currentRow();
    if (row < 0 || row >= _diagRequests.size())
        return;

    LinDiagRequestDialog dlg(this, _backend, &_diagRequests[row]);
    if (dlg.exec() != QDialog::Accepted)
        return;

    LinDiagRequest &req = _diagRequests[row];
    req.name        = dlg.name();
    req.interfaceId = dlg.interfaceId();
    req.nad         = dlg.nad();
    req.data        = dlg.data();

    const QString ifaceName = _backend.getInterfaceName(req.interfaceId);
    _diagList->item(row)->setText(ifaceName.isEmpty() ? req.name : QStringLiteral("[%1]   %2").arg(ifaceName, req.name));
}

void LinControlWindow::onRemoveDiagRequest()
{
    const int row = _diagList->currentRow();
    if (row < 0 || row >= _diagRequests.size())
        return;

    _diagRequests.removeAt(row);
    delete _diagList->takeItem(row);
}

void LinControlWindow::onSendDiagRequest(QListWidgetItem *item)
{
    const int row = _diagList->row(item);
    if (row < 0 || row >= _diagRequests.size())
        return;

    const LinDiagRequest &req = _diagRequests.at(row);
    BusInterface *iface = _backend.getInterfaceById(req.interfaceId);
    if (!iface)
        return;

    iface->sendLinDiagRequest(
        req.nad,
        reinterpret_cast<const uint8_t *>(req.data.constData()),
        static_cast<uint8_t>(req.data.size()));
}

bool LinControlWindow::saveXML(Backend &backend, QDomDocument &doc, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, doc, root))
        return false;

    for (const LinDiagRequest &req : std::as_const(_diagRequests))
    {
        QDomElement el = doc.createElement("DiagRequest");
        el.setAttribute("name",        req.name);
        el.setAttribute("interfaceId", static_cast<uint>(req.interfaceId));
        el.setAttribute("nad",         static_cast<uint>(req.nad));
        el.setAttribute("data",        QString::fromLatin1(req.data.toHex()));
        root.appendChild(el);
    }

    return true;
}

bool LinControlWindow::loadXML(Backend &backend, QDomElement &el)
{
    if (!ConfigurableWidget::loadXML(backend, el))
        return false;

    _diagRequests.clear();
    _diagList->clear();

    const QDomNodeList nodes = el.elementsByTagName("DiagRequest");
    for (int i = 0; i < nodes.count(); ++i)
    {
        const QDomElement e = nodes.at(i).toElement();

        LinDiagRequest req;
        req.name        = e.attribute("name");
        req.interfaceId = static_cast<BusInterfaceId>(e.attribute("interfaceId").toUInt());
        req.nad         = static_cast<uint8_t>(e.attribute("nad").toUInt());
        req.data        = QByteArray::fromHex(e.attribute("data").toLatin1());

        _diagRequests.append(req);
        const QString ifaceName = _backend.getInterfaceName(req.interfaceId);
        _diagList->addItem(ifaceName.isEmpty() ? req.name : QStringLiteral("[%1]   %2").arg(ifaceName, req.name));
    }

    return true;
}
