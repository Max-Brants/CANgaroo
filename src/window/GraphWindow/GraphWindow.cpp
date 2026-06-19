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

#include "GraphWindow.h"
#include "ui_GraphWindow.h"

#include <QDomDocument>
#include <QInputDialog>
#include <QMessageBox>
#include <QColorDialog>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QDateTime>
#include <QSettings>
#include <QShowEvent>
#include <QCloseEvent>
#include <QtCharts/QLegendMarker>

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/BusMessage.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/LinFrame.h"
#include "core/DBC/LinSignal.h"
#include "core/DBC/CanOpenDb.h"
#include "driver/BusInterface.h"

#include "TimeSeriesVisualization.h"
#include "window/ConditionalLoggingDialog.h"
#include "ScatterVisualization.h"
#include "TextVisualization.h"
#include "GaugeVisualization.h"

#include "SignalSelectorDialog.h"

SignalDecoderWorker::SignalDecoderWorker(Backend& backend, QObject* parent)
    : QObject(parent), _backend(backend), _lastProcessedIdx(0), _globalStartTime(-1.0)
{
    qRegisterMetaType<QMap<GraphSignal*, DecodedSignalData>>("QMap<GraphSignal*,DecodedSignalData>");
    qRegisterMetaType<QList<GraphSignal*>>("QList<GraphSignal*>");
    qRegisterMetaType<QMap<GraphSignal*,BusInterfaceIdList>>("QMap<GraphSignal*,BusInterfaceIdList>");
}

void SignalDecoderWorker::updateActiveSignals(const QList<GraphSignal*>& activeSignals, const QMap<GraphSignal*, BusInterfaceIdList>& signalInterfaces, double globalStartTime)
{
    QMutexLocker locker(&_mutex);
    _activeSignals = activeSignals;
    _signalInterfaces = signalInterfaces;
    _globalStartTime = globalStartTime;
}

void SignalDecoderWorker::clearActiveSignals()
{
    QMutexLocker locker(&_mutex);
    _activeSignals.clear();
    _signalInterfaces.clear();
}

void SignalDecoderWorker::reset()
{
    QMutexLocker locker(&_mutex);
    _lastProcessedIdx = _backend.getTrace()->size();
    _globalStartTime = -1.0;
}

void SignalDecoderWorker::rewindForWindow(int windowSeconds)
{
    QMutexLocker locker(&_mutex);
    const int size = static_cast<int>(_backend.getTrace()->size());
    if (size == 0)
    {
        _lastProcessedIdx = 0;
        _globalStartTime = -1.0;
        return;
    }

    if (windowSeconds <= 0)
    {
        _lastProcessedIdx = 0;
    }
    else
    {
        double latestTs = _backend.getTrace()->getMessage(size - 1).getFloatTimestamp();
        double cutoff = latestTs - static_cast<double>(windowSeconds);

        int lo = 0, hi = size - 1;
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;
            if (_backend.getTrace()->getMessage(mid).getFloatTimestamp() < cutoff)
                lo = mid + 1;
            else
                hi = mid;
        }
        _lastProcessedIdx = lo;
    }
    _globalStartTime = -1.0;
}

void SignalDecoderWorker::onTraceAppended()
{
    QMutexLocker locker(&_mutex);

    int currentSize = _backend.getTrace()->size();
    if (_lastProcessedIdx >= currentSize) return;

    if (_globalStartTime < 0) {
        if (currentSize > _lastProcessedIdx) {
            _globalStartTime = _backend.getTrace()->getMessage(_lastProcessedIdx).getFloatTimestamp();
        } else {
            _lastProcessedIdx = currentSize;
            return;
        }
    }

    if (_activeSignals.isEmpty()) {
        _lastProcessedIdx = currentSize;
        return;
    }

    QMap<GraphSignal*, DecodedSignalData> newPoints;

    for (int i = _lastProcessedIdx; i < currentSize; ++i) {
        BusMessage msg = _backend.getTrace()->getMessage(i);
        BusInterfaceId msgIfId = msg.getInterfaceId();
        double t = msg.getFloatTimestamp() - _globalStartTime;

        for (GraphSignal* signal : _activeSignals) {
            if (signal->isPresentInMessage(msg)) {
                if (_signalInterfaces.contains(signal) && !_signalInterfaces[signal].contains(msgIfId)) {
                    continue;
                }
                double value = signal->extractPhysicalFromMessage(msg);
                newPoints[signal].interfaceId = msgIfId;
                newPoints[signal].timestamps.append(t);
                newPoints[signal].values.append(value);
            }
        }
    }

    _lastProcessedIdx = currentSize;
    if (!newPoints.isEmpty()) {
        emit dataDecoded(newPoints, _globalStartTime);
    }
}

