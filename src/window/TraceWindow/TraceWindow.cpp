/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>

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

#include "TraceWindow.h"
#include "ui_TraceWindow.h"

#include <QCheckBox>
#include <QDomDocument>
#include <QHeaderView>
#include <QScrollBar>
#include <QSortFilterProxyModel>

#include "core/Backend.h"
#include "window/ConditionalLoggingDialog.h"

#include "AggregatedTraceViewModel.h"
#include "DataColumnDelegate.h"
#include "LinearTraceViewModel.h"
#include "TraceFilterDialog.h"
#include "TraceFilterModel.h"
#include "UnifiedTraceViewModel.h"


TraceWindow::TraceWindow(QWidget *parent, Backend &backend) :
    ConfigurableWidget(parent),
    ui(new Ui::TraceWindow),
    _backend(&backend),
    _mode(mode_unified),
    _timestampMode(timestamp_mode_absolute)
{
    ui->setupUi(this);

    _aggregatedTraceViewModel = new AggregatedTraceViewModel(backend);
    _aggregatedProxyModel = new QSortFilterProxyModel(this);
    _aggregatedProxyModel->setSourceModel(_aggregatedTraceViewModel);
    _aggregatedProxyModel->setDynamicSortFilter(false);

    _aggMonitorFilterModel = new TraceFilterModel(this);
    _aggMonitorFilterModel->setSourceModel(_aggregatedProxyModel);

    // Initialize the 4 rolling categories
    UnifiedTraceViewModel::Category cats[] = {
        UnifiedTraceViewModel::Cat_All,
        UnifiedTraceViewModel::Cat_UDS,
        UnifiedTraceViewModel::Cat_J1939
    };

    QTreeView* trees[] = { ui->treeAgg, ui->treeUds, ui->treeJ1939 };

    QFont font("Monospace");
    font.setStyleHint(QFont::TypeWriter);

    for (int i = 0; i < Cat_Count; ++i) {
        _viewModels[i] = new UnifiedTraceViewModel(backend, cats[i]);
        _filterModels[i] = new TraceFilterModel(this);
        _filterModels[i]->setSourceModel(_viewModels[i]);

        QTreeView* tree = trees[i];
        tree->setModel(_filterModels[i]);
        tree->setFont(font);
        tree->setAlternatingRowColors(true);
        tree->setUniformRowHeights(true);
        tree->setRootIsDecorated(true);
        tree->header()->setDefaultAlignment(Qt::AlignCenter | Qt::AlignVCenter);

        tree->setColumnWidth(BaseTraceViewModel::column_index, 70);
        tree->setColumnWidth(BaseTraceViewModel::column_timestamp, 100);
        tree->setColumnWidth(BaseTraceViewModel::column_channel, 120);
        tree->setColumnWidth(BaseTraceViewModel::column_direction, 56);
        tree->setColumnWidth(BaseTraceViewModel::column_type, 80);
        tree->setColumnWidth(BaseTraceViewModel::column_canid, 110);
        tree->setColumnWidth(BaseTraceViewModel::column_sender, 150);
        tree->setColumnWidth(BaseTraceViewModel::column_name, 150);
        tree->setColumnWidth(BaseTraceViewModel::column_dlc, 50);
        tree->setColumnWidth(BaseTraceViewModel::column_data, 260);
        tree->setColumnWidth(BaseTraceViewModel::column_comment, 120);
        tree->setItemDelegateForColumn(BaseTraceViewModel::column_data, new DataColumnDelegate(tree));

        connect(_filterModels[i], &QAbstractItemModel::rowsInserted, this, &TraceWindow::onRowsInserted);
    }

    connect(_aggMonitorFilterModel, &QAbstractItemModel::rowsInserted, this, &TraceWindow::onRowsInserted);

    // Special handling for the first tab: Can show either Aggregated (Monitor) or Unified (All)
    ui->cbViewMode->addItem(tr("Aggregated"), mode_aggregated);
    ui->cbViewMode->addItem(tr("Rolling Log"), mode_unified);

    ui->cbTimestampMode->addItem(tr("Absolute"), timestamp_mode_absolute);
    ui->cbTimestampMode->addItem(tr("Absolute (UTC)"), timestamp_mode_absolute_utc);
    ui->cbTimestampMode->addItem(tr("Relative"), timestamp_mode_relative);
    ui->cbTimestampMode->addItem(tr("Delta"), timestamp_mode_delta);
    setTimestampMode(timestamp_mode_delta);

    connect(ui->filterLineEdit, &QLineEdit::textChanged, this, &TraceWindow::on_cbFilterChanged);
    connect(ui->TraceClearpushButton, &QPushButton::released, this, &TraceWindow::on_cbTraceClearpushButton);
    connect(ui->cbViewMode, &QComboBox::currentIndexChanged, this, &TraceWindow::on_cbViewMode_currentIndexChanged);
    connect(ui->filterButton, &QPushButton::clicked, this, &TraceWindow::openFilterDialog);

    _scrollTimer.setInterval(100);
    _scrollTimer.setSingleShot(true);
    connect(&_scrollTimer, &QTimer::timeout, this, &TraceWindow::doScrollToBottom);

    setMode(mode_aggregated);
}

