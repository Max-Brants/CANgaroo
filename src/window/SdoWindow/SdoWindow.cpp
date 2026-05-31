#include "SdoWindow.h"

#include <QAbstractItemView>
#include <QByteArray>
#include <QComboBox>
#include <QDomDocument>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <limits>

#include "core/BusTrace.h"
#include "core/DBC/CanOpenDb.h"
#include "core/MeasurementInterface.h"
#include "core/MeasurementNetwork.h"
#include "core/PythonEngine.h"

namespace {
enum ItemRoles
{
    RoleIndex = Qt::UserRole + 1,
    RoleSubIndex,
    RoleObjectKey,
};

bool accessAllowsRead(const QString &accessType)
{
    const QString normalized = accessType.trimmed().toLower();
    return normalized.isEmpty() || normalized.contains(QLatin1Char('r'));
}

bool accessAllowsWrite(const QString &accessType)
{
    return accessType.trimmed().toLower().contains(QLatin1Char('w'));
}

bool isBooleanType(const CanOpenObjectEntry &entry)
{
    return entry.dataType == 0x0001;
}

bool isSignedIntegerType(const CanOpenObjectEntry &entry)
{
    switch (entry.dataType)
    {
    case 0x0002:
    case 0x0003:
    case 0x0004:
    case 0x0010:
        return true;
    default:
        return false;
    }
}

bool isUnsignedIntegerType(const CanOpenObjectEntry &entry)
{
    switch (entry.dataType)
    {
    case 0x0005:
    case 0x0006:
    case 0x0007:
    case 0x0016:
        return true;
    default:
        return false;
    }
}

bool isIntegerType(const CanOpenObjectEntry &entry)
{
    return isSignedIntegerType(entry) || isUnsignedIntegerType(entry);
}

bool isTextType(const CanOpenObjectEntry &entry)
{
    return entry.dataType == 0x0009 || entry.dataType == 0x000B;
}

bool isHexType(const CanOpenObjectEntry &entry)
{
    return entry.dataType == 0x000A || entry.dataType == 0x000F;
}

bool isDomainType(const CanOpenObjectEntry &entry)
{
    return entry.dataType == 0x000F;
}

QString jsonQuoted(const QByteArray &value)
{
    return QStringLiteral("\"%1\"").arg(QString::fromLatin1(value.toBase64()));
}

int byteWidth(const CanOpenObjectEntry &entry)
{
    if (entry.bitLength > 0)
        return qMax(1, static_cast<int>((entry.bitLength + 7) / 8));

    switch (entry.dataType)
    {
    case 0x0001: return 1;
    case 0x0002:
    case 0x0005: return 1;
    case 0x0003:
    case 0x0006: return 2;
    case 0x0010:
    case 0x0016: return 3;
    case 0x0004:
    case 0x0007: return 4;
    default: return 0;
    }
}

QByteArray integerToLittleEndian(quint64 value, int width)
{
    QByteArray out;
    out.reserve(width);
    for (int i = 0; i < width; ++i)
        out.append(static_cast<char>((value >> (8 * i)) & 0xFFu));
    return out;
}

quint64 littleEndianToUnsigned(const QByteArray &data)
{
    quint64 value = 0;
    for (int i = 0; i < data.size() && i < 8; ++i)
        value |= static_cast<quint64>(static_cast<unsigned char>(data.at(i))) << (8 * i);
    return value;
}

qint64 littleEndianToSigned(const QByteArray &data)
{
    const int bits = qBound(1, data.size() * 8, 63);
    quint64 raw = littleEndianToUnsigned(data);
    if ((raw >> (bits - 1)) & 0x1ULL)
    {
        const quint64 signMask = (~0ULL) << bits;
        raw |= signMask;
    }
    return static_cast<qint64>(raw);
}

QString bytesToHex(const QByteArray &data)
{
    return QString::fromLatin1(data.toHex(' ')).toUpper();
}

QString decodeValueForDisplay(const CanOpenObjectEntry &entry, const QByteArray &data)
{
    if (data.isEmpty())
        return QString();

    if (isBooleanType(entry))
        return (static_cast<unsigned char>(data.at(0)) != 0U) ? QObject::tr("true") : QObject::tr("false");

    if (isSignedIntegerType(entry))
        return QString::number(littleEndianToSigned(data));

    if (isUnsignedIntegerType(entry))
        return QString::number(littleEndianToUnsigned(data));

    if (isTextType(entry))
        return QString::fromUtf8(data);

    return bytesToHex(data);
}

QString buildTransferText(const CanOpenObjectEntry &entry, const QByteArray &data)
{
    QString text;
    text += QObject::tr("Bytes: %1\n").arg(data.size());
    text += QObject::tr("Hex: %1").arg(bytesToHex(data));

    const QString decoded = decodeValueForDisplay(entry, data);
    if (!decoded.isEmpty())
        text += QObject::tr("\nText: %1").arg(decoded);

    return text;
}

bool itemMatchesFilter(QTreeWidgetItem *item, const QString &filter)
{
    if (filter.isEmpty())
        return true;

    for (int column = 0; column < item->columnCount(); ++column)
    {
        if (item->text(column).contains(filter, Qt::CaseInsensitive))
            return true;
    }
    return false;
}
}