GraphWindow::GraphWindow(QWidget *parent, Backend &backend) :
    ConfigurableWidget(parent),
    ui(new Ui::GraphWindow),
    _backend(backend),
    _activeVisualization(nullptr)
{
    ui->setupUi(this);
    ui->durationSelector->setCurrentIndex(1); // Default to 1 min

    // Fix UI Scale Issues
    ui->signalTree->setColumnWidth(0, 280);

    setupVisualizations();

    connect(ui->viewSelector, &QComboBox::currentIndexChanged, this, &GraphWindow::onViewTypeChanged);
    connect(ui->durationSelector, &QComboBox::currentIndexChanged, this, &GraphWindow::onDurationChanged);
    onDurationChanged(ui->durationSelector->currentIndex());
    connect(ui->clearButton, &QPushButton::clicked, this, &GraphWindow::onClearClicked);
    connect(ui->btnFullReset, &QPushButton::clicked, this, &GraphWindow::onFullResetClicked);
    connect(ui->zoomInButton, &QPushButton::clicked, this, &GraphWindow::onZoomInClicked);
    connect(ui->zoomOutButton, &QPushButton::clicked, this, &GraphWindow::onZoomOutClicked);
    connect(ui->resetZoomButton, &QPushButton::clicked, this, &GraphWindow::on_resetZoomButton_clicked);

    // Setup Background Decoder Thread
    _decoderThread = new QThread(this);
    _decoderWorker = new SignalDecoderWorker(_backend);
    _decoderWorker->moveToThread(_decoderThread);

    connect(_decoderThread, &QThread::finished, _decoderWorker, &QObject::deleteLater);
    connect(this, &GraphWindow::activeSignalsUpdated, _decoderWorker, &SignalDecoderWorker::updateActiveSignals);
    connect(this, &GraphWindow::requestDecoderReset, _decoderWorker, &SignalDecoderWorker::reset);
    connect(this, &GraphWindow::requestDecoderRewindForWindow, _decoderWorker, &SignalDecoderWorker::rewindForWindow);
    connect(_backend.getTrace(), &BusTrace::afterAppend, _decoderWorker, &SignalDecoderWorker::onTraceAppended);
    connect(_decoderWorker, &SignalDecoderWorker::dataDecoded, this, &GraphWindow::onDecodedDataReady);

    // See header comment on _sdoPollTimer: a fast heartbeat checks each active SDO
    // signal's own due time (GraphSignal::sdoPollIntervalMs()) and sends at most one
    // request per tick.
    _sdoPollTimer = new QTimer(this);
    _sdoPollTimer->setInterval(kSdoPollHeartbeatMs);
    connect(_sdoPollTimer, &QTimer::timeout, this, &GraphWindow::onSdoPollTick);

    connect(&_backend, &Backend::beginMeasurement, this, &GraphWindow::onResumeMeasurement);
    connect(&_backend, &Backend::endMeasurement, this, &GraphWindow::onPauseMeasurement);

    _decoderThread->start();

    // Conditional Logging Panel
    connect(ui->chkGraphFilter, &QCheckBox::toggled, this, &GraphWindow::onConditionToggled);

    ConditionalLoggingManager *mgr = _backend.getConditionalLoggingManager();
    connect(mgr, &ConditionalLoggingManager::conditionChanged, this, &GraphWindow::onConditionChanged);
    connect(mgr, &ConditionalLoggingManager::liveValuesUpdated, this, &GraphWindow::onLiveValuesUpdated);

    ui->chkGraphFilter->setChecked(mgr->isEnabled());
    onConditionChanged(mgr->isConditionMet());
    updateConditionalSignals();

    // Dynamic Column Selector for Gauges (Grouped in a container)
    _columnContainer = new QWidget(this);
    QHBoxLayout *colLayout = new QHBoxLayout(_columnContainer);
    colLayout->setContentsMargins(0, 0, 0, 0);
    colLayout->setSpacing(5);

    _columnLabel = new QLabel("Columns:", _columnContainer);
    _columnSelector = new QComboBox(_columnContainer);
    _columnSelector->addItems({"1", "2", "3", "4"});
    _columnSelector->setCurrentIndex(1); // Default to 2

    colLayout->addWidget(_columnLabel);
    colLayout->addWidget(_columnSelector);
    connect(_columnSelector, &QComboBox::currentIndexChanged, this, &GraphWindow::onColumnSelectorChanged);

    // Insert into toolbar layout (on the far right using a spring/stretch)
    // We use a spring to push the following widgets to the absolute right
    ui->toolbarLayout->addStretch(1);
    ui->toolbarLayout->addWidget(_columnContainer);

    _columnContainer->hide();

    connect(ui->signalSearchEdit, &QLineEdit::textChanged, this, &GraphWindow::onSearchTextChanged);
    connect(ui->btnAddGraph, &QPushButton::clicked, this, &GraphWindow::onAddGraphClicked);
    connect(ui->signalTree, &QTreeWidget::itemChanged, this, &GraphWindow::onSignalItemChanged);
    connect(ui->btnAddCondition, &QPushButton::clicked, this, &GraphWindow::onAddToConditionClicked);
    connect(&_backend, &Backend::onSetupChanged, this, &GraphWindow::populateSignalTree);

    populateSignalTree();

    // Initial view - call this LAST after all UI elements are initialized
    onViewTypeChanged(0);
}

GraphWindow::~GraphWindow()
{
    if (_decoderThread) {
        _decoderThread->quit();
        _decoderThread->wait();
    }
    qDeleteAll(_ownedSignals);
    delete ui;
}

void GraphWindow::showEvent(QShowEvent *event)
{
    ConfigurableWidget::showEvent(event);
    QSettings settings;
    const int tabWidth = settings.value("GraphWindow/tabWidgetWidth", -1).toInt();
    if (tabWidth > 0)
    {
        const int total = ui->verticalMainSplitter->width();
        ui->verticalMainSplitter->setSizes({total - tabWidth, tabWidth});
    }
}

void GraphWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings;
    settings.setValue("GraphWindow/tabWidgetWidth", ui->tabWidget->width());
    ConfigurableWidget::closeEvent(event);
}

void GraphWindow::retranslateUi()
{
    ui->retranslateUi(this);
}