TraceWindow::~TraceWindow()
{
    delete ui;
    delete _aggregatedTraceViewModel;
    for (int i = 0; i < Cat_Count; ++i)
    {
        delete _viewModels[i];
    }
}

void TraceWindow::retranslateUi()
{
    ui->retranslateUi(this);
}

void TraceWindow::setMode(TraceWindow::mode_t mode)
{
    _mode = mode;

    if (mode == mode_aggregated) {
        ui->treeAgg->setModel(_aggMonitorFilterModel);
        ui->treeAgg->setSortingEnabled(true);
        ui->treeAgg->sortByColumn(BaseTraceViewModel::column_canid, Qt::AscendingOrder);
    } else {
        ui->treeAgg->setSortingEnabled(false);
        ui->treeAgg->header()->setSortIndicator(-1, Qt::AscendingOrder);
        _filterModels[Cat_Aggregated]->sort(-1);
        ui->treeAgg->setModel(_filterModels[Cat_Aggregated]);
        ui->treeAgg->setRootIsDecorated(true);
    }

    for (int i = 0; i < ui->cbViewMode->count(); i++) {
        if (ui->cbViewMode->itemData(i).toInt() == mode) {
            ui->cbViewMode->setCurrentIndex(i);
            break;
        }
    }
    ui->treeAgg->scrollToBottom();
}


void TraceWindow::setTimestampMode(int mode)
{
    timestamp_mode_t new_mode;
    if ( (mode>=0) && (mode<timestamp_modes_count) )
    {
        new_mode = (timestamp_mode_t) mode;
    }
    else
    {
        new_mode = timestamp_mode_absolute;
    }

    _aggregatedTraceViewModel->setTimestampMode(new_mode);
    for (int i = 0; i < Cat_Count; ++i)
    {
        _viewModels[i]->setTimestampMode(new_mode);
    }

    if (new_mode != _timestampMode)
    {
        _timestampMode = new_mode;
        for (int i=0; i<ui->cbTimestampMode->count(); i++)
        {
            if (ui->cbTimestampMode->itemData(i).toInt() == new_mode)
            {
                ui->cbTimestampMode->setCurrentIndex(i);
            }
        }
        emit(settingsChanged(this));
    }
}

bool TraceWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    if (!ConfigurableWidget::saveXML(backend, xml, root))
    {
        return false;
    }

    root.setAttribute("type", "TraceWindow");
    root.setAttribute("mode", _mode == mode_unified ? "unified" : "aggregated");
    root.setAttribute("TimestampMode", _timestampMode);
    root.setAttribute("ActiveTab", ui->tabs->currentIndex());

    QDomElement elAggregated = xml.createElement("AggregatedTraceView");
    elAggregated.setAttribute("SortColumn", _aggregatedProxyModel->sortColumn());
    root.appendChild(elAggregated);

    QDomElement filterEl = xml.createElement("TraceFilter");
    filterEl.setAttribute("showTx", _filterShowTx ? "1" : "0");
    filterEl.setAttribute("showRx", _filterShowRx ? "1" : "0");

    QStringList hiddenMsgs;
    for (uint32_t id : _filterHiddenMessageIds)
    {
        hiddenMsgs.append(QString::number(id));
    }
    if (!hiddenMsgs.isEmpty())
    {
        filterEl.setAttribute("hiddenMessages", hiddenMsgs.join(","));
    }

    QStringList hiddenLinFrames;
    for (uint8_t id : _filterHiddenLinFrameIds)
    {
        hiddenLinFrames.append(QString::number(id));
    }
    if (!hiddenLinFrames.isEmpty())
    {
        filterEl.setAttribute("hiddenLinFrames", hiddenLinFrames.join(","));
    }

    QStringList hiddenIfs;
    for (BusInterfaceId id : _filterHiddenInterfaces)
    {
        hiddenIfs.append(QString::number(id));
    }
    if (!hiddenIfs.isEmpty())
    {
        filterEl.setAttribute("hiddenInterfaces", hiddenIfs.join(","));
    }

    root.appendChild(filterEl);

    return true;
}

