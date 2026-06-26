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
#include <QMap>
#include <QByteArray>
#include <QVector>

class LinDb;
class LinFrame;
class LinSignal;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class LinFrameDefaultsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LinFrameDefaultsDialog(
        LinDb                     *db,
        const QString             &nodeName,
        QMap<uint8_t, QByteArray> &defaults,
        QWidget                   *parent = nullptr);

private:
    void buildUi();
    void populateFrameCombo();
    void loadFrame(int comboIdx);
    void updateBytesFromData();
    void updateSignalsFromData();
    void onByteEdited(int byteIdx);
    void onSignalRawChanged(int signalRow, LinSignal *sig, uint64_t raw);
    void resetToInitValues();

    static void writeRawValue(QByteArray &data, const LinSignal *sig, uint64_t raw);

    LinDb                     *_db;
    QString                    _nodeName;
    QMap<uint8_t, QByteArray> &_defaults;
    QList<LinFrame *>          _frames;

    QComboBox          *_framePicker  {nullptr};
    QGroupBox          *_bytesGroup   {nullptr};
    QVector<QLabel *>   _byteLabels;
    QVector<QLineEdit *> _byteEdits;
    QTableWidget       *_signalTable  {nullptr};

    bool _updating {false};
};