void GraphWindow::setupVisualizations()
{
    // Add visualizations to list and stacked widget
    _visualizations.append(new TimeSeriesVisualization(ui->stackedWidget, _backend));
    _visualizations.append(new ScatterVisualization(ui->stackedWidget, _backend));
    _visualizations.append(new TextVisualization(ui->stackedWidget, _backend));
    _visualizations.append(new GaugeVisualization(ui->stackedWidget, _backend));

    QStringList names = {"Graph (Time-series)", "Scatter Chart", "Text", "Gauge"};
    for (int i = 0; i < _visualizations.size(); ++i) {
        ui->stackedWidget->addWidget(_visualizations[i]);
        ui->viewSelector->addItem(names[i]);

        // Connect mouse move for visualizations that support it
        if (auto tsv = qobject_cast<TimeSeriesVisualization*>(_visualizations[i])) {
            connect(tsv, &TimeSeriesVisualization::mouseMoved, this, &GraphWindow::onMouseMove);
            connectLegendMarkers(tsv);
        } else if (auto sv = qobject_cast<ScatterVisualization*>(_visualizations[i])) {
            connect(sv, &ScatterVisualization::mouseMoved, this, &GraphWindow::onMouseMove);
            connectLegendMarkers(sv);
        }
    }
}

void GraphWindow::connectLegendMarkers(VisualizationWidget* v)
{
    QChart *chart = nullptr;
    if (auto tsv = qobject_cast<TimeSeriesVisualization*>(v)) chart = tsv->chart();
    else if (auto sv = qobject_cast<ScatterVisualization*>(v)) chart = sv->chart();

    if (chart) {
        for (auto marker : chart->legend()->markers()) {
            disconnect(marker, &QLegendMarker::clicked, this, &GraphWindow::onLegendMarkerClicked);
            connect(marker, &QLegendMarker::clicked, this, &GraphWindow::onLegendMarkerClicked);
        }
    }
}

void GraphWindow::onViewTypeChanged(int index)
{
    if (index >= 0 && index < _visualizations.size()) {
        _activeVisualization = _visualizations[index];

        ui->stackedWidget->setCurrentWidget(_activeVisualization);

        _activeVisualization->onActivated();

        // Show column selector only for Gauge view (index 3)
        bool isGauge = (index == 3);
        if (_columnContainer) _columnContainer->setVisible(isGauge);
    }
}

void GraphWindow::onDurationChanged(int index)
{
    int seconds = 0;
    switch (index) {
        case 1: seconds = 60; break;
        case 2: seconds = 300; break;
        case 3: seconds = 600; break;
        case 4: seconds = 900; break;
        case 5: seconds = 1800; break;
        default: seconds = 0; break;
    }

    for (auto v : _visualizations) {
        v->setWindowDuration(seconds);
    }

    if (_activeVisualization) {
        _activeVisualization->onActivated();
    }
}

void GraphWindow::onClearClicked()
{
    clearGraphData();
}

void GraphWindow::clearGraphData()
{
    _sessionStartTime = -1.0;
    for (auto v : _visualizations) {
        v->clear();
    }
    emit requestDecoderReset();
}

void GraphWindow::resetGraphView()
{
    clearGraphData();

    // Reset Conditional Logging (Signals, conditions, triggers)
    ConditionalLoggingManager *mgr = _backend.getConditionalLoggingManager();
    mgr->reset();

    // Clear signals from ALL visualizations
    for (auto v : _visualizations) {
        v->clearSignals();
    }

    // Reset UI state
    ui->chkGraphFilter->setChecked(false);

    if (_activeVisualization) _activeVisualization->resetZoom();
}

void GraphWindow::onZoomInClicked()
{
    if (_activeVisualization) _activeVisualization->zoomIn();
}

void GraphWindow::onZoomOutClicked()
{
    if (_activeVisualization) _activeVisualization->zoomOut();
}

void GraphWindow::onFullResetClicked()
{
    resetGraphView();
}

void GraphWindow::on_resetZoomButton_clicked()
{
    if (_activeVisualization) _activeVisualization->resetZoom();
}

void GraphWindow::onConditionChanged(bool met)
{
    if (met) {
        ui->triggerStatusLabel->setText(tr("CONDITION MET - ACTIVE"));
        ui->triggerStatusLabel->setStyleSheet("color: green; font-weight: bold;");

        if (ui->chkGraphFilter->isChecked()) {
            for (auto v : _visualizations) {
                if (auto tsv = qobject_cast<TimeSeriesVisualization*>(v)) {
                    if (tsv->chart()) tsv->chart()->setBackgroundBrush(QBrush(QColor(100, 255, 100, 25)));
                } else if (auto sv = qobject_cast<ScatterVisualization*>(v)) {
                    if (sv->chart()) sv->chart()->setBackgroundBrush(QBrush(QColor(100, 255, 100, 25)));
                }
            }
        }
    } else {
        ui->triggerStatusLabel->setText(tr("Condition not met - Waiting..."));
        ui->triggerStatusLabel->setStyleSheet("");

        for (auto v : _visualizations) {
            v->applyTheme(ThemeManager::instance().currentTheme()); // Reset to default layout
        }
    }
}

void GraphWindow::onLiveValuesUpdated(const QMap<CanDbSignal*, double>& values, bool isStale)
{
    for (int r = 0; r < ui->conditionTable->rowCount(); ++r) {
        QTableWidgetItem *nameItem = ui->conditionTable->item(r, 0);
        if (!nameItem) continue;
        auto *sig = static_cast<CanDbSignal*>(nameItem->data(Qt::UserRole).value<void*>());

        QLabel *liveLabel = qobject_cast<QLabel*>(ui->conditionTable->cellWidget(r, 3));
        if (liveLabel && sig) {
            if (values.contains(sig)) {
                liveLabel->setText(QString::number(values[sig], 'f', 2));
                if (isStale) {
                    liveLabel->setStyleSheet("color: red; font-weight: bold;");
                    ui->triggerStatusLabel->setText(tr("Idle - Signal Lost"));
                    ui->triggerStatusLabel->setStyleSheet("color: gray; font-style: italic;");
                } else {
                    liveLabel->setStyleSheet("color: green; font-weight: bold;");
                }
            } else {
                liveLabel->setText("-");
                liveLabel->setStyleSheet("");
            }
        }
    }
}