SdoWindow::SdoWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , _backend(&backend)
    , _engine(new PythonEngine(backend, this))
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(2, 2, 2, 2);

    auto *selectorGroup = new QGroupBox(tr("Target"), this);
    auto *selectorLayout = new QGridLayout(selectorGroup);

    selectorLayout->addWidget(new QLabel(tr("Network"), selectorGroup), 0, 0);
    _networkCombo = new QComboBox(selectorGroup);
    selectorLayout->addWidget(_networkCombo, 0, 1);

    selectorLayout->addWidget(new QLabel(tr("Device"), selectorGroup), 0, 2);
    _deviceCombo = new QComboBox(selectorGroup);
    selectorLayout->addWidget(_deviceCombo, 0, 3);

    selectorLayout->addWidget(new QLabel(tr("Interface"), selectorGroup), 1, 0);
    _interfaceCombo = new QComboBox(selectorGroup);
    selectorLayout->addWidget(_interfaceCombo, 1, 1);

    selectorLayout->addWidget(new QLabel(tr("Node ID"), selectorGroup), 1, 2);
    _nodeSpin = new QSpinBox(selectorGroup);
    _nodeSpin->setRange(1, 127);
    _nodeSpin->setValue(1);
    selectorLayout->addWidget(_nodeSpin, 1, 3);

    mainLayout->addWidget(selectorGroup);

    auto *toolbar = new QHBoxLayout();
    toolbar->addWidget(new QLabel(tr("Search"), this));
    _searchEdit = new QLineEdit(this);
    _searchEdit->setClearButtonEnabled(true);
    toolbar->addWidget(_searchEdit, 1);
    mainLayout->addLayout(toolbar);

    auto *splitterLayout = new QHBoxLayout();

    _objectTree = new QTreeWidget(this);
    _objectTree->setColumnCount(6);
    _objectTree->setRootIsDecorated(true);
    _objectTree->setAlternatingRowColors(true);
    _objectTree->setSelectionMode(QAbstractItemView::SingleSelection);
    _objectTree->setHeaderLabels({tr("Index"), tr("SubIndex"), tr("Name"), tr("Type"), tr("Access"), tr("Current value")});
    _objectTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _objectTree->header()->setStretchLastSection(true);
    splitterLayout->addWidget(_objectTree, 2);

    auto *rightPanel = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    _warningLabel = new QLabel(rightPanel);
    _warningLabel->setWordWrap(true);
    _warningLabel->setStyleSheet(QStringLiteral("color:#b36b00;"));
    rightLayout->addWidget(_warningLabel);

    _editorHintLabel = new QLabel(rightPanel);
    _editorHintLabel->setWordWrap(true);
    rightLayout->addWidget(_editorHintLabel);

    _editorStack = new QStackedWidget(rightPanel);
    auto *nonePage = new QWidget(_editorStack);
    _editorStack->addWidget(nonePage);

    auto *boolPage = new QWidget(_editorStack);
    auto *boolLayout = new QHBoxLayout(boolPage);
    boolLayout->setContentsMargins(0, 0, 0, 0);
    _boolEditor = new QComboBox(boolPage);
    _boolEditor->addItems({tr("false"), tr("true")});
    boolLayout->addWidget(_boolEditor);
    _editorStack->addWidget(boolPage);

    auto *integerPage = new QWidget(_editorStack);
    auto *integerLayout = new QHBoxLayout(integerPage);
    integerLayout->setContentsMargins(0, 0, 0, 0);
    _integerEditor = new QLineEdit(integerPage);
    integerLayout->addWidget(_integerEditor);
    _editorStack->addWidget(integerPage);

    auto *textPage = new QWidget(_editorStack);
    auto *textLayout = new QHBoxLayout(textPage);
    textLayout->setContentsMargins(0, 0, 0, 0);
    _textEditor = new QLineEdit(textPage);
    textLayout->addWidget(_textEditor);
    _editorStack->addWidget(textPage);

    auto *hexPage = new QWidget(_editorStack);
    auto *hexLayout = new QHBoxLayout(hexPage);
    hexLayout->setContentsMargins(0, 0, 0, 0);
    _hexEditor = new QLineEdit(hexPage);
    hexLayout->addWidget(_hexEditor);
    _editorStack->addWidget(hexPage);

    rightLayout->addWidget(_editorStack);

    auto *buttonLayout = new QHBoxLayout();
    _btnRead = new QPushButton(tr("Read"), rightPanel);
    _btnWrite = new QPushButton(tr("Write"), rightPanel);
    _btnDomainUpload = new QPushButton(tr("Domain Download"), rightPanel);
    _btnSaveToFile = new QPushButton(tr("Save to File"), rightPanel);
    buttonLayout->addWidget(_btnRead);
    buttonLayout->addWidget(_btnWrite);
    buttonLayout->addWidget(_btnDomainUpload);
    buttonLayout->addWidget(_btnSaveToFile);
    buttonLayout->addStretch();
    rightLayout->addLayout(buttonLayout);

    _progressBar = new QProgressBar(rightPanel);
    _progressBar->setTextVisible(false);
    _progressBar->setRange(0, 1);
    _progressBar->setValue(0);
    rightLayout->addWidget(_progressBar);

    _statusLabel = new QLabel(rightPanel);
    _statusLabel->setWordWrap(true);
    rightLayout->addWidget(_statusLabel);

    _resultView = new QPlainTextEdit(rightPanel);
    _resultView->setReadOnly(true);
    rightLayout->addWidget(_resultView, 1);

    splitterLayout->addWidget(rightPanel, 1);
    mainLayout->addLayout(splitterLayout, 1);

    connect(_networkCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        populateDevices();
        populateInterfaces();
        emit settingsChanged(this);
    });
    connect(_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { onDeviceChanged(); });
    connect(_interfaceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        updateActionState();
        emit settingsChanged(this);
    });
    connect(_nodeSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
        updateActionState();
        emit settingsChanged(this);
    });
    connect(_searchEdit, &QLineEdit::textChanged, this, &SdoWindow::onSearchTextChanged);
    connect(_objectTree, &QTreeWidget::itemSelectionChanged, this, &SdoWindow::onSelectionChanged);
    connect(_btnRead, &QPushButton::clicked, this, &SdoWindow::onReadClicked);
    connect(_btnWrite, &QPushButton::clicked, this, &SdoWindow::onWriteClicked);
    connect(_btnDomainUpload, &QPushButton::clicked, this, &SdoWindow::onDomainUploadClicked);
    connect(_btnSaveToFile, &QPushButton::clicked, this, &SdoWindow::onSaveToFileClicked);

    connect(_boolEditor, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { emit settingsChanged(this); });
    connect(_integerEditor, &QLineEdit::textChanged, this, [this](const QString &) { emit settingsChanged(this); });
    connect(_textEditor, &QLineEdit::textChanged, this, [this](const QString &) { emit settingsChanged(this); });
    connect(_hexEditor, &QLineEdit::textChanged, this, [this](const QString &) { emit settingsChanged(this); });

    connect(_engine, &PythonEngine::scriptOutput, this, &SdoWindow::onScriptOutput, Qt::QueuedConnection);
    connect(_engine, &PythonEngine::scriptError, this, &SdoWindow::onScriptError, Qt::QueuedConnection);
    connect(_engine, &PythonEngine::scriptStarted, this, &SdoWindow::onScriptStarted, Qt::QueuedConnection);
    connect(_engine, &PythonEngine::scriptFinished, this, &SdoWindow::onScriptFinished, Qt::QueuedConnection);

    BusTrace *trace = backend.getTrace();
    connect(trace, &BusTrace::messageEnqueued, this, [this, trace](int idx)
    {
        if (_engine->isRunning())
            _engine->enqueueMessage(trace->getMessage(idx));
    }, Qt::DirectConnection);

    connect(_backend, &Backend::onSetupChanged, this, &SdoWindow::repopulateFromSetup);
    connect(_backend, &Backend::beginMeasurement, this, &SdoWindow::onMeasurementStateChanged);
    connect(_backend, &Backend::endMeasurement, this, &SdoWindow::onMeasurementStateChanged);

    populateNetworks();
    updateEditorForSelection();
    updateActionState();
}

