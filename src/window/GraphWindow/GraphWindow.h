/*

  Copyright (c) 2026 Jayachandran Dharuman

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
#include "core/DBC/CanDbSignal.h"
#include "GraphSignal.h"
#include "VisualizationWidget.h"

class QComboBox;
class QLabel;

namespace Ui {
class GraphWindow;
}

class QDomDocument;
class QDomElement;

struct DecodedSignalData;

class SignalDecoderWorker : public QObject
{
    Q_OBJECT
public:
    explicit SignalDecoderWorker(Backend& backend, QObject *parent = nullptr);

public slots:
    void updateActiveSignals(const QList<GraphSignal*>& activeSignals, const QMap<GraphSignal*, BusInterfaceIdList>& signalInterfaces, double globalStartTime);
    void clearActiveSignals();
    void reset();
    void rewindForWindow(int windowSeconds);
    void onTraceAppended();

signals:
    void dataDecoded(const QMap<GraphSignal*, DecodedSignalData>& newPoints, double globalStartTime);

private:
    struct BusLoadEntry { double timestamp; uint32_t bits; };

    Backend& _backend;
    int _lastProcessedIdx;
    double _globalStartTime;
    QList<GraphSignal*> _activeSignals;
    QMap<GraphSignal*, BusInterfaceIdList> _signalInterfaces;
    QMap<GraphSignal*, QList<BusLoadEntry>> _busLoadWindows;
    QMutex _mutex;
};

class GraphWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    explicit GraphWindow(QWidget *parent, Backend &backend);
    ~GraphWindow();
    virtual bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root);
    virtual bool loadXML(Backend &backend, QDomElement &el);

protected:
    void retranslateUi() override;
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onViewTypeChanged(int index);
    void onClearClicked();
    void onDurationChanged(int index);
    void onZoomInClicked();
    void onZoomOutClicked();
    void on_resetZoomButton_clicked();
    void onConditionChanged(bool met);
    void onLiveValuesUpdated(const QMap<CanDbSignal*, double>& values, bool isStale);
    void onConditionToggled();
    void onAddToConditionClicked();
    void buildConditionsFromTable();
    void onDecodedDataReady(const QMap<GraphSignal*, DecodedSignalData>& newPoints, double globalStartTime);
    void onMouseMove(QMouseEvent *event);
    void onLegendMarkerClicked();
    void onColumnSelectorChanged(int val);
    void onFullResetClicked();
    void populateSignalTree();

    void onSearchTextChanged(const QString &text);
    void onSignalItemChanged(class QTreeWidgetItem *item, int column);
    void onAddGraphClicked();

    void onResumeMeasurement();
    void onPauseMeasurement();

signals:
    void activeSignalsUpdated(const QList<GraphSignal*>& activeSignals, const QMap<GraphSignal*, BusInterfaceIdList>& signalInterfaces, double globalStartTime);
    void requestDecoderReset();
    void requestDecoderRewindForWindow(int windowSeconds);

private:
    void connectLegendMarkers(VisualizationWidget* v);
    Ui::GraphWindow *ui;
    QComboBox *_columnSelector = nullptr;
    QLabel *_columnLabel = nullptr;
    QWidget *_columnContainer = nullptr;
    Backend &_backend;
    double _sessionStartTime = -1.0;
    QList<VisualizationWidget*> _visualizations;
    VisualizationWidget* _activeVisualization;

    QList<GraphSignal*> _ownedSignals;

    void setupVisualizations();
    void updateConditionalSignals();
    void clearGraphData();
    void resetGraphView();
    void notifyWorkerActiveSignals();

    void filterSignalTree(const QString &searchText);
    bool shouldShowSignalItem(class QTreeWidgetItem *item, const QString &searchText);

    void applyPendingSignals();

    struct PendingSignal {
        QString type;   // "can", "lin", "busload"
        QString parent; // message/frame name, or "" for busload
        QString name;   // signal name or full bus-load label
        QColor  color;
    };
    QList<PendingSignal> _pendingSignals;

    QThread* _decoderThread = nullptr;
    SignalDecoderWorker* _decoderWorker = nullptr;
};