void GraphWindow::onConditionToggled()
{
    bool graphFilter = ui->chkGraphFilter->isChecked();

    ConditionalLoggingManager *mgr = _backend.getConditionalLoggingManager();
    mgr->setEnabled(graphFilter);

    onConditionChanged(mgr->isConditionMet());
}

void GraphWindow::onAddToConditionClicked()
{
    // Iterate over whole tree to find checked signals
    QTreeWidgetItemIterator it(ui->signalTree);
    while (*it) {
        if ((*it)->checkState(0) == Qt::Checked) {
            void* sigPtr = (*it)->data(0, Qt::UserRole).value<void*>();
            if (sigPtr) {
                GraphSignal *gs = static_cast<GraphSignal*>(sigPtr);

                // Condition table only supports CAN signals
                if (gs->isLin() || gs->isSdo()) {
                    ++it;
                    continue;
                }

                CanDbSignal *sig = gs->asCanSignal();

                // Ensure it's not already in the table
                bool alreadyAdded = false;
                for (int r = 0; r < ui->conditionTable->rowCount(); ++r) {
                    QTableWidgetItem *existingItem = ui->conditionTable->item(r, 0);
                    if (existingItem && existingItem->data(Qt::UserRole).value<void*>() == static_cast<void*>(sig)) {
                        alreadyAdded = true;
                        break;
                    }
                }

                if (!alreadyAdded) {
                    int row = ui->conditionTable->rowCount();
                    ui->conditionTable->insertRow(row);

                    QString parentMsgName = gs->parentName();
                    if (parentMsgName.isEmpty()) parentMsgName = "MSG";
                    QTableWidgetItem *nameItem = new QTableWidgetItem(QString("[%1] %2").arg(parentMsgName).arg(gs->name()));
                    nameItem->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(sig)));

                    BusInterfaceIdList ifaces = (*it)->data(0, Qt::UserRole + 1).value<BusInterfaceIdList>();
                    nameItem->setData(Qt::UserRole + 1, QVariant::fromValue(ifaces));

                    ui->conditionTable->setItem(row, 0, nameItem);

                    QComboBox *opCombo = new QComboBox();
                    opCombo->addItems({">", "<", "==", ">=", "<=", "!="});
                    ui->conditionTable->setCellWidget(row, 1, opCombo);

                    QLineEdit *valEdit = new QLineEdit("0");
                    ui->conditionTable->setCellWidget(row, 2, valEdit);

                    QLabel *liveLabel = new QLabel("-");
                    liveLabel->setAlignment(Qt::AlignCenter);
                    ui->conditionTable->setCellWidget(row, 3, liveLabel);

                    QPushButton *removeBtn = new QPushButton("Remove");
                    connect(removeBtn, &QPushButton::clicked, this, [this, removeBtn]() {
                        for (int r = 0; r < ui->conditionTable->rowCount(); ++r) {
                            if (ui->conditionTable->cellWidget(r, 4) == removeBtn) {
                                ui->conditionTable->removeRow(r);
                                buildConditionsFromTable();
                                break;
                            }
                        }
                    });
                    ui->conditionTable->setCellWidget(row, 4, removeBtn);

                    connect(opCombo, &QComboBox::currentIndexChanged, this, [this]() { buildConditionsFromTable(); });
                    connect(valEdit, &QLineEdit::textChanged, this, [this]() { buildConditionsFromTable(); });
                }
            }
        }
        ++it;
    }

    buildConditionsFromTable();
}

void GraphWindow::buildConditionsFromTable()
{
    QList<LoggingCondition> conds;
    QList<CanDbSignal*> sigs;
    QMap<CanDbSignal*, BusInterfaceIdList> interfaces;

    for (int r = 0; r < ui->conditionTable->rowCount(); ++r) {
        QTableWidgetItem *nameItem = ui->conditionTable->item(r, 0);
        if (!nameItem) continue;
        auto *sig = static_cast<CanDbSignal*>(nameItem->data(Qt::UserRole).value<void*>());
        BusInterfaceIdList ifaces = nameItem->data(Qt::UserRole + 1).value<BusInterfaceIdList>();

        QComboBox *opCombo = qobject_cast<QComboBox*>(ui->conditionTable->cellWidget(r, 1));
        QLineEdit *valEdit = qobject_cast<QLineEdit*>(ui->conditionTable->cellWidget(r, 2));

        if (sig && opCombo && valEdit) {
            LoggingCondition lc;
            lc.signal = sig;
            lc.threshold = valEdit->text().toDouble();
            switch (opCombo->currentIndex()) {
                case 0: lc.op = ConditionOperator::Greater; break;
                case 1: lc.op = ConditionOperator::Less; break;
                case 2: lc.op = ConditionOperator::Equal; break;
                case 3: lc.op = ConditionOperator::GreaterEqual; break;
                case 4: lc.op = ConditionOperator::LessEqual; break;
                case 5: lc.op = ConditionOperator::NotEqual; break;
            }
            conds.append(lc);
            if (!sigs.contains(sig)) {
                sigs.append(sig);
                interfaces[sig] = ifaces;
            }
        }
    }

    ConditionalLoggingManager *mgr = _backend.getConditionalLoggingManager();
    mgr->setConditions(conds, true); // use AND logic
    mgr->setLogSignals(sigs);
    mgr->setSignalInterfaces(interfaces);

    ui->chkGraphFilter->setEnabled(conds.size() > 0);
    if (conds.isEmpty()) {
        ui->chkGraphFilter->setChecked(false);
    }

    updateConditionalSignals();
}