SdoWindow::~SdoWindow()
{
    _engine->stopScript();
}

bool SdoWindow::saveXML(Backend &backend, QDomDocument &xml, QDomElement &root)
{
    (void) backend;
    (void) xml;
    if (MeasurementNetwork *network = currentNetwork())
        root.setAttribute(QStringLiteral("network"), network->name());
    if (const CanOpenDb *db = currentDb())
        root.setAttribute(QStringLiteral("eds"), db->path());
    root.setAttribute(QStringLiteral("interface"), _interfaceCombo->currentData().toInt());
    root.setAttribute(QStringLiteral("node"), _nodeSpin->value());
    root.setAttribute(QStringLiteral("search"), _searchEdit->text());
    root.setAttribute(QStringLiteral("object"), currentObjectKey());
    return true;
}

bool SdoWindow::loadXML(Backend &backend, QDomElement &el)
{
    (void) backend;
    const QString networkName = el.attribute(QStringLiteral("network"));
    const QString dbPath = el.attribute(QStringLiteral("eds"));
    const int interfaceId = el.attribute(QStringLiteral("interface"), QStringLiteral("-1")).toInt();
    const int nodeId = el.attribute(QStringLiteral("node"), QStringLiteral("1")).toInt();
    const QString searchText = el.attribute(QStringLiteral("search"));
    const QString objectKey = el.attribute(QStringLiteral("object"));

    populateNetworks(networkName, dbPath, interfaceId, objectKey);
    _searchEdit->setText(searchText);
    _nodeSpin->setValue(qBound(1, nodeId, 127));
    selectObjectKey(objectKey);
    updateActionState();
    return true;
}

void SdoWindow::retranslateUi()
{
    _btnRead->setText(tr("Read"));
    _btnWrite->setText(tr("Write"));
    _btnDomainUpload->setText(tr("Domain Download"));
    _btnSaveToFile->setText(tr("Save to File"));
}

void SdoWindow::repopulateFromSetup()
{
    populateNetworks(currentNetwork() ? currentNetwork()->name() : QString(),
                     currentDb() ? currentDb()->path() : QString(),
                     _interfaceCombo->currentData().toInt(),
                     currentObjectKey());
}

void SdoWindow::onDeviceChanged()
{
    const CanOpenDb *db = currentDb();
    if (db && db->configuredNodeId() >= 0)
        _nodeSpin->setValue(db->configuredNodeId());

    populateObjects(currentObjectKey());
    updateActionState();
    emit settingsChanged(this);
}

void SdoWindow::onSelectionChanged()
{
    updateEditorForSelection();
    updateActionState();
    emit settingsChanged(this);
}

void SdoWindow::onSearchTextChanged(const QString &)
{
    applyFilter();
    emit settingsChanged(this);
}

