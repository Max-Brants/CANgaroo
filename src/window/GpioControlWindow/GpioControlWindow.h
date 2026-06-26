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

#pragma once

#include "core/ConfigurableWidget.h"

#include <QList>
#include <QMap>
#include <QVector>

class Backend;
class GrIPHandler;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

struct GpioPinRow
{
    int          pin;
    QComboBox   *dirCombo;   // "Input" / "Output"
    QLabel      *digitalLbl; // "HIGH" / "LOW"
    QLabel      *voltLbl;    // voltage in mV (pins 0-7) or "N/A" (pins 8-15)
    QPushButton *outputBtn;  // visible only when direction == Output
};

struct GpioDevicePanel
{
    GrIPHandler       *handler;
    QWidget           *container;
    QPushButton       *toggleBtn;
    QSpinBox          *cycleSpin; // update interval in ms (5-255)
    bool               enabled{false};
    QList<GpioPinRow>  pinRows;   // always 16 entries
    uint16_t           outputMask{0};
    uint16_t           dirMask{0};
};

class GpioControlWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    explicit GpioControlWindow(QWidget *parent, Backend &backend);
    ~GpioControlWindow() override;

protected:
    void retranslateUi() override;

private slots:
    void rebuildRows();
    void clearRows();

private:
    void buildDevicePanel(GrIPHandler *handler, const QString &deviceName);
    void onToggleClicked(GpioDevicePanel *panel);
    void onOutputToggled(GpioDevicePanel *panel, int pin);
    void onGpioUpdated(GpioDevicePanel *panel, uint16_t pinState,
                       const QVector<uint16_t> &analogValues);

    Backend     &_backend;
    QWidget     *_rowContainer;
    QVBoxLayout *_rowLayout;
    QLabel      *_placeholder;

    QMap<GrIPHandler *, GpioDevicePanel *> _panels;
};