void GraphWindow::updateConditionalSignals()
{
    notifyWorkerActiveSignals();
}

void GraphWindow::notifyWorkerActiveSignals()
{
    QList<GraphSignal*> allActive;
    QMap<GraphSignal*, BusInterfaceIdList> allInterfaces;

    if (_activeVisualization) {
        allActive = _activeVisualization->getSignals();
        for (auto sig : allActive) {
            allInterfaces[sig] = _activeVisualization->getSignalInterfaces(sig);
        }
    }

    emit activeSignalsUpdated(allActive, allInterfaces, _sessionStartTime);

    _activeSdoSignals.clear();
    _sdoNextDueMs.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto sig : allActive) {
        if (sig->isSdo()) {
            _activeSdoSignals.append(sig);
            _sdoNextDueMs[sig] = now; // poll right away once activated
        }
    }
    updateSdoPollTimer();
}

void GraphWindow::updateSdoPollTimer()
{
    if (!_activeSdoSignals.isEmpty() && _backend.isMeasurementRunning()) {
        if (!_sdoPollTimer->isActive()) _sdoPollTimer->start();
    } else {
        _sdoPollTimer->stop();
    }
}

void GraphWindow::onSdoPollTick()
{
    if (_activeSdoSignals.isEmpty() || !_backend.isMeasurementRunning() || !_activeVisualization)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int n = _activeSdoSignals.size();
    for (int i = 0; i < n; ++i) {
        const int idx = (_sdoScanCursor + i) % n;
        GraphSignal *sig = _activeSdoSignals.at(idx);
        if (_sdoNextDueMs.value(sig, 0) > now)
            continue;

        const CanOpenObjectEntry *entry = sig->asSdoEntry();
        if (!entry) continue;

        // SDO upload (read) initiate request: cs=0x40, index (LE), sub-index, 4 reserved bytes.
        BusMessage req;
        req.setId(0x600u + sig->sdoNodeId());
        req.setExtended(false);
        req.setLength(8);
        req.setData(0x40,
                   static_cast<uint8_t>(entry->index & 0xFF),
                   static_cast<uint8_t>((entry->index >> 8) & 0xFF),
                   entry->subIndex, 0, 0, 0, 0);

        const BusInterfaceIdList ifaces = _activeVisualization->getSignalInterfaces(sig);
        for (BusInterfaceId ifId : ifaces) {
            BusInterface *intf = _backend.getInterfaceById(ifId);
            if (intf && intf->isOpen()) {
                req.setInterfaceId(ifId);
                intf->sendMessage(req);
            }
        }

        _sdoNextDueMs[sig] = now + sig->sdoPollIntervalMs();
        // Rotate so the next tick's scan starts after this signal, giving everyone a fair
        // turn even if several signals are simultaneously due (at most 1 request/tick).
        _sdoScanCursor = (idx + 1) % n;
        break;
    }
}

void GraphWindow::onDecodedDataReady(const QMap<GraphSignal*, DecodedSignalData>& newPoints, double globalStartTime)
{
    if (_sessionStartTime < 0 && globalStartTime >= 0) {
        _sessionStartTime = globalStartTime;
        for (auto v : _visualizations) {
            v->setGlobalStartTime(_sessionStartTime);
        }
    }

    // Live visualizations
    bool triggerEnabled = ui->chkGraphFilter->isChecked();
    bool conditionMet = _backend.getConditionalLoggingManager()->isConditionMet();

    if (!triggerEnabled || conditionMet) {
        for (auto v : _visualizations) {
            v->addDecodedData(newPoints);
        }
    }
}

