#include "CanOpenDb.h"

#include <QFileInfo>
#include <QRegularExpression>

#include "parser/eds/CanOpenEdsParser.h"

namespace {
quint32 objectKey(quint16 index, quint8 subIndex)
{
    return (static_cast<quint32>(index) << 8) | subIndex;
}
}

CanOpenDb::CanOpenDb() = default;

bool CanOpenObjectEntry::isSigned() const
{
    return CanOpenDb::isSignedDataType(dataType);
}

bool CanOpenDb::loadFile(const QString &path)
{
    clear();
    CanOpenEdsParser parser;
    QString error;
    if (!parser.parseFile(path, *this, &error)) {
        _lastError = error;
        return false;
    }
    _path = path;
    return true;
}

QString CanOpenDb::lastError() const
{
    return _lastError;
}

void CanOpenDb::clear()
{
    _path.clear();
    _lastError.clear();
    _deviceName.clear();
    _vendorName.clear();
    _productName.clear();
    _configuredNodeId = -1;
    _objects.clear();
    _tpdos.clear();
    _rpdos.clear();
}

QString CanOpenDb::path() const { return _path; }
void CanOpenDb::setPath(const QString &path) { _path = path; }
QString CanOpenDb::fileName() const { return QFileInfo(_path).fileName(); }
QString CanOpenDb::directory() const { return QFileInfo(_path).absolutePath(); }
QString CanOpenDb::deviceName() const { return _deviceName; }
void CanOpenDb::setDeviceName(const QString &name) { _deviceName = name; }
QString CanOpenDb::vendorName() const { return _vendorName; }
void CanOpenDb::setVendorName(const QString &name) { _vendorName = name; }
QString CanOpenDb::productName() const { return _productName; }
void CanOpenDb::setProductName(const QString &name) { _productName = name; }
int CanOpenDb::configuredNodeId() const { return _configuredNodeId; }
void CanOpenDb::setConfiguredNodeId(int nodeId) { _configuredNodeId = nodeId; }
const CanOpenObjectMap &CanOpenDb::objects() const { return _objects; }
const QList<CanOpenPdo> &CanOpenDb::tpdos() const { return _tpdos; }
const QList<CanOpenPdo> &CanOpenDb::rpdos() const { return _rpdos; }

void CanOpenDb::addObject(const CanOpenObjectEntry &entry)
{
    _objects.insert(objectKey(entry.index, entry.subIndex), entry);
}

const CanOpenObjectEntry *CanOpenDb::findObject(quint16 index, quint8 subIndex) const
{
    const auto it = _objects.constFind(objectKey(index, subIndex));
    return (it == _objects.constEnd()) ? nullptr : &it.value();
}

void CanOpenDb::addPdo(const CanOpenPdo &pdo)
{
    if (pdo.isTransmit) {
        _tpdos.append(pdo);
    } else {
        _rpdos.append(pdo);
    }
}

QList<const CanOpenPdo*> CanOpenDb::findMatchingPdos(quint32 cobId, quint8 nodeId, bool transmit) const
{
    QList<const CanOpenPdo*> matches;
    const QList<CanOpenPdo> &pdos = transmit ? _tpdos : _rpdos;
    for (const CanOpenPdo &pdo : pdos) {
        quint32 resolvedCobId = 0;
        if (resolveCobId(pdo, nodeId, &resolvedCobId) && (resolvedCobId & 0x7FFu) == (cobId & 0x7FFu)) {
            matches.append(&pdo);
        }
    }
    return matches;
}

bool CanOpenDb::resolveCobId(const CanOpenPdo &pdo, quint8 nodeId, quint32 *cobId) const
{
    if (!cobId) {
        return false;
    }

    quint32 value = 0;
    if (!parseIntegerExpression(pdo.cobIdExpression, nodeId, &value)) {
        return false;
    }
    if (value & 0x80000000u) {
        return false;
    }
    *cobId = value & 0x7FFFFFFFu;
    return true;
}

QString CanOpenDb::formatObjectKey(quint16 index, quint8 subIndex)
{
    return QStringLiteral("0x%1:%2")
        .arg(index, 4, 16, QChar('0'))
        .arg(subIndex, 2, 16, QChar('0'))
        .toUpper();
}