void SdoWindow::onReadClicked()
{
    const CanOpenObjectEntry *entry = currentObject();
    if (!entry)
        return;

    if (_engine->isRunning())
    {
        setStatusMessage(tr("An SDO operation is already running."), true);
        return;
    }

    const int interfaceId = _interfaceCombo->currentData().toInt();
    const QString script = buildReadScript(QStringLiteral("read"), entry->index, entry->subIndex,
                                           interfaceId, _nodeSpin->value(), 1.0);
    setBusy(QStringLiteral("read"), tr("Reading %1...").arg(CanOpenDb::formatObjectKey(entry->index, entry->subIndex)));
    _engine->runScript(script);
}

void SdoWindow::onWriteClicked()
{
    const CanOpenObjectEntry *entry = currentObject();
    if (!entry)
        return;

    QByteArray payload;
    QString errorMessage;
    if (!prepareWritePayload(*entry, payload, errorMessage))
    {
        setStatusMessage(errorMessage, true);
        return;
    }

    const int interfaceId = _interfaceCombo->currentData().toInt();
    const QString script = buildWriteScript(entry->index, entry->subIndex,
                                            interfaceId, _nodeSpin->value(), payload, 1.0);
    setBusy(QStringLiteral("write"), tr("Writing %1...").arg(CanOpenDb::formatObjectKey(entry->index, entry->subIndex)));
    _engine->runScript(script);
}

void SdoWindow::onDomainUploadClicked()
{
    const CanOpenObjectEntry *entry = currentObject();
    if (!entry || !isDomainType(*entry) || !_engine)
        return;

    const int interfaceId = _interfaceCombo->currentData().toInt();
    if (!accessAllowsWrite(entry->accessType) || interfaceId < 0)
        return;

    const QString fileName = QFileDialog::getOpenFileName(this, tr("Select Domain Data"));
    if (fileName.isEmpty())
        return;

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
    {
        setStatusMessage(tr("Cannot open file for reading."), true);
        return;
    }

    const QByteArray payload = file.readAll();
    const QString script = buildWriteScript(entry->index, entry->subIndex,
                                            interfaceId, _nodeSpin->value(), payload, 10.0);
    setBusy(QStringLiteral("domain-download"),
            tr("Downloading %1 to device (%2 bytes)...")
                .arg(CanOpenDb::formatObjectKey(entry->index, entry->subIndex))
                .arg(payload.size()));
    _engine->runScript(script);
}

void SdoWindow::onSaveToFileClicked()
{
    if (_lastTransferData.isEmpty())
        return;

    QString fileName = QFileDialog::getSaveFileName(this, tr("Save SDO payload"));
    if (fileName.isEmpty())
        return;

    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        setStatusMessage(tr("Cannot open file for writing."), true);
        return;
    }

    file.write(_lastTransferData);
    file.close();
    setStatusMessage(tr("Saved %1 bytes to %2.").arg(_lastTransferData.size()).arg(fileName));
}

void SdoWindow::onScriptOutput(const QString &text)
{
    _outputBuffer += text;
    int newlineIndex = _outputBuffer.indexOf(QLatin1Char('\n'));
    while (newlineIndex >= 0)
    {
        const QString line = _outputBuffer.left(newlineIndex).trimmed();
        _outputBuffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty())
            handleScriptLine(line);
        newlineIndex = _outputBuffer.indexOf(QLatin1Char('\n'));
    }
}

void SdoWindow::onScriptError(const QString &text)
{
    const QString msg = text.trimmed();
    if (!msg.isEmpty())
        setStatusMessage(msg, true);
}

void SdoWindow::onScriptStarted()
{
    updateActionState();
}

void SdoWindow::onScriptFinished()
{
    clearBusy();
    if (!_outputBuffer.trimmed().isEmpty())
    {
        handleScriptLine(_outputBuffer.trimmed());
        _outputBuffer.clear();
    }
    updateActionState();
}

void SdoWindow::onMeasurementStateChanged()
{
    updateActionState();
}

void SdoWindow::populateNetworks(const QString &preferredNetwork,
                                 const QString &preferredDbPath,
                                 int preferredInterfaceId,
                                 const QString &preferredObjectKey)
{
    _networkCombo->blockSignals(true);
    _networkCombo->clear();

    const QList<MeasurementNetwork*> networks = _backend->getSetup().getNetworks();
    int selectedIndex = -1;
    for (int i = 0; i < networks.size(); ++i)
    {
        MeasurementNetwork *network = networks.at(i);
        if (network->_canOpenDbs.isEmpty())
            continue;

        _networkCombo->addItem(network->name(), network->name());
        if (!preferredNetwork.isEmpty() && network->name() == preferredNetwork)
            selectedIndex = _networkCombo->count() - 1;
    }

    if (_networkCombo->count() > 0)
        _networkCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    _networkCombo->blockSignals(false);

    populateDevices(preferredDbPath, preferredObjectKey);
    populateInterfaces(preferredInterfaceId);
    updateActionState();
}

void SdoWindow::populateDevices(const QString &preferredDbPath, const QString &preferredObjectKey)
{
    _deviceCombo->blockSignals(true);
    _deviceCombo->clear();

    MeasurementNetwork *network = currentNetwork();
    int selectedIndex = -1;
    if (network)
    {
        for (int i = 0; i < network->_canOpenDbs.size(); ++i)
        {
            const pCanOpenDb &db = network->_canOpenDbs.at(i);
            QString label = db->deviceName().isEmpty() ? db->fileName() : db->deviceName();
            if (db->configuredNodeId() >= 0)
                label += tr(" (node %1)").arg(db->configuredNodeId());
            _deviceCombo->addItem(label, db->path());
            if (!preferredDbPath.isEmpty() && db->path() == preferredDbPath)
                selectedIndex = i;
        }
    }

    if (_deviceCombo->count() > 0)
        _deviceCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    _deviceCombo->blockSignals(false);

    const CanOpenDb *db = currentDb();
    if (db && db->configuredNodeId() >= 0)
        _nodeSpin->setValue(db->configuredNodeId());

    populateObjects(preferredObjectKey);
}