bool TraceWindow::loadXML(Backend &backend, QDomElement &el)
{
    if (!ConfigurableWidget::loadXML(backend, el))
    {
        return false;
    }

    QString modeStr = el.attribute("mode", "unified");
    setMode(modeStr == "unified" ? mode_unified : mode_aggregated);
    setTimestampMode(el.attribute("TimestampMode", "0").toInt());
    ui->tabs->setCurrentIndex(el.attribute("ActiveTab", "0").toInt());

    QDomElement elAggregated = el.firstChildElement("AggregatedTraceView");
    int sortColumn = elAggregated.attribute("SortColumn", "-1").toInt();
    ui->treeAgg->sortByColumn(sortColumn, Qt::AscendingOrder);

    QDomElement filterEl = el.firstChildElement("TraceFilter");
    if (!filterEl.isNull())
    {
        _filterShowTx = filterEl.attribute("showTx", "1") == "1";
        _filterShowRx = filterEl.attribute("showRx", "1") == "1";

        QString hiddenMsgsStr = filterEl.attribute("hiddenMessages");
        if (!hiddenMsgsStr.isEmpty())
        {
            for (const QString &s : hiddenMsgsStr.split(","))
            {
                _filterHiddenMessageIds.insert(s.toUInt());
            }
        }

        QString hiddenLinStr = filterEl.attribute("hiddenLinFrames");
        if (!hiddenLinStr.isEmpty())
        {
            for (const QString &s : hiddenLinStr.split(","))
            {
                _filterHiddenLinFrameIds.insert(static_cast<uint8_t>(s.toUInt()));
            }
        }

        QString hiddenIfsStr = filterEl.attribute("hiddenInterfaces");
        if (!hiddenIfsStr.isEmpty())
        {
            for (const QString &s : hiddenIfsStr.split(","))
            {
                _filterHiddenInterfaces.insert(static_cast<BusInterfaceId>(s.toUInt()));
            }
        }

        applyDialogFilters();
    }

    return true;
}

void TraceWindow::addMessage(const BusMessage &msg)
{
    _backend->getTrace()->enqueueMessage(msg);
}

void TraceWindow::onRowsInserted(const QModelIndex &parent, int first, int last)
{
    (void) parent;
    (void) first;
    (void) last;

    if (!ui->cbAutoscroll->isChecked()) { return; }

    TraceFilterModel *filterModel = qobject_cast<TraceFilterModel*>(sender());

    for (int i = 0; i < Cat_Count; ++i) {
        if (_filterModels[i] == filterModel || (i == 0 && _aggMonitorFilterModel == filterModel)) {
            _scrollPending[i] = true;
            break;
        }
    }

    if (!_scrollTimer.isActive()) {
        _scrollTimer.start();
    }

    if(_backend->getTrace()->size() > 1000000)
    {
        _backend->clearTrace();
    }
}

void TraceWindow::doScrollToBottom()
{
    QTreeView* trees[] = { ui->treeAgg, ui->treeUds, ui->treeJ1939 };
    for (int i = 0; i < Cat_Count; ++i) {
        if (_scrollPending[i]) {
            trees[i]->scrollToBottom();
            _scrollPending[i] = false;
        }
    }
}

void TraceWindow::on_cbTimestampMode_currentIndexChanged(int index)
{
    setTimestampMode((timestamp_mode_t)ui->cbTimestampMode->itemData(index).toInt());
}

void TraceWindow::on_cbFilterChanged()
{
    QString filterText = ui->filterLineEdit->text();
    _aggMonitorFilterModel->setFilterText(filterText);
    _aggMonitorFilterModel->invalidate();

    for (int i = 0; i < Cat_Count; ++i) {
        _filterModels[i]->setFilterText(filterText);
        _filterModels[i]->invalidate();
    }
}

void TraceWindow::on_cbTraceClearpushButton()
{
    _backend->clearTrace();
}

void TraceWindow::on_cbViewMode_currentIndexChanged(int index)
{
    setMode((mode_t)ui->cbViewMode->itemData(index).toInt());
}

void TraceWindow::openFilterDialog()
{
    TraceFilterDialog dlg(*_backend, this);
    dlg.setShowTx(_filterShowTx);
    dlg.setShowRx(_filterShowRx);
    dlg.setHiddenMessageIds(_filterHiddenMessageIds);
    dlg.setHiddenLinFrameIds(_filterHiddenLinFrameIds);
    dlg.setHiddenInterfaces(_filterHiddenInterfaces);

    if (dlg.exec() == QDialog::Accepted)
    {
        _filterShowTx = dlg.showTx();
        _filterShowRx = dlg.showRx();
        _filterHiddenMessageIds = dlg.hiddenMessageIds();
        _filterHiddenLinFrameIds = dlg.hiddenLinFrameIds();
        _filterHiddenInterfaces = dlg.hiddenInterfaces();

        applyDialogFilters();
        emit settingsChanged(this);
    }
}

void TraceWindow::applyDialogFilters()
{
    auto applyToModel = [&](TraceFilterModel *model)
    {
        model->setShowTx(_filterShowTx);
        model->setShowRx(_filterShowRx);
        model->setHiddenMessageIds(_filterHiddenMessageIds);
        model->setHiddenLinFrameIds(_filterHiddenLinFrameIds);
        model->setHiddenInterfaces(_filterHiddenInterfaces);
        model->invalidate();
    };

    for (int i = 0; i < Cat_Count; ++i)
    {
        applyToModel(_filterModels[i]);
    }
    applyToModel(_aggMonitorFilterModel);
}

