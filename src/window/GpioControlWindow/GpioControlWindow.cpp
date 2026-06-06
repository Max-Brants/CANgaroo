/*

  Copyright (c) 2024 - 2026 Schildkroet

  This file is part of CANgaroo.

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

#include "GpioControlWindow.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>

#include "core/Backend.h"
#include "core/MeasurementInterface.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementSetup.h"
#include "driver/BusInterface.h"
#include "driver/GrIPDriver/GrIP/GrIPHandler.h"
#include "driver/GrIPDriver/GrIPInterface.h"

GpioControlWindow::GpioControlWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , _backend(backend)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(4, 4, 4, 4);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    _rowContainer = new QWidget(scrollArea);
    _rowLayout = new QVBoxLayout(_rowContainer);
    _rowLayout->setAlignment(Qt::AlignTop);
    _rowLayout->setSpacing(8);
    _rowLayout->setContentsMargins(0, 0, 0, 0);

    _placeholder = new QLabel(tr("No GrIP devices configured"), _rowContainer);
    _placeholder->setAlignment(Qt::AlignCenter);
    _placeholder->setEnabled(false);
    _rowLayout->addWidget(_placeholder);

    scrollArea->setWidget(_rowContainer);
    outerLayout->addWidget(scrollArea);

    connect(&_backend, &Backend::beginMeasurement, this, &GpioControlWindow::rebuildRows);
    connect(&_backend, &Backend::endMeasurement,   this, &GpioControlWindow::clearRows);

    if (_backend.isMeasurementRunning())
        rebuildRows();
}

GpioControlWindow::~GpioControlWindow() = default;

void GpioControlWindow::retranslateUi()
{
    _placeholder->setText(tr("No GrIP devices configured"));
}

void GpioControlWindow::rebuildRows()
{
    clearRows();

    QSet<GrIPHandler *> seen;
    int deviceIndex = 0;

    for (MeasurementNetwork *network : _backend.getSetup().getNetworks())
    {
        for (MeasurementInterface *mi : network->interfaces())
        {
            BusInterface *iface = _backend.getInterfaceById(mi->busInterface());
            if (!iface)
                continue;

            auto *grip = qobject_cast<GrIPInterface *>(iface);
            if (!grip)
                continue;

            GrIPHandler *h = grip->handler();
            if (!h || seen.contains(h))
                continue;

            seen.insert(h);
            buildDevicePanel(h, tr("GrIP Device %1").arg(++deviceIndex));
        }
    }

    _placeholder->setVisible(_panels.isEmpty());
}

void GpioControlWindow::clearRows()
{
    for (auto *panel : std::as_const(_panels))
    {
        if (panel->enabled)
            panel->handler->GpioSetConfig(false, static_cast<uint8_t>(panel->cycleSpin->value()), panel->dirMask);

        panel->handler->disconnect(this);
        panel->container->deleteLater();
        delete panel;
    }
    _panels.clear();

    _rowLayout->addWidget(_placeholder);
    _placeholder->setVisible(true);
}

void GpioControlWindow::buildDevicePanel(GrIPHandler *handler, const QString &deviceName)
{
    auto *panel = new GpioDevicePanel{};
    panel->handler = handler;

    auto *groupBox = new QGroupBox(deviceName, _rowContainer);
    panel->container = groupBox;

    auto *outerLayout = new QVBoxLayout(groupBox);
    outerLayout->setContentsMargins(6, 6, 6, 6);
    outerLayout->setSpacing(6);

    // --- Config bar ---
    auto *cfgBar = new QWidget(groupBox);
    auto *cfgLayout = new QHBoxLayout(cfgBar);
    cfgLayout->setContentsMargins(0, 0, 0, 0);
    cfgLayout->setSpacing(6);

    auto *cycleLabel = new QLabel(tr("Update interval (ms):"), cfgBar);

    panel->cycleSpin = new QSpinBox(cfgBar);
    panel->cycleSpin->setRange(5, 500);
    panel->cycleSpin->setValue(50);
    panel->cycleSpin->setToolTip(tr("How often the device reports GPIO state (minimum 5 ms)"));

    panel->toggleBtn = new QPushButton(tr("Enable"), cfgBar);

    cfgLayout->addWidget(cycleLabel);
    cfgLayout->addWidget(panel->cycleSpin);
    cfgLayout->addWidget(panel->toggleBtn);
    cfgLayout->addStretch();

    outerLayout->addWidget(cfgBar);

    // --- Pin grid ---
    auto *gridWidget = new QWidget(groupBox);
    auto *grid = new QGridLayout(gridWidget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    // Header row
    int col = 0;
    auto makeHdr = [&](const QString &text)
    {
        auto *lbl = new QLabel(text, gridWidget);
        lbl->setStyleSheet("font-weight: bold;");
        grid->addWidget(lbl, 0, col++);
    };
    makeHdr(tr("Pin"));
    makeHdr(tr("Direction"));
    makeHdr(tr("Digital"));
    makeHdr(tr("Voltage (mV)"));
    makeHdr(tr("Output"));

    // Pin rows
    for (int pin = 0; pin < 16; ++pin)
    {
        const int row = pin + 1;

        auto *pinLbl = new QLabel(QString::number(pin), gridWidget);
        pinLbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(pinLbl, row, 0);

        auto *dirCombo = new QComboBox(gridWidget);
        dirCombo->addItem(tr("Input"));
        dirCombo->addItem(tr("Output"));
        grid->addWidget(dirCombo, row, 1);

        auto *digitalLbl = new QLabel(QStringLiteral("--"), gridWidget);
        digitalLbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(digitalLbl, row, 2);

        auto *voltLbl = new QLabel(pin < 8 ? QStringLiteral("--") : tr("N/A"), gridWidget);
        voltLbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(voltLbl, row, 3);

        auto *outputBtn = new QPushButton(tr("Set HIGH"), gridWidget);
        outputBtn->setVisible(false);
        grid->addWidget(outputBtn, row, 4);

        GpioPinRow pinRow{pin, dirCombo, digitalLbl, voltLbl, outputBtn};
        panel->pinRows.append(pinRow);

        connect(dirCombo, &QComboBox::currentIndexChanged, this,
                [panel, pin](int index)
                {
                    if (index == 1)
                        panel->dirMask |= static_cast<uint16_t>(1u << pin);
                    else
                        panel->dirMask &= static_cast<uint16_t>(~(1u << pin));
                    panel->pinRows[pin].outputBtn->setVisible(index == 1);
                });

        connect(outputBtn, &QPushButton::clicked, this,
                [this, panel, pin]() { onOutputToggled(panel, pin); });
    }

    outerLayout->addWidget(gridWidget);
    _rowLayout->addWidget(groupBox);

    connect(panel->toggleBtn, &QPushButton::clicked, this,
            [this, panel]() { onToggleClicked(panel); });

    connect(handler, &GrIPHandler::gpioUpdated, this,
            [this, panel](uint16_t pinState, QVector<uint16_t> analogValues)
            {
                onGpioUpdated(panel, pinState, analogValues);
            });

    _panels.insert(handler, panel);
}

void GpioControlWindow::onToggleClicked(GpioDevicePanel *panel)
{
    panel->enabled = !panel->enabled;

    if (panel->enabled)
    {
        uint16_t dirMask = 0;
        for (const GpioPinRow &row : std::as_const(panel->pinRows))
        {
            if (row.dirCombo->currentIndex() == 1)
                dirMask |= static_cast<uint16_t>(1u << row.pin);
        }
        panel->dirMask = dirMask;
    }

    for (const GpioPinRow &row : std::as_const(panel->pinRows))
        row.dirCombo->setEnabled(!panel->enabled);

    panel->cycleSpin->setEnabled(!panel->enabled);
    panel->toggleBtn->setText(panel->enabled ? tr("Disable") : tr("Enable"));

    panel->handler->GpioSetConfig(
        panel->enabled,
        static_cast<uint8_t>(panel->cycleSpin->value()),
        panel->dirMask);
}

void GpioControlWindow::onOutputToggled(GpioDevicePanel *panel, int pin)
{
    panel->outputMask ^= static_cast<uint16_t>(1u << pin);

    const bool isHigh = (panel->outputMask >> pin) & 1u;
    panel->pinRows[pin].outputBtn->setText(isHigh ? tr("Set LOW") : tr("Set HIGH"));

    GpioPinRow &row = panel->pinRows[pin];
    row.digitalLbl->setText(isHigh ? tr("HIGH") : tr("LOW"));
    row.digitalLbl->setStyleSheet(isHigh
        ? QStringLiteral("color: #00cc00; font-weight: bold;")
        : QStringLiteral("color: #cc0000; font-weight: bold;"));

    panel->handler->GpioSetOutput(panel->outputMask);
}

void GpioControlWindow::onGpioUpdated(GpioDevicePanel *panel, uint16_t pinState,
                                       const QVector<uint16_t> &analogValues)
{
    for (GpioPinRow &row : panel->pinRows)
    {
        const bool isOutput = (panel->dirMask >> row.pin) & 1u;
        const bool high     = isOutput ? ((panel->outputMask >> row.pin) & 1u)
                                       : ((pinState >> row.pin) & 1u);

        row.digitalLbl->setText(high ? tr("HIGH") : tr("LOW"));
        row.digitalLbl->setStyleSheet(high
            ? QStringLiteral("color: #00cc00; font-weight: bold;")
            : QStringLiteral("color: #cc0000; font-weight: bold;"));

        if (row.pin < 8 && row.pin < analogValues.size())
            row.voltLbl->setText(QString::number(analogValues[row.pin]) + tr(" mV"));

        row.outputBtn->setVisible(isOutput);
    }
}