void GraphWindow::onMouseMove(QMouseEvent *event)
{
    auto tsv = qobject_cast<TimeSeriesVisualization*>(_activeVisualization);

    if (!tsv) {
        return; // Only support tooltips for Time Series currently
    }

    QChartView *view = tsv->chartView();
    QChart *chart = tsv->chart();
    QGraphicsLineItem *cursor = tsv->cursorLine();
    QGraphicsRectItem *tooltipBox = tsv->tooltipBox();
    QGraphicsTextItem *tooltipText = tsv->tooltipText();

    // Mapping from viewport to chart coordinates
    QPointF scenePos = view->mapToScene(event->pos());
    QPointF chartPos = chart->mapFromScene(scenePos);

    if (!chart->plotArea().contains(chartPos)) {
        cursor->hide();
        tooltipBox->hide();
        for (auto tracer : tsv->tracers().values()) tracer->hide();
        return;
    }

    double t = chart->mapToValue(chartPos).x();

    // Update cursor line (position is in chart coordinates)
    cursor->setLine(chartPos.x(), chart->plotArea().top(), chartPos.x(), chart->plotArea().bottom());
    cursor->show();

    // Absolute Timestamp
    uint64_t startUsecs = _backend.getUsecsAtMeasurementStart();
    uint64_t currentUsecs = startUsecs + static_cast<uint64_t>(t * 1000000.0);
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(currentUsecs / 1000);
    QString timeStr = dt.toString("yyyy-MM-dd HH:mm:ss.zzz t");

    QString html = QString("<div style='font-family: Arial; font-size: 11px; padding: 5px; background: white;'>"
                           "<b>%1</b><br/><br/>").arg(timeStr);

    bool foundAny = false;

    auto seriesMap = tsv->seriesMap();
    auto tracers = tsv->tracers();
    for (auto it = seriesMap.begin(); it != seriesMap.end(); ++it) {
        GraphSignal *sig = it.key();
        QXYSeries *series = it.value();
        QGraphicsEllipseItem *tracer = tracers.value(sig);

        const QList<QPointF> points = series->points();
        if (points.isEmpty()) { if (tracer) tracer->hide(); continue; }

        int left = 0, right = points.size() - 1;
        while (left < right) {
            int mid = (left + right) / 2;
            if (points[mid].x() < t) left = mid + 1;
            else right = mid;
        }
        int nearestIdx = (left > 0 && qAbs(points[left-1].x() - t) < qAbs(points[left].x() - t)) ? left - 1 : left;

        QValueAxis *axisX = qobject_cast<QValueAxis*>(chart->axes(Qt::Horizontal).first());
        double xRange = axisX->max() - axisX->min();

        if (qAbs(points[nearestIdx].x() - t) < xRange * 0.1) {
            html += QString("<span style='color:%1; font-size: 14px;'>●</span> (Bus %2) %3: <b>%4</b> %5<br/>")
                    .arg(series->color().name()).arg(tsv->getBusId(sig)).arg(sig->name())
                    .arg(points[nearestIdx].y(), 0, 'f', 2).arg(sig->unit());
            foundAny = true;
            if (tracer) { tracer->setPos(chart->mapToPosition(points[nearestIdx])); tracer->show(); }
        } else { if (tracer) tracer->hide(); }
    }

    html += "</div>";

    if (foundAny) {
        tooltipText->setHtml(html);
        QRectF textRect = tooltipText->boundingRect();
        tooltipBox->setRect(0, 0, textRect.width() + 4, textRect.height() + 4);
        QPointF tooltipPos = chartPos + QPointF(15, -textRect.height() - 15);
        if (tooltipPos.x() + tooltipBox->rect().width() > chart->plotArea().right()) tooltipPos.setX(chartPos.x() - tooltipBox->rect().width() - 15);
        if (tooltipPos.y() < chart->plotArea().top()) tooltipPos.setY(chartPos.y() + 15);
        tooltipBox->setPos(tooltipPos);
        tooltipBox->show();
    } else {
        tooltipBox->hide();
    }
}

void GraphWindow::onLegendMarkerClicked()
{
    if (!_activeVisualization) return;

    QLegendMarker* marker = qobject_cast<QLegendMarker*>(sender());
    if (!marker) return;

    QXYSeries *series = qobject_cast<QXYSeries*>(marker->series());
    if (!series) return;

    // Find the signal associated with this series
    GraphSignal *targetSignal = nullptr;
    for (auto v : _visualizations) {
        if (!v) continue;
        if (auto tsv = qobject_cast<TimeSeriesVisualization*>(v)) {
            auto seriesMap = tsv->seriesMap();
            for (auto it = seriesMap.begin(); it != seriesMap.end(); ++it) {
                if (it.value() == series) { targetSignal = it.key(); break; }
            }
        }
        if (targetSignal) break;
    }

    if (targetSignal) {
        QColor color = QColorDialog::getColor(series->color(), this, "Select Signal Color");
        if (color.isValid()) {
            for (auto v : _visualizations) {
                if (v) v->setSignalColor(targetSignal, color);
            }
        }
    }
}

void GraphWindow::onColumnSelectorChanged(int index)
{
    for (auto *v : _visualizations)
    {
        if (auto *gv = qobject_cast<GaugeVisualization*>(v))
        {
            gv->setColumnCount(index + 1);
            break;
        }
    }
}

bool GraphWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, xml, root)) { return false; }
    root.setAttribute("type", "GraphWindow");
    root.setAttribute("viewType", ui->viewSelector->currentIndex());
    root.setAttribute("tabWidgetWidth", ui->tabWidget->width());
    return true;
}

bool GraphWindow::loadXML(Backend &backend, QDomElement &el)
{
    if (!ConfigurableWidget::loadXML(backend, el)) { return false; }
    int index = el.attribute("viewType", "0").toInt();
    ui->viewSelector->setCurrentIndex(index);
    const int tabWidth = el.attribute("tabWidgetWidth", "-1").toInt();
    if (tabWidth > 0)
    {
        QTimer::singleShot(0, this, [this, tabWidth]()
        {
            const int total = ui->verticalMainSplitter->width();
            ui->verticalMainSplitter->setSizes({total - tabWidth, tabWidth});
        });
    }
    return true;
}

void GraphWindow::onResumeMeasurement()
{
    clearGraphData();
    for (auto v : _visualizations) v->setActive(true);
    updateSdoPollTimer();
}

void GraphWindow::onPauseMeasurement()
{
    for (auto v : _visualizations) v->setActive(false);
    updateSdoPollTimer();
}

