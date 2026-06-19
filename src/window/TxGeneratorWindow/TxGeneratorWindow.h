/*

  Copyright (c) 2026 Jayachandran Dharuman
  Copyright (c) 2026 Schildkroet

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

#include "core/Backend.h"
#include "core/ConfigurableWidget.h"
#include "core/MeasurementSetup.h"
#include <QList>
#include <QTreeWidgetItem>


namespace Ui {
class TxGeneratorWindow;
}

#include "BitMatrixWidget.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/CanDbSignal.h"

class QDomDocument;
class QDomElement;

class TxGeneratorWindow : public ConfigurableWidget
{
    Q_OBJECT
public:
    explicit TxGeneratorWindow(QWidget *parent, Backend &backend);
    ~TxGeneratorWindow();

    virtual bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root);
    virtual bool loadXML(Backend &backend, QDomElement &el);
    virtual QSize sizeHint() const override;

protected:
    void retranslateUi() override;

signals:
    void loopbackFrame(const BusMessage &msg);
    void interfaceChanged(BusInterfaceId interfaceId);

public slots:
    void stopAll();

private slots:
    void on_lineEditSearchAvailable_textChanged(const QString &text);
    void on_sliderLayoutZoom_valueChanged(int value);
    void on_cbLayoutCompact_toggled(bool checked);
    void on_btnAddToList_released();
    void on_btnAddManual_released();
    void on_btnRemove_released();
    void on_btnSendOnce_released();
    void on_btnBulkRun_clicked();
    void on_btnBulkStop_clicked();
    void on_spinInterval_valueChanged(int i);
    void on_comboBoxInterface_currentIndexChanged(int index);
    void on_treeAvailable_itemDoubleClicked(QTreeWidgetItem *item, int column);
    void on_treeAvailable_itemSelectionChanged();
    void on_treeActive_itemSelectionChanged();
    void treeActiveItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onSendTimerTimeout();
    void onSetupChanged();
    void on_btnSelectAll_released();
    void on_btnClearAll_released();
    void on_treeActive_itemChanged(QTreeWidgetItem *item, int column);
    void onStatusButtonClicked();
    void updateMeasurementState();
    void refreshInterfaces();
    void onRandomPayloadReleased();

private:
    Ui::TxGeneratorWindow *ui;
    Backend &_backend;
    QTimer *_sendTimer;
    BitMatrixWidget *_bitMatrixWidget;
    class QPushButton *_btnRandomPayload;

    struct CyclicMessage {
        BusMessage msg;
        QString name;
        int interval;
        bool enabled;
        uint64_t lastSent;
        BusInterfaceId interfaceId;
        CanDbMessage *dbMsg;
        QString interfaceName; ///< Display name saved to XML; resolved to interfaceId by resolveInterfaceNames()
    };

    QList<CyclicMessage> _cyclicMessages;

    bool isLoading;
    void updateAvailableList();
    void updateActiveList();
    void updateRowUI(int row);
    void populateAvailableMessages();
    // Matches each CyclicMessage::interfaceName against the combo box and
    // sets interfaceId accordingly. Called after the combo box is repopulated.
    void resolveInterfaceNames();
    void updateSendTimer();
};