QString CanOpenDb::dataTypeName(quint16 dataType)
{
    switch (dataType) {
    case 0x0001: return QStringLiteral("BOOLEAN");
    case 0x0002: return QStringLiteral("INTEGER8");
    case 0x0003: return QStringLiteral("INTEGER16");
    case 0x0004: return QStringLiteral("INTEGER32");
    case 0x0005: return QStringLiteral("UNSIGNED8");
    case 0x0006: return QStringLiteral("UNSIGNED16");
    case 0x0007: return QStringLiteral("UNSIGNED32");
    case 0x0008: return QStringLiteral("REAL32");
    case 0x0009: return QStringLiteral("VISIBLE_STRING");
    case 0x000A: return QStringLiteral("OCTET_STRING");
    case 0x000B: return QStringLiteral("UNICODE_STRING");
    case 0x000C: return QStringLiteral("TIME_OF_DAY");
    case 0x000D: return QStringLiteral("TIME_DIFFERENCE");
    case 0x000F: return QStringLiteral("DOMAIN");
    case 0x0010: return QStringLiteral("INTEGER24");
    case 0x0011: return QStringLiteral("REAL64");
    case 0x0012: return QStringLiteral("INTEGER40");
    case 0x0013: return QStringLiteral("INTEGER48");
    case 0x0014: return QStringLiteral("INTEGER56");
    case 0x0015: return QStringLiteral("INTEGER64");
    case 0x0016: return QStringLiteral("UNSIGNED24");
    case 0x0018: return QStringLiteral("UNSIGNED40");
    case 0x0019: return QStringLiteral("UNSIGNED48");
    case 0x001A: return QStringLiteral("UNSIGNED56");
    case 0x001B: return QStringLiteral("UNSIGNED64");
    default:
        return QStringLiteral("0x%1").arg(dataType, 4, 16, QChar('0')).toUpper();
    }
}

quint8 CanOpenDb::bitLengthForDataType(quint16 dataType)
{
    switch (dataType) {
    case 0x0001: return 1;
    case 0x0002:
    case 0x0005: return 8;
    case 0x0003:
    case 0x0006: return 16;
    case 0x0004:
    case 0x0007:
    case 0x0008: return 32;
    case 0x0010:
    case 0x0016: return 24;
    case 0x0011:
    case 0x0015:
    case 0x001B: return 64;
    case 0x0012:
    case 0x0018: return 40;
    case 0x0013:
    case 0x0019: return 48;
    case 0x0014:
    case 0x001A: return 56;
    default: return 0;
    }
}

bool CanOpenDb::isSignedDataType(quint16 dataType)
{
    switch (dataType) {
    case 0x0002:
    case 0x0003:
    case 0x0004:
    case 0x0010:
    case 0x0012:
    case 0x0013:
    case 0x0014:
    case 0x0015:
        return true;
    default:
        return false;
    }
}

bool CanOpenDb::parseIntegerExpression(const QString &text, quint8 nodeId, quint32 *value)
{
    if (!value) {
        return false;
    }

    QString expr = text.trimmed();
    if (expr.isEmpty()) {
        return false;
    }

    expr.replace(QRegularExpression(QStringLiteral("\\s+")), QString());
    expr.replace(QStringLiteral("$NODEID"), QString::number(nodeId));
    expr.replace(QStringLiteral("$NodeID"), QString::number(nodeId));
    expr.replace(QStringLiteral("$nodeid"), QString::number(nodeId));

    quint64 total = 0;
    int sign = +1;
    int pos = 0;
    while (pos < expr.size()) {
        if (expr.at(pos) == QLatin1Char('+')) {
            sign = +1;
            ++pos;
            continue;
        }
        if (expr.at(pos) == QLatin1Char('-')) {
            sign = -1;
            ++pos;
            continue;
        }

        int next = pos;
        while (next < expr.size() && expr.at(next) != QLatin1Char('+') && expr.at(next) != QLatin1Char('-')) {
            ++next;
        }

        const QString token = expr.mid(pos, next - pos);
        bool ok = false;
        const bool isHex = token.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
        quint64 part = isHex
            ? ((token.size() > 2) ? token.mid(2).toULongLong(&ok, 16) : 0)
            : token.toULongLong(&ok, 10);
        if (!ok) {
            return false;
        }

        if (sign < 0) {
            if (part > total) {
                return false;
            }
            total -= part;
        } else {
            total += part;
        }
        pos = next;
    }

    *value = static_cast<quint32>(total & 0xFFFFFFFFu);
    return true;
}