void GraphWindow::populateSignalTree()
{
    ui->signalTree->blockSignals(true);
    ui->signalTree->clear();

    // Synchronously drain the worker's signal list before freeing GraphSignal objects.
    // BlockingQueuedConnection ensures onTraceAppended cannot run with stale pointers.
    QMetaObject::invokeMethod(_decoderWorker, "clearActiveSignals", Qt::BlockingQueuedConnection);

    for (auto v : _visualizations) v->clearSignals();
    qDeleteAll(_ownedSignals);
    _ownedSignals.clear();

    MeasurementSetup &setup = _backend.getSetup();

    auto addSignalItem = [&](QTreeWidgetItem *parentItem, GraphSignal *gs,
                              const QString &sigName, const QString &msgName,
                              const QString &details, const QString &comment,
                              const QVariant &interfaceData)
    {
        QTreeWidgetItem *sigItem = new QTreeWidgetItem(parentItem);
        sigItem->setText(0, QString("[%1] %2").arg(msgName).arg(sigName));

        sigItem->setText(1, details);
        sigItem->setText(2, comment);
        sigItem->setCheckState(0, Qt::Unchecked);
        sigItem->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<void*>(gs)));
        sigItem->setData(0, Qt::UserRole + 1, interfaceData);

        QPixmap pix(12, 12);
        uint h = qHash(sigName);
        QColor c = QColor::fromHsl(h % 360, 180, 150);
        pix.fill(c);
        sigItem->setIcon(0, QIcon(pix));

        if (gs->isSdo()) {
            QSpinBox *pollSpin = new QSpinBox();
            pollSpin->setRange(20, 60000);
            pollSpin->setSingleStep(10);
            pollSpin->setSuffix(" ms");
            pollSpin->setValue(gs->sdoPollIntervalMs());
            pollSpin->setToolTip(tr("Read-poll interval for this object. Space out objects "
                                     "on the same node to avoid overlapping SDO requests."));
            connect(pollSpin, qOverload<int>(&QSpinBox::valueChanged), this,
                    [gs](int ms) { gs->setSdoPollIntervalMs(ms); });
            ui->signalTree->setItemWidget(sigItem, 4, pollSpin);
        }
    };

    for (MeasurementNetwork *network : setup.getNetworks()) {
        BusInterfaceIdList interfaces = network->getReferencedBusInterfaces();
        QVariant interfaceData = QVariant::fromValue(interfaces);

        // ── CAN signals ──────────────────────────────────────────────
        if (!network->_canDbs.isEmpty()) {
            QTreeWidgetItem *canRoot = new QTreeWidgetItem(ui->signalTree);
            canRoot->setText(0, tr("CAN — %1").arg(network->name()));
            canRoot->setExpanded(true);

            for (pCanDb db : network->_canDbs) {
                for (CanDbMessage *msg : db->getMessageList().values()) {
                    QString msgName = msg->getName().trimmed();
                    if (msgName.isEmpty())
                        msgName = "MSG_" + QString::number(msg->getRaw_id(), 16).toUpper();

                    QTreeWidgetItem *msgItem = new QTreeWidgetItem(canRoot);
                    msgItem->setText(0, QString("%1 (0x%2)").arg(msgName).arg(msg->getRaw_id(), 0, 16));
                    msgItem->setText(3, "MSG");
                    msgItem->setCheckState(0, Qt::Unchecked);

                    QPixmap msgPix(12, 12);
                    msgPix.fill(QColor("#607d8b"));
                    msgItem->setIcon(0, QIcon(msgPix));

                    for (CanDbSignal *sig : msg->getSignals()) {
                        auto *gs = new GraphSignal(sig);
                        _ownedSignals.append(gs);

                        QString sigName = sig->name().trimmed();
                        if (sigName.isEmpty()) sigName = "Signal";

                        QString details = QString("%1 | %2..%3 %4")
                            .arg(sig->isUnsigned() ? tr("Unsigned") : tr("Signed"))
                            .arg(sig->getMinimumValue())
                            .arg(sig->getMaximumValue())
                            .arg(sig->getUnit());

                        addSignalItem(msgItem, gs, sigName, msgName,
                                      details, sig->comment(), interfaceData);
                    }
                }
            }
        }

        // ── LIN signals ──────────────────────────────────────────────
        if (!network->_linDbs.isEmpty()) {
            QTreeWidgetItem *linRoot = new QTreeWidgetItem(ui->signalTree);
            linRoot->setText(0, tr("LIN — %1").arg(network->name()));
            linRoot->setExpanded(true);

            for (pLinDb db : network->_linDbs) {
                for (LinFrame *frame : db->frames().values()) {
                    QString frameName = frame->name().trimmed();
                    if (frameName.isEmpty())
                        frameName = "FRM_" + QString::number(frame->id(), 16).toUpper();

                    QTreeWidgetItem *frmItem = new QTreeWidgetItem(linRoot);
                    frmItem->setText(0, QString("%1 (0x%2)").arg(frameName).arg(frame->id(), 0, 16));
                    frmItem->setText(3, "FRM");
                    frmItem->setCheckState(0, Qt::Unchecked);

                    QPixmap frmPix(12, 12);
                    frmPix.fill(QColor("#8d6e63"));
                    frmItem->setIcon(0, QIcon(frmPix));

                    for (LinSignal *sig : frame->signalList()) {
                        auto *gs = new GraphSignal(sig, frame);
                        _ownedSignals.append(gs);

                        QString sigName = sig->name().trimmed();
                        if (sigName.isEmpty()) sigName = "Signal";

                        QString details = QString("LIN | %1..%2 %3")
                            .arg(sig->minValue())
                            .arg(sig->maxValue())
                            .arg(sig->unit());

                        addSignalItem(frmItem, gs, sigName, frameName,
                                      details, QString(), interfaceData);
                    }
                }
            }
        }

        // ── CANopen SDO objects ──────────────────────────────────────
        // Polled on demand (see onSdoPollTick()) rather than decoded from passive
        // traffic, so only readable, expedited-sized (<=4 byte) numeric entries qualify.
        if (!network->_canOpenDbs.isEmpty()) {
            QTreeWidgetItem *sdoRoot = new QTreeWidgetItem(ui->signalTree);
            sdoRoot->setText(0, tr("SDO — %1").arg(network->name()));
            sdoRoot->setExpanded(true);

            for (const pCanOpenDb &db : network->_canOpenDbs) {
                if (!db) continue;

                const int nodeId = qBound(1, db->configuredNodeId() >= 0 ? db->configuredNodeId() : 1, 127);
                QString devName = db->deviceName().isEmpty() ? db->fileName() : db->deviceName();

                QTreeWidgetItem *devItem = new QTreeWidgetItem(sdoRoot);
                devItem->setText(0, QString("%1 (node %2)").arg(devName).arg(nodeId));
                devItem->setText(3, "DEV");
                devItem->setCheckState(0, Qt::Unchecked);

                QPixmap devPix(12, 12);
                devPix.fill(QColor("#ff9800"));
                devItem->setIcon(0, QIcon(devPix));

                for (auto it = db->objects().cbegin(); it != db->objects().cend(); ++it) {
                    const CanOpenObjectEntry &entry = it.value();
                    if (!CanOpenDb::isNumericDataType(entry.dataType))
                        continue;
                    const quint8 width = CanOpenDb::expeditedSdoByteWidth(entry);
                    if (width < 1 || width > 4 || !CanOpenDb::accessAllowsRead(entry.accessType))
                        continue;

                    auto *gs = new GraphSignal(&entry, static_cast<quint8>(nodeId));
                    _ownedSignals.append(gs);

                    QString sigName = entry.name.trimmed();
                    if (sigName.isEmpty()) sigName = "Object";

                    QString details = QString("SDO | %1").arg(CanOpenDb::formatObjectKey(entry.index, entry.subIndex));

                    addSignalItem(devItem, gs, sigName, devName,
                                  details, entry.accessType, interfaceData);
                }
            }
        }
    }

    ui->signalTree->blockSignals(false);
}

