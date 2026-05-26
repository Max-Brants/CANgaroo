#pragma once

#include <QList>
#include <QMap>
#include <QSharedPointer>
#include <QString>
#include <QVariantMap>

class BusMessage;

struct CanOpenObjectEntry
{
    quint16 index = 0;
    quint8 subIndex = 0;
    QString name;
    QString objectType;
    quint16 dataType = 0;
    QString dataTypeName;
    quint8 bitLength = 0;
    QString accessType;
    QString defaultValue;
    QString parameterValue;
    bool pdoMappable = false;

    QString effectiveValue() const
    {
        return parameterValue.isEmpty() ? defaultValue : parameterValue;
    }

    bool isSigned() const;
};

struct CanOpenPdoMapping
{
    quint16 objectIndex = 0;
    quint8 objectSubIndex = 0;
    quint8 bitLength = 0;
    QString objectName;
    quint16 dataType = 0;
    QString dataTypeName;
    bool isSigned = false;
};

struct CanOpenPdo
{
    bool isTransmit = true;
    quint8 number = 0;
    quint16 communicationIndex = 0;
    quint16 mappingIndex = 0;
    QString cobIdExpression;
    quint8 transmissionType = 0xFF;
    QList<CanOpenPdoMapping> mappings;
};

using CanOpenObjectMap = QMap<quint32, CanOpenObjectEntry>;

class CanOpenDb
{
public:
    CanOpenDb();

    bool loadFile(const QString &path);
    QString lastError() const;

    void clear();

    QString path() const;
    void setPath(const QString &path);
    QString fileName() const;
    QString directory() const;

    QString deviceName() const;
    void setDeviceName(const QString &name);
    QString vendorName() const;
    void setVendorName(const QString &name);
    QString productName() const;
    void setProductName(const QString &name);

    int configuredNodeId() const;
    void setConfiguredNodeId(int nodeId);

    const CanOpenObjectMap &objects() const;
    void addObject(const CanOpenObjectEntry &entry);
    const CanOpenObjectEntry *findObject(quint16 index, quint8 subIndex = 0) const;

    const QList<CanOpenPdo> &tpdos() const;
    const QList<CanOpenPdo> &rpdos() const;
    void addPdo(const CanOpenPdo &pdo);
    QList<const CanOpenPdo*> findMatchingPdos(quint32 cobId, quint8 nodeId, bool transmit) const;
    bool resolveCobId(const CanOpenPdo &pdo, quint8 nodeId, quint32 *cobId) const;

    static QString formatObjectKey(quint16 index, quint8 subIndex);
    static QString dataTypeName(quint16 dataType);
    static quint8 bitLengthForDataType(quint16 dataType);
    static bool isSignedDataType(quint16 dataType);
    static bool parseIntegerExpression(const QString &text, quint8 nodeId, quint32 *value);

private:
    QString _path;
    QString _lastError;
    QString _deviceName;
    QString _vendorName;
    QString _productName;
    int _configuredNodeId = -1;
    CanOpenObjectMap _objects;
    QList<CanOpenPdo> _tpdos;
    QList<CanOpenPdo> _rpdos;
};

using pCanOpenDb = QSharedPointer<CanOpenDb>;
