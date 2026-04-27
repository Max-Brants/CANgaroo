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

#include "core/Backend.h"
#include "core/ConfigurableWidget.h"
#include "core/MeasurementSetup.h"

class QLineEdit;
class QComboBox;
class QCheckBox;
class QTableWidget;
class QDomDocument;
class QDomElement;
class CanDbMessage;

class RawTxWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    explicit RawTxWindow(QWidget *parent, Backend &backend);

    bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root) override;
    bool loadXML(Backend &backend, QDomElement &el) override;

protected:
    void retranslateUi() override;

public slots:
    void setMessage(const BusMessage &msg, const QString &name, BusInterfaceId interfaceId, CanDbMessage *dbMsg = nullptr);

signals:
    void messageUpdated(const BusMessage &msg);
    void interfaceSelected(BusInterfaceId interfaceId);

private slots:
    void onFieldChanged();
    void onSignalValueChanged(int row, int col);

private:
    Backend &_backend;
    BusMessage _can_msg;
    CanDbMessage *_currentDbMsg;
    BusInterfaceId _slavedInterfaceId;
    bool _settingMessage;
    bool _updatingSignals;

    QLineEdit *_editId;
    QComboBox *_comboDlc;
    QComboBox *_comboInterface;
    QCheckBox *_cbExtended;
    QCheckBox *_cbRTR;
    QCheckBox *_cbFD;
    QCheckBox *_cbBRS;
    QTableWidget *_dataTable;
    QTableWidget *_signalTable;

    static constexpr int DataCols = 8;
    static constexpr int MaxDataRows = 8; // 8 rows × 8 cols = 64 bytes

    void updateDataGrid();
    void updateSignalTable();
    void populateDlcCombo(bool canfd);
    int currentDlc() const;
};