void GraphWindow::onSearchTextChanged(const QString &text)
{
    filterSignalTree(text);
}

void GraphWindow::filterSignalTree(const QString &searchText)
{
    QTreeWidgetItemIterator it(ui->signalTree);
    while (*it) {
        QTreeWidgetItem *item = *it;
        bool visible = shouldShowSignalItem(item, searchText);
        item->setHidden(!visible);

        if (visible) {
            QTreeWidgetItem *p = item->parent();
            while (p) {
                p->setHidden(false);
                if (!searchText.isEmpty()) {
                    p->setExpanded(true);
                }
                p = p->parent();
            }
        }
        ++it;
    }
}

bool GraphWindow::shouldShowSignalItem(QTreeWidgetItem *item, const QString &searchText)
{
    void* sigPtr = item->data(0, Qt::UserRole).value<void*>();
    if (sigPtr) {
        return searchText.isEmpty() || item->text(0).contains(searchText, Qt::CaseInsensitive);
    }

    for (int i = 0; i < item->childCount(); ++i) {
        if (shouldShowSignalItem(item->child(i), searchText)) {
            return true;
        }
    }

    if (!searchText.isEmpty() && item->text(0).contains(searchText, Qt::CaseInsensitive)) {
        return true;
    }
    return false;
}

void GraphWindow::onSignalItemChanged(QTreeWidgetItem *item, int column)
{
    if (column != 0) return;

    ui->signalTree->blockSignals(true);
    Qt::CheckState state = item->checkState(0);

    auto propagateDown = [&](auto self, QTreeWidgetItem *parentItem, Qt::CheckState checkState) -> void {
        for (int i = 0; i < parentItem->childCount(); ++i) {
            QTreeWidgetItem *child = parentItem->child(i);
            child->setCheckState(0, checkState);
            self(self, child, checkState);
        }
    };
    propagateDown(propagateDown, item, state);

    auto propagateUp = [&](auto self, QTreeWidgetItem *childItem) -> void {
        QTreeWidgetItem *parent = childItem->parent();
        if (!parent) return;

        int checkedCount = 0;
        int partiallyCheckedCount = 0;
        for (int i = 0; i < parent->childCount(); ++i) {
            Qt::CheckState childState = parent->child(i)->checkState(0);
            if (childState == Qt::Checked) checkedCount++;
            else if (childState == Qt::PartiallyChecked) partiallyCheckedCount++;
        }

        if (checkedCount == parent->childCount()) {
            parent->setCheckState(0, Qt::Checked);
        } else if (checkedCount > 0 || partiallyCheckedCount > 0) {
            parent->setCheckState(0, Qt::PartiallyChecked);
        } else {
            parent->setCheckState(0, Qt::Unchecked);
        }
        QFont font = parent->font(0);
        font.setBold(parent->checkState(0) != Qt::Unchecked);
        parent->setFont(0, font);

        self(self, parent);
    };

    QFont itemFont = item->font(0);
    itemFont.setBold(state != Qt::Unchecked);
    item->setFont(0, itemFont);
    propagateUp(propagateUp, item);
    ui->signalTree->blockSignals(false);
}

void GraphWindow::onAddGraphClicked()
{
    if (_visualizations.isEmpty()) return;

    for (auto v : _visualizations) {
        v->clearSignals();
    }

    QTreeWidgetItemIterator it(ui->signalTree);
    while (*it) {
        if ((*it)->checkState(0) == Qt::Checked) {
            void* sigPtr = (*it)->data(0, Qt::UserRole).value<void*>();
            if (sigPtr) {
                GraphSignal *sig = static_cast<GraphSignal*>(sigPtr);
                BusInterfaceIdList interfaces = (*it)->data(0, Qt::UserRole + 1).value<BusInterfaceIdList>();

                QColor sigColor = (*it)->icon(0).pixmap(1,1).toImage().pixelColor(0,0);

                for (auto v : _visualizations) {
                    v->addSignal(sig, interfaces);
                    v->setSignalColor(sig, sigColor);
                }
            }
        }
        ++it;
    }
    int windowSeconds = _activeVisualization ? _activeVisualization->getWindowDuration() : 0;
    _sessionStartTime = -1.0;
    emit requestDecoderRewindForWindow(windowSeconds);
    notifyWorkerActiveSignals();
}