void SdoWindow::populateInterfaces(int preferredInterfaceId)
{
    _interfaceCombo->blockSignals(true);
    _interfaceCombo->clear();

    MeasurementNetwork *network = currentNetwork();
    int selectedIndex = -1;
    if (network)
    {
        const QList<MeasurementInterface*> interfaces = network->interfaces();
        for (int i = 0; i < interfaces.size(); ++i)
        {
            MeasurementInterface *mi = interfaces.at(i);
            if (mi->busType() != BusType::CAN)
                continue;

            QString label;
            int interfaceId = static_cast<int>(mi->busInterface());
            if (!mi->isResolved() || interfaceId == InvalidBusInterfaceId)
            {
                interfaceId = -1;
                label = tr("(unavailable) %1 / %2")
                    .arg(mi->savedDriverName(), mi->savedInterfaceName());
            }
            else
            {
                label = _backend->getDriverName(mi->busInterface()) + QStringLiteral(" / ")
                      + _backend->getInterfaceName(mi->busInterface());
            }

            _interfaceCombo->addItem(label, interfaceId);
            if (interfaceId == preferredInterfaceId)
                selectedIndex = _interfaceCombo->count() - 1;
        }
    }

    if (_interfaceCombo->count() > 0)
        _interfaceCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    _interfaceCombo->blockSignals(false);
}

void SdoWindow::populateObjects(const QString &preferredObjectKey)
{
    _objectTree->clear();

    const CanOpenDb *db = currentDb();
    if (!db)
    {
        updateEditorForSelection();
        updateActionState();
        return;
    }

    QMap<quint16, QList<CanOpenObjectEntry>> grouped;
    for (auto it = db->objects().cbegin(); it != db->objects().cend(); ++it)
        grouped[it.value().index].append(it.value());

    for (auto it = grouped.cbegin(); it != grouped.cend(); ++it)
    {
        const QList<CanOpenObjectEntry> entries = it.value();
        const CanOpenObjectEntry *sub0Entry = nullptr;
        for (const CanOpenObjectEntry &entry : entries)
        {
            if (entry.subIndex == 0)
            {
                sub0Entry = &entry;
                break;
            }
        }

        QTreeWidgetItem *parentItem = new QTreeWidgetItem(_objectTree);
        if (sub0Entry)
        {
            fillObjectTreeItem(parentItem, *sub0Entry);
        }
        else
        {
            parentItem->setText(0, QStringLiteral("0x%1").arg(it.key(), 4, 16, QChar('0')).toUpper());
            parentItem->setText(2, entries.first().name);
            parentItem->setFlags(parentItem->flags() & ~Qt::ItemIsSelectable);
        }

        for (const CanOpenObjectEntry &entry : entries)
        {
            if (sub0Entry && entry.subIndex == 0)
                continue;
            QTreeWidgetItem *child = new QTreeWidgetItem(parentItem);
            fillObjectTreeItem(child, entry);
        }
    }

    _objectTree->expandToDepth(0);
    applyFilter();
    selectObjectKey(preferredObjectKey);
    if (!_objectTree->currentItem() && _objectTree->topLevelItemCount() > 0)
        _objectTree->setCurrentItem(_objectTree->topLevelItem(0));
}

void SdoWindow::fillObjectTreeItem(QTreeWidgetItem *item, const CanOpenObjectEntry &entry) const
{
    item->setText(0, QStringLiteral("0x%1").arg(entry.index, 4, 16, QChar('0')).toUpper());
    item->setText(1, QString::number(entry.subIndex));
    item->setText(2, entry.name);
    item->setText(3, entry.dataTypeName.isEmpty() ? entry.objectType : entry.dataTypeName);
    item->setText(4, entry.accessType);
    item->setText(5, entry.effectiveValue());
    item->setData(0, RoleIndex, static_cast<int>(entry.index));
    item->setData(0, RoleSubIndex, static_cast<int>(entry.subIndex));
    item->setData(0, RoleObjectKey, CanOpenDb::formatObjectKey(entry.index, entry.subIndex));
}

void SdoWindow::applyFilter()
{
    const QString filter = _searchEdit->text().trimmed();
    for (int i = 0; i < _objectTree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *parent = _objectTree->topLevelItem(i);
        const bool parentMatches = itemMatchesFilter(parent, filter);
        bool anyVisibleChild = false;
        for (int childIndex = 0; childIndex < parent->childCount(); ++childIndex)
        {
            QTreeWidgetItem *child = parent->child(childIndex);
            const bool childVisible = parentMatches || itemMatchesFilter(child, filter);
            child->setHidden(!childVisible);
            anyVisibleChild = anyVisibleChild || childVisible;
        }

        const bool hasObject = parent->data(0, RoleObjectKey).isValid();
        const bool parentVisible = parentMatches || anyVisibleChild || (filter.isEmpty() && (hasObject || parent->childCount() > 0));
        parent->setHidden(!parentVisible);
        if (!filter.isEmpty())
            parent->setExpanded(anyVisibleChild);
    }
}

