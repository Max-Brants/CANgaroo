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

#include "LinDiagRequestDialog.h"
#include "LinControlWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>

#include "core/Backend.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"

LinDiagRequestDialog::LinDiagRequestDialog(QWidget *parent, Backend &backend,
                                           const LinDiagRequest *existing)
    : QDialog(parent)
    , _backend(backend)
{
    setWindowTitle(existing ? tr("Edit Diagnostic Request") : tr("Add Diagnostic Request"));
    setMinimumWidth(380);

    auto *form = new QFormLayout;

    _nameEdit = new QLineEdit(this);
    _nameEdit->setPlaceholderText(tr("e.g. Read Product ID"));
    form->addRow(tr("Name:"), _nameEdit);

    _ifaceCombo = new QComboBox(this);
    for (BusInterfaceId id : backend.getInterfaceList())
    {
        BusInterface *iface = backend.getInterfaceById(id);
        if (iface && iface->busType() == BusType::LIN)
            _ifaceCombo->addItem(backend.getInterfaceName(id), static_cast<uint>(id));
    }
    form->addRow(tr("Interface:"), _ifaceCombo);

    _nadSpin = new QSpinBox(this);
    _nadSpin->setRange(0, 255);
    _nadSpin->setPrefix("0x");
    _nadSpin->setDisplayIntegerBase(16);
    _nadSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _nadHexLabel = new QLabel("(0)", this);
    _nadHexLabel->setEnabled(false);

    auto *nadContainer = new QWidget(this);
    nadContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *nadRow = new QHBoxLayout(nadContainer);
    nadRow->setContentsMargins(0, 0, 0, 0);
    nadRow->addWidget(_nadSpin, 1);
    nadRow->addWidget(_nadHexLabel);
    form->addRow(tr("NAD:"), nadContainer);

    _dataEdit = new QLineEdit(this);
    _dataEdit->setPlaceholderText(tr("SID + params (no PCI), e.g. 22 F1 90"));
    _dataEdit->setToolTip(tr("Service data bytes starting with the SID (max 6 bytes).\n"
                             "The PCI byte is added automatically — do not include it."));
    // Accept hex pairs separated by optional spaces
    _dataEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("^([0-9A-Fa-f]{2}\\s*)*$"), this));
    form->addRow(tr("Data (SID + params):"), _dataEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(_nadSpin, &QSpinBox::valueChanged, this, &LinDiagRequestDialog::onNadChanged);
    connect(buttons, &QDialogButtonBox::accepted, this, &LinDiagRequestDialog::onAccepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (existing)
    {
        _nameEdit->setText(existing->name);

        const int idx = _ifaceCombo->findData(static_cast<uint>(existing->interfaceId));
        if (idx >= 0)
            _ifaceCombo->setCurrentIndex(idx);

        _nadSpin->setValue(existing->nad);

        QStringList hexParts;
        for (uint8_t byte : existing->data)
            hexParts += QString("%1").arg(byte, 2, 16, QChar('0')).toUpper();
        _dataEdit->setText(hexParts.join(' '));
    }

    onNadChanged(_nadSpin->value());
}

QString LinDiagRequestDialog::name() const
{
    return _nameEdit->text().trimmed();
}

BusInterfaceId LinDiagRequestDialog::interfaceId() const
{
    if (_ifaceCombo->currentIndex() < 0)
        return InvalidBusInterfaceId;
    return static_cast<BusInterfaceId>(_ifaceCombo->currentData().toUInt());
}

uint8_t LinDiagRequestDialog::nad() const
{
    return static_cast<uint8_t>(_nadSpin->value());
}

QByteArray LinDiagRequestDialog::data() const
{
    const QString raw = _dataEdit->text().simplified().remove(' ');
    QByteArray result;
    for (int i = 0; i + 1 < raw.size(); i += 2)
        result.append(static_cast<char>(raw.mid(i, 2).toUInt(nullptr, 16)));
    return result;
}

void LinDiagRequestDialog::onNadChanged(int value)
{
    _nadHexLabel->setText(QString("(%1 dec)").arg(value));
}

void LinDiagRequestDialog::onAccepted()
{
    if (name().isEmpty())
    {
        QMessageBox::warning(this, tr("Validation"), tr("Please enter a name."));
        return;
    }
    if (_ifaceCombo->count() == 0)
    {
        QMessageBox::warning(this, tr("Validation"), tr("No LIN interface available."));
        return;
    }
    if (data().isEmpty())
    {
        QMessageBox::warning(this, tr("Validation"), tr("Please enter at least one data byte."));
        return;
    }
    accept();
}
