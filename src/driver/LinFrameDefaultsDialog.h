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