void SdoWindow::updateEditorForSelection()
{
    const CanOpenObjectEntry *entry = currentObject();
    if (!entry)
    {
        _editorStack->setCurrentIndex(EditorNone);
        _editorHintLabel->setText(tr("Select an object to inspect or transfer."));
        _integerEditor->clear();
        _textEditor->clear();
        _hexEditor->clear();
        return;
    }

    const QString currentValue = _objectTree->currentItem() ? _objectTree->currentItem()->text(5).trimmed()
                                                            : entry->effectiveValue().trimmed();

    if (isBooleanType(*entry))
    {
        _editorStack->setCurrentIndex(EditorBoolean);
        _boolEditor->setCurrentIndex((currentValue == QStringLiteral("1"))
                                     || currentValue.compare(tr("true"), Qt::CaseInsensitive) == 0 ? 1 : 0);
        _editorHintLabel->setText(tr("Boolean editor"));
    }
    else if (isIntegerType(*entry) && byteWidth(*entry) > 0 && byteWidth(*entry) <= 4)
    {
        _editorStack->setCurrentIndex(EditorInteger);
        _integerEditor->setText(currentValue);
        _integerEditor->setPlaceholderText(entry->isSigned() ? tr("Decimal or hex value") : tr("Unsigned decimal or hex value"));
        _editorHintLabel->setText(tr("Integer editor (%1-bit)").arg(entry->bitLength > 0 ? entry->bitLength : byteWidth(*entry) * 8));
    }
    else if (isTextType(*entry))
    {
        _editorStack->setCurrentIndex(EditorText);
        _textEditor->setText(currentValue);
        _editorHintLabel->setText(tr("Text editor (expedited writes up to 4 bytes)"));
    }
    else if (isDomainType(*entry))
    {
        _editorStack->setCurrentIndex(EditorNone);
        _editorHintLabel->setText(tr("Use Domain Download to send a file to the device."));
    }
    else if (isHexType(*entry))
    {
        _editorStack->setCurrentIndex(EditorHex);
        _hexEditor->setText(currentValue);
        _hexEditor->setPlaceholderText(tr("AA BB CC DD"));
        _editorHintLabel->setText(tr("Hex byte editor (expedited writes up to 4 bytes)"));
    }
    else
    {
        _editorStack->setCurrentIndex(EditorNone);
        _editorHintLabel->setText(tr("This datatype is read-only in the current SDO UI."));
    }
}

void SdoWindow::updateActionState()
{
    const CanOpenObjectEntry *entry = currentObject();
    const bool hasObject = entry != nullptr;
    const bool interfaceAvailable = _interfaceCombo->currentData().toInt() >= 0;
    const bool measurementRunning = _backend->isMeasurementRunning();
    const bool engineIdle = !_engine->isRunning();

    const bool domainObject = hasObject && isDomainType(*entry);
    const bool canRead = hasObject && !domainObject && accessAllowsRead(entry->accessType) && interfaceAvailable && measurementRunning && engineIdle;
    const bool canWrite = hasObject
        && !domainObject
        && accessAllowsWrite(entry->accessType)
        && interfaceAvailable
        && measurementRunning
        && engineIdle
        && ((isBooleanType(*entry))
            || ((isIntegerType(*entry) || isTextType(*entry) || isHexType(*entry)) && byteWidth(*entry) <= 4));
    const bool canDomainUpload = hasObject && domainObject && accessAllowsWrite(entry->accessType) && interfaceAvailable && measurementRunning && engineIdle;

    _btnRead->setEnabled(canRead);
    _btnWrite->setEnabled(canWrite);
    _btnDomainUpload->setEnabled(canDomainUpload);
    _btnDomainUpload->setVisible(domainObject);
    _btnSaveToFile->setEnabled(!_lastTransferData.isEmpty() && engineIdle);
    _btnSaveToFile->setVisible(!_lastTransferData.isEmpty());

    QStringList warnings;
    if (!measurementRunning)
        warnings << tr("Start a measurement to enable SDO transfers.");
    if (!interfaceAvailable)
        warnings << tr("The selected interface is unavailable.");
    if (entry)
    {
        const CanOpenDb *db = currentDb();
        if (db && db->configuredNodeId() >= 0 && db->configuredNodeId() != _nodeSpin->value())
        {
            warnings << tr("Selected node %1 does not match the EDS node %2.")
                        .arg(_nodeSpin->value())
                        .arg(db->configuredNodeId());
        }
    }
    if (!hasObject)
        warnings << tr("Select an object entry to continue.");
    if (hasObject && !domainObject && !canRead)
        warnings << tr("Read is only available for readable non-domain objects.");
    if (hasObject && !domainObject && !canWrite)
        warnings << tr("Write is limited to BOOLEAN, integer, text, or byte-array values up to 4 bytes.");
    if (hasObject && domainObject && !canDomainUpload)
        warnings << tr("Domain download requires a writable DOMAIN object, active measurement, and interface.");
    _warningLabel->setText(warnings.join(QLatin1Char('\n')));
}

void SdoWindow::setStatusMessage(const QString &text, bool error)
{
    _statusLabel->setStyleSheet(error ? QStringLiteral("color:#b00020;") : QString());
    _statusLabel->setText(text);
}

void SdoWindow::setBusy(const QString &command, const QString &message)
{
    Q_UNUSED(command);
    _outputBuffer.clear();
    _progressBar->setRange(0, 0);
    _lastTransferData.clear();
    _btnSaveToFile->setEnabled(false);
    setStatusMessage(message);
}

