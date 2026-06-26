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

#include <QDialog>
#include <QByteArray>
#include <QString>

#include "driver/CanDriver.h"

class Backend;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QLabel;

struct LinDiagRequest;

class LinDiagRequestDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LinDiagRequestDialog(QWidget *parent, Backend &backend,
                                  const LinDiagRequest *existing = nullptr);

    QString        name()        const;
    BusInterfaceId interfaceId() const;
    uint8_t        nad()         const;
    QByteArray     data()        const;

private slots:
    void onNadChanged(int value);
    void onAccepted();

private:
    Backend       &_backend;

    QLineEdit     *_nameEdit   {nullptr};
    QComboBox     *_ifaceCombo {nullptr};
    QSpinBox      *_nadSpin    {nullptr};
    QLabel        *_nadHexLabel{nullptr};
    QLineEdit     *_dataEdit   {nullptr};
};
