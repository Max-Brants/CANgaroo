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

#include "LinFrameDefaultsDialog.h"

#include "core/DBC/LinDb.h"
#include "core/DBC/LinFrame.h"
#include "core/DBC/LinSignal.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

LinFrameDefaultsDialog::LinFrameDefaultsDialog(
    LinDb                     *db,
    const QString             &nodeName,
    QMap<uint8_t, QByteArray> &defaults,
    QWidget                   *parent)
    : QDialog(parent),
      _db(db),
      _nodeName(nodeName),
      _defaults(defaults)
{
    setWindowTitle(tr("LIN Frame Default Data"));
    setMinimumWidth(520);
    buildUi();
    populateFrameCombo();
    if (!_frames.isEmpty())
        loadFrame(0);
}

void LinFrameDefaultsDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Frame selector row
    auto *frameRow = new QHBoxLayout;
    frameRow->addWidget(new QLabel(tr("Frame:"), this));
    _framePicker = new QComboBox(this);
    _framePicker->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    frameRow->addWidget(_framePicker);
    mainLayout->addLayout(frameRow);

    // Bytes group
    _bytesGroup = new QGroupBox(tr("Bytes (hex)"), this);
    auto *bytesLayout = new QHBoxLayout(_bytesGroup);
    bytesLayout->setSpacing(6);

    const QRegularExpression hexRe(QStringLiteral("[0-9A-Fa-f]{0,2}"));
    for (int i = 0; i < 8; ++i)
    {
        auto *lbl = new QLabel(QStringLiteral("[%1]").arg(i), this);
        lbl->setAlignment(Qt::AlignCenter);
        _byteLabels.append(lbl);
        bytesLayout->addWidget(lbl);

        auto *edit = new QLineEdit(QStringLiteral("00"), this);
        edit->setMaxLength(2);
        edit->setFixedWidth(34);
        edit->setAlignment(Qt::AlignCenter);
        edit->setValidator(new QRegularExpressionValidator(hexRe, edit));
        _byteEdits.append(edit);
        bytesLayout->addWidget(edit);

        const int idx = i;
        connect(edit, &QLineEdit::textEdited, this, [this, idx]() { onByteEdited(idx); });
    }
    bytesLayout->addStretch();
    mainLayout->addWidget(_bytesGroup);

    // Signals table
    auto *sigGroup = new QGroupBox(tr("Signals"), this);
    auto *sigLayout = new QVBoxLayout(sigGroup);
    _signalTable = new QTableWidget(0, 4, this);
    _signalTable->setHorizontalHeaderLabels({tr("Signal"), tr("Raw Value"), tr("Physical"), tr("Unit")});
    _signalTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _signalTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _signalTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    _signalTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    _signalTable->verticalHeader()->setVisible(false);
    _signalTable->setSelectionMode(QAbstractItemView::NoSelection);
    _signalTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sigLayout->addWidget(_signalTable);
    mainLayout->addWidget(sigGroup);

    // Bottom button row
    auto *btnRow = new QHBoxLayout;
    auto *resetBtn = new QPushButton(tr("Reset to Init Values"), this);
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();
    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    btnRow->addWidget(btnBox);
    mainLayout->addLayout(btnRow);

    connect(_framePicker, &QComboBox::currentIndexChanged, this, &LinFrameDefaultsDialog::loadFrame);
    connect(resetBtn,     &QPushButton::clicked,           this, &LinFrameDefaultsDialog::resetToInitValues);
    connect(btnBox,       &QDialogButtonBox::accepted,     this, &QDialog::accept);
}

void LinFrameDefaultsDialog::populateFrameCombo()
{
    _frames.clear();
    _framePicker->blockSignals(true);
    _framePicker->clear();

    if (_db)
    {
        for (auto it = _db->frames().constBegin(); it != _db->frames().constEnd(); ++it)
        {
            LinFrame *frame = it.value();
            if (!_nodeName.isEmpty() && frame->publisher() != _nodeName)
                continue;
            _frames.append(frame);
            _framePicker->addItem(
                QStringLiteral("%1 (0x%2)")
                    .arg(frame->name())
                    .arg(frame->id(), 2, 16, QChar('0')).toUpper());
        }
    }

    _framePicker->blockSignals(false);
}

void LinFrameDefaultsDialog::loadFrame(int comboIdx)
{
    if (comboIdx < 0 || comboIdx >= _frames.size())
        return;

    LinFrame *frame = _frames.at(comboIdx);
    const uint8_t frameId = frame->id();
    const int dlc = frame->length();

    // Ensure a default data entry exists for this frame
    if (!_defaults.contains(frameId) || _defaults.value(frameId).size() != dlc)
        _defaults.insert(frameId, QByteArray(dlc, '\x00'));

    // Show only the byte editors that fit this frame's DLC
    for (int i = 0; i < 8; ++i)
    {
        const bool visible = (i < dlc);
        _byteLabels[i]->setVisible(visible);
        _byteEdits[i]->setVisible(visible);
    }

    _updating = true;
    updateBytesFromData();

    // Rebuild signal rows
    _signalTable->setRowCount(0);
    const QByteArray &data = _defaults.value(frameId);
    const std::span<const uint8_t> span(
        reinterpret_cast<const uint8_t *>(data.constData()),
        static_cast<size_t>(data.size()));

    for (LinSignal *sig : std::as_const(frame->signalList()))
    {
        const int row = _signalTable->rowCount();
        _signalTable->insertRow(row);

        _signalTable->setItem(row, 0, new QTableWidgetItem(sig->name()));

        const uint64_t rawInit = sig->extractRawValue(span);

        auto *rawSpin = new QSpinBox(_signalTable);
        rawSpin->setMinimum(0);
        rawSpin->setMaximum(sig->bitLength() < 31
                            ? static_cast<int>((1u << sig->bitLength()) - 1u)
                            : INT_MAX);
        rawSpin->setValue(static_cast<int>(rawInit));
        _signalTable->setCellWidget(row, 1, rawSpin);

        const double phys = sig->convertToPhysical(rawInit);
        auto *physItem = new QTableWidgetItem(QString::number(phys, 'f', 3));
        physItem->setFlags(physItem->flags() & ~Qt::ItemIsEditable);
        _signalTable->setItem(row, 2, physItem);

        _signalTable->setItem(row, 3, new QTableWidgetItem(sig->unit()));

        connect(rawSpin, &QSpinBox::valueChanged, this, [this, row, sig](int val)
        {
            onSignalRawChanged(row, sig, static_cast<uint64_t>(val));
        });
    }

    _updating = false;
}

