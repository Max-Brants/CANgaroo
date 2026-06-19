#pragma once

#include "core/Backend.h"
#include "core/ConfigurableWidget.h"

#include <QByteArray>

class CanOpenDb;
class CanOpenObjectEntry;
class MeasurementNetwork;
class PythonEngine;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;

class SdoWindow : public ConfigurableWidget
{
    Q_OBJECT

public:
    explicit SdoWindow(QWidget *parent, Backend &backend);
    ~SdoWindow() override;

    bool saveXML(Backend &backend, QDomDocument &xml, QDomElement &root) override;
    bool loadXML(Backend &backend, QDomElement &el) override;

protected:
    void retranslateUi() override;

private slots:
    void repopulateFromSetup();
    void onDeviceChanged();
    void onSelectionChanged();
    void onSearchTextChanged(const QString &);
    void onReadClicked();
    void onWriteClicked();
    void onDomainUploadClicked();
    void onSaveToFileClicked();
    void onScriptOutput(const QString &text);
    void onScriptError(const QString &text);
    void onScriptStarted();
    void onScriptFinished();
    void onMeasurementStateChanged();

private:
    enum EditorPage
    {
        EditorNone = 0,
        EditorBoolean,
        EditorInteger,
        EditorReal,
        EditorText,
        EditorHex,
    };

    Backend *_backend;
    PythonEngine *_engine;

    QComboBox *_networkCombo;
    QComboBox *_deviceCombo;
    QComboBox *_interfaceCombo;
    QSpinBox *_nodeSpin;
    QLineEdit *_searchEdit;
    QTreeWidget *_objectTree;
    QStackedWidget *_editorStack;
    QComboBox *_boolEditor;
    QLineEdit *_integerEditor;
    QLineEdit *_realEditor;
    QLineEdit *_textEditor;
    QLineEdit *_hexEditor;
    QPlainTextEdit *_resultView;
    QPushButton *_btnRead;
    QPushButton *_btnWrite;
    QPushButton *_btnDomainUpload;
    QPushButton *_btnSaveToFile;
    QProgressBar *_progressBar;
    QLabel *_statusLabel;
    QLabel *_warningLabel;
    QLabel *_editorHintLabel;

    QString _busyCommand;
    bool _busyHadError = false;
    QString _outputBuffer;
    QByteArray _lastTransferData;
    QByteArray _pendingWritePayload;

    void populateNetworks(const QString &preferredNetwork = QString(),
                          const QString &preferredDbPath = QString(),
                          int preferredInterfaceId = -1,
                          const QString &preferredObjectKey = QString());
    void populateDevices(const QString &preferredDbPath = QString(),
                         const QString &preferredObjectKey = QString());
    void populateInterfaces(int preferredInterfaceId = -1);
    void populateObjects(const QString &preferredObjectKey = QString());
    void fillObjectTreeItem(QTreeWidgetItem *item, const CanOpenObjectEntry &entry) const;
    void applyFilter();
    void updateEditorForSelection();
    void updateActionState();
    void setStatusMessage(const QString &text, bool error = false);
    void setBusy(const QString &command, const QString &message);
    void clearBusy();
    void appendResultText(const QString &text);
    void handleScriptLine(const QString &line);
    void handleProgressPayload(const QByteArray &jsonData);
    void handleResultPayload(const QByteArray &jsonData, bool errorPayload);
    void updateCurrentValue(quint16 index, quint8 subIndex, const QString &valueText);
    void selectObjectKey(const QString &objectKey);
    QString currentObjectKey() const;
    MeasurementNetwork *currentNetwork() const;
    const CanOpenDb *currentDb() const;
    const CanOpenObjectEntry *currentObject() const;
    bool prepareWritePayload(const CanOpenObjectEntry &entry, QByteArray &payload, QString &errorMessage) const;
    QString buildReadScript(const QString &command, quint16 index, quint8 subIndex, int interfaceId, int nodeId, double timeoutSec) const;
    QString buildWriteScript(quint16 index, quint8 subIndex, int interfaceId, int nodeId, const QByteArray &payload, double timeoutSec, bool forceSegment = false) const;
};