void SdoWindow::clearBusy()
{
    _progressBar->setRange(0, 1);
    _progressBar->setValue(1);
}

void SdoWindow::appendResultText(const QString &text)
{
    if (text.isEmpty())
        return;
    if (!_resultView->toPlainText().isEmpty())
        _resultView->appendPlainText(QString());
    _resultView->appendPlainText(text);
}

void SdoWindow::handleScriptLine(const QString &line)
{
    static const QString resultPrefix = QStringLiteral("CANGAROO_SDO_JSON:");
    static const QString errorPrefix = QStringLiteral("CANGAROO_SDO_ERROR:");

    if (line.startsWith(resultPrefix))
    {
        handleResultPayload(line.mid(resultPrefix.size()).toUtf8(), false);
        return;
    }
    if (line.startsWith(errorPrefix))
    {
        handleResultPayload(line.mid(errorPrefix.size()).toUtf8(), true);
        return;
    }

    appendResultText(line);
}

void SdoWindow::handleResultPayload(const QByteArray &jsonData, bool errorPayload)
{
    const QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (!doc.isObject())
    {
        setStatusMessage(tr("Received an invalid SDO response."), true);
        return;
    }

    const QJsonObject obj = doc.object();
    const QString command = obj.value(QStringLiteral("cmd")).toString();
    const quint16 index = static_cast<quint16>(obj.value(QStringLiteral("index")).toInt());
    const quint8 subIndex = static_cast<quint8>(obj.value(QStringLiteral("sub_index")).toInt());

    if (errorPayload)
    {
        setStatusMessage(obj.value(QStringLiteral("message")).toString(), true);
        return;
    }

    if (command == QStringLiteral("write"))
    {
        const QByteArray payload = QByteArray::fromBase64(obj.value(QStringLiteral("base64")).toString().toLatin1());
        const CanOpenDb *db = currentDb();
        const CanOpenObjectEntry *entry = db ? db->findObject(index, subIndex) : nullptr;
        if (entry)
        {
            updateCurrentValue(index, subIndex, decodeValueForDisplay(*entry, payload));
            _resultView->setPlainText(buildTransferText(*entry, payload));
        }
        setStatusMessage(tr("Write successful: %1 bytes transferred.").arg(payload.size()));
        return;
    }

    const QByteArray data = QByteArray::fromBase64(obj.value(QStringLiteral("base64")).toString().toLatin1());
    _lastTransferData = data;
    _btnSaveToFile->setEnabled(!data.isEmpty());

    const CanOpenDb *db = currentDb();
    const CanOpenObjectEntry *entry = db ? db->findObject(index, subIndex) : nullptr;
    if (entry)
    {
        updateCurrentValue(index, subIndex, decodeValueForDisplay(*entry, data));
        _resultView->setPlainText(buildTransferText(*entry, data));
    }
    else
    {
        _resultView->setPlainText(tr("Bytes: %1\nHex: %2")
                                  .arg(data.size())
                                  .arg(bytesToHex(data)));
    }

    const QString verb = (command == QStringLiteral("domain")) ? tr("Upload") : tr("Read");
    setStatusMessage(tr("%1 successful: %2 bytes transferred.").arg(verb).arg(data.size()));
}

void SdoWindow::updateCurrentValue(quint16 index, quint8 subIndex, const QString &valueText)
{
    const QString targetKey = CanOpenDb::formatObjectKey(index, subIndex);
    for (QTreeWidgetItemIterator it(_objectTree); *it; ++it)
    {
        QTreeWidgetItem *item = *it;
        if (item->data(0, RoleObjectKey).toString() == targetKey)
            item->setText(5, valueText);
    }
}

void SdoWindow::selectObjectKey(const QString &objectKey)
{
    if (objectKey.isEmpty())
        return;

    for (QTreeWidgetItemIterator it(_objectTree); *it; ++it)
    {
        QTreeWidgetItem *item = *it;
        if (item->data(0, RoleObjectKey).toString() == objectKey)
        {
            _objectTree->setCurrentItem(item);
            item->setSelected(true);
            return;
        }
    }
}

QString SdoWindow::currentObjectKey() const
{
    if (QTreeWidgetItem *item = _objectTree->currentItem())
        return item->data(0, RoleObjectKey).toString();
    return QString();
}

MeasurementNetwork *SdoWindow::currentNetwork() const
{
    const QString networkName = _networkCombo->currentData().toString();
    if (networkName.isEmpty())
        return nullptr;
    return _backend->getSetup().getNetworkByName(networkName);
}

const CanOpenDb *SdoWindow::currentDb() const
{
    MeasurementNetwork *network = currentNetwork();
    if (!network)
        return nullptr;

    const QString path = _deviceCombo->currentData().toString();
    for (const pCanOpenDb &db : network->_canOpenDbs)
    {
        if (db->path() == path)
            return db.data();
    }
    return nullptr;
}

const CanOpenObjectEntry *SdoWindow::currentObject() const
{
    QTreeWidgetItem *item = _objectTree->currentItem();
    const CanOpenDb *db = currentDb();
    if (!item || !db || !item->data(0, RoleIndex).isValid())
        return nullptr;

    return db->findObject(static_cast<quint16>(item->data(0, RoleIndex).toInt()),
                          static_cast<quint8>(item->data(0, RoleSubIndex).toInt()));
}

