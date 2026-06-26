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

#ifndef GENERICCANSETUPPAGE_H
#define GENERICCANSETUPPAGE_H

#include <QWidget>

namespace Ui {
class GenericCanSetupPage;
}

class BusInterface;
class SetupDialog;
class MeasurementInterface;
class Backend;

class GenericCanSetupPage : public QWidget
{
    Q_OBJECT

public:
    explicit GenericCanSetupPage(QWidget *parent = 0);
    ~GenericCanSetupPage();

public slots:
    void onSetupDialogCreated(SetupDialog &dlg);
    void onShowInterfacePage(SetupDialog &dlg, MeasurementInterface *mi);

private slots:
    void updateUI();

private:
    Ui::GenericCanSetupPage *ui;
    MeasurementInterface *_mi;
    bool _enable_ui_updates;

    void fillBitratesList(BusInterface *intf, unsigned selectedBitrate);
    void fillSamplePointsForBitrate(BusInterface *intf, unsigned selectedBitrate, unsigned selectedSamplePoint);
    void fillFdBitrate(BusInterface *intf, unsigned selectedBitrate);
    void fillSamplePointsForFdBitrate(BusInterface *intf, unsigned selectedBitrate, unsigned selectedSamplePoint);
    void disenableUI(bool enabled);

    Backend &backend();
};

#endif // GENERICCANSETUPPAGE_H