void LinFrameDefaultsDialog::updateBytesFromData()
{
    const int idx = _framePicker->currentIndex();
    if (idx < 0 || idx >= _frames.size())
        return;

    const QByteArray &data = _defaults.value(_frames.at(idx)->id());
    for (int i = 0; i < data.size() && i < 8; ++i)
    {
        _byteEdits[i]->setText(
            QString::number(static_cast<uint8_t>(data[i]), 16)
                .rightJustified(2, '0')
                .toUpper());
    }
}

void LinFrameDefaultsDialog::updateSignalsFromData()
{
    const int idx = _framePicker->currentIndex();
    if (idx < 0 || idx >= _frames.size())
        return;

    LinFrame *frame = _frames.at(idx);
    const QByteArray &data = _defaults.value(frame->id());
    const std::span<const uint8_t> span(
        reinterpret_cast<const uint8_t *>(data.constData()),
        static_cast<size_t>(data.size()));

    const auto &sigList = frame->signalList();
    for (int row = 0; row < _signalTable->rowCount() && row < sigList.size(); ++row)
    {
        LinSignal *sig = sigList.at(row);
        const uint64_t raw = sig->extractRawValue(span);
        const double phys = sig->convertToPhysical(raw);

        if (auto *spin = qobject_cast<QSpinBox *>(_signalTable->cellWidget(row, 1)))
        {
            const QSignalBlocker blocker(spin);
            spin->setValue(static_cast<int>(raw));
        }
        if (auto *item = _signalTable->item(row, 2))
            item->setText(QString::number(phys, 'f', 3));
    }
}

void LinFrameDefaultsDialog::onByteEdited(int byteIdx)
{
    if (_updating)
        return;

    const int idx = _framePicker->currentIndex();
    if (idx < 0 || idx >= _frames.size())
        return;

    const uint8_t frameId = _frames.at(idx)->id();
    QByteArray &data = _defaults[frameId];

    bool ok = false;
    const uint8_t val = static_cast<uint8_t>(_byteEdits[byteIdx]->text().toUInt(&ok, 16));
    if (byteIdx < data.size())
        data[byteIdx] = static_cast<char>(ok ? val : 0u);

    _updating = true;
    updateSignalsFromData();
    _updating = false;
}

void LinFrameDefaultsDialog::onSignalRawChanged(int signalRow, LinSignal *sig, uint64_t raw)
{
    if (_updating)
        return;

    const int idx = _framePicker->currentIndex();
    if (idx < 0 || idx >= _frames.size())
        return;

    const uint8_t frameId = _frames.at(idx)->id();
    QByteArray &data = _defaults[frameId];

    writeRawValue(data, sig, raw);

    if (auto *item = _signalTable->item(signalRow, 2))
        item->setText(QString::number(sig->convertToPhysical(raw), 'f', 3));

    _updating = true;
    updateBytesFromData();
    _updating = false;
}

void LinFrameDefaultsDialog::resetToInitValues()
{
    const int idx = _framePicker->currentIndex();
    if (idx < 0 || idx >= _frames.size())
        return;

    LinFrame *frame = _frames.at(idx);
    const uint8_t frameId = frame->id();
    QByteArray &data = _defaults[frameId];
    data.fill('\x00', frame->length());

    for (LinSignal *sig : std::as_const(frame->signalList()))
        writeRawValue(data, sig, sig->initValue());

    _updating = true;
    updateBytesFromData();
    updateSignalsFromData();
    _updating = false;
}

void LinFrameDefaultsDialog::writeRawValue(QByteArray &data, const LinSignal *sig, uint64_t raw)
{
    for (int i = 0; i < sig->bitLength(); ++i)
    {
        const uint32_t bitPos  = static_cast<uint32_t>(sig->bitOffset()) + static_cast<uint32_t>(i);
        const uint8_t  byteIdx = static_cast<uint8_t>(bitPos / 8u);
        const uint8_t  bitIdx  = static_cast<uint8_t>(bitPos % 8u);
        if (byteIdx < static_cast<uint8_t>(data.size()))
        {
            uint8_t byte = static_cast<uint8_t>(data[byteIdx]);
            if ((raw >> i) & 1u)
                byte |= static_cast<uint8_t>(1u << bitIdx);
            else
                byte &= static_cast<uint8_t>(~(1u << bitIdx));
            data[byteIdx] = static_cast<char>(byte);
        }
    }
}