bool SdoWindow::prepareWritePayload(const CanOpenObjectEntry &entry, QByteArray &payload, QString &errorMessage) const
{
    if (!accessAllowsWrite(entry.accessType))
    {
        errorMessage = tr("This object is read-only.");
        return false;
    }

    if (isBooleanType(entry))
    {
        payload = QByteArray(1, static_cast<char>(_boolEditor->currentIndex() == 0 ? 0 : 1));
        return true;
    }

    if (isIntegerType(entry))
    {
        const int bits = entry.bitLength > 0 ? entry.bitLength : byteWidth(entry) * 8;
        const int width = byteWidth(entry);
        const QString text = _integerEditor->text().trimmed();
        if (text.isEmpty())
        {
            errorMessage = tr("Enter a value to write.");
            return false;
        }

        bool ok = false;
        if (entry.isSigned())
        {
            const qlonglong value = text.toLongLong(&ok, 0);
            if (!ok)
            {
                errorMessage = tr("Invalid signed integer value.");
                return false;
            }
            const qlonglong minValue = -(1LL << (bits - 1));
            const qlonglong maxValue = (1LL << (bits - 1)) - 1;
            if (value < minValue || value > maxValue)
            {
                errorMessage = tr("Value does not fit in %1 bits.").arg(bits);
                return false;
            }
            quint64 encoded = static_cast<quint64>(value);
            if (value < 0)
                encoded &= (1ULL << (width * 8)) - 1ULL;
            payload = integerToLittleEndian(encoded, width);
            return true;
        }

        const qulonglong value = text.toULongLong(&ok, 0);
        if (!ok)
        {
            errorMessage = tr("Invalid unsigned integer value.");
            return false;
        }
        const quint64 maxValue = (bits >= 64) ? std::numeric_limits<quint64>::max() : ((1ULL << bits) - 1ULL);
        if (value > maxValue)
        {
            errorMessage = tr("Value does not fit in %1 bits.").arg(bits);
            return false;
        }
        payload = integerToLittleEndian(static_cast<quint64>(value), width);
        return true;
    }

    if (isTextType(entry))
    {
        payload = _textEditor->text().toUtf8();
    }
    else if (isHexType(entry))
    {
        QString normalized = _hexEditor->text().trimmed();
        normalized.remove(QLatin1Char(' '));
        normalized.replace(QStringLiteral("0x"), QString(), Qt::CaseInsensitive);
        if (!QRegularExpression(QStringLiteral("^[0-9A-Fa-f]*$")).match(normalized).hasMatch())
        {
            errorMessage = tr("Hex payload must contain only hexadecimal digits.");
            return false;
        }
        if (normalized.size() % 2 != 0)
            normalized.prepend(QLatin1Char('0'));
        payload = QByteArray::fromHex(normalized.toLatin1());
    }
    else
    {
        errorMessage = tr("This datatype is not writable in the SDO view yet.");
        return false;
    }

    if (payload.isEmpty())
    {
        errorMessage = tr("Enter payload bytes to write.");
        return false;
    }
    if (payload.size() > 4)
    {
        errorMessage = tr("The current expedited write path supports at most 4 bytes.");
        return false;
    }
    return true;
}

QString SdoWindow::buildReadScript(const QString &command, quint16 index, quint8 subIndex, int interfaceId, int nodeId, double timeoutSec) const
{
    return QString(
        "import base64, json, cangaroo\n"
        "try:\n"
        "    res = cangaroo.sdo_read(node_id=%1, index=%2, sub_index=%3, interface_id=%4, timeout=%5)\n"
        "    data = bytes(res['data'])\n"
        "    print('CANGAROO_SDO_JSON:' + json.dumps({'cmd': '%6', 'node_id': res['node_id'], 'index': res['index'], 'sub_index': res['sub_index'], 'size': res['size'], 'raw': res['raw'], 'base64': base64.b64encode(data).decode('ascii')}))\n"
        "except Exception as exc:\n"
        "    print('CANGAROO_SDO_ERROR:' + json.dumps({'cmd': '%6', 'index': %2, 'sub_index': %3, 'message': str(exc)}))\n")
        .arg(nodeId)
        .arg(index)
        .arg(subIndex)
        .arg(interfaceId)
        .arg(timeoutSec, 0, 'f', 1)
        .arg(command);
}

QString SdoWindow::buildWriteScript(quint16 index, quint8 subIndex, int interfaceId, int nodeId, const QByteArray &payload, double timeoutSec) const
{
    return QString(
        "import base64, json, cangaroo\n"
        "payload = base64.b64decode(%1)\n"
        "try:\n"
        "    res = cangaroo.sdo_write(node_id=%2, index=%3, sub_index=%4, value=payload, size=None, interface_id=%5, timeout=%6)\n"
        "    print('CANGAROO_SDO_JSON:' + json.dumps({'cmd': 'write', 'node_id': res['node_id'], 'index': res['index'], 'sub_index': res['sub_index'], 'size': res['size'], 'raw': res['raw'], 'base64': base64.b64encode(payload).decode('ascii')}))\n"
        "except Exception as exc:\n"
        "    print('CANGAROO_SDO_ERROR:' + json.dumps({'cmd': 'write', 'index': %3, 'sub_index': %4, 'message': str(exc)}))\n")
        .arg(jsonQuoted(payload))
        .arg(nodeId)
        .arg(index)
        .arg(subIndex)
        .arg(interfaceId)
        .arg(QString::number(timeoutSec, 'f', 2));
}
