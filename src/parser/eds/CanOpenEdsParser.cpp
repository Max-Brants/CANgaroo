#include "CanOpenEdsParser.h"

#include <QRegularExpression>
#include <QSettings>

#include "core/DBC/CanOpenDb.h"

namespace {
quint16 parseHexWord(const QString &text, bool *ok = nullptr)
{
    bool localOk = false;
    quint16 value = text.toUShort(&localOk, 16);
    if (ok) {
        *ok = localOk;
    }
    return value;
}

quint32 parseValue(const CanOpenObjectEntry &entry)
{
    quint32 value = 0;
    CanOpenDb::parseIntegerExpression(entry.effectiveValue(), 0, &value);
    return value;
}
}

bool CanOpenEdsParser::parseFile(const QString &path, CanOpenDb &db, QString *errorMsg) const
{
    QSettings ini(path, QSettings::IniFormat);

    if (ini.status() != QSettings::NoError) {
        if (errorMsg) {
            *errorMsg = QStringLiteral("Failed to read EDS file.");
        }
        return false;
    }

    db.setPath(path);
    db.setDeviceName(ini.value(QStringLiteral("DeviceInfo/ProductName")).toString());
    db.setVendorName(ini.value(QStringLiteral("DeviceInfo/VendorName")).toString());
    db.setProductName(ini.value(QStringLiteral("DeviceInfo/ProductName")).toString());

    const QString nodeIdText = ini.value(QStringLiteral("DeviceCommissioning/NodeID")).toString().trimmed();
    if (!nodeIdText.isEmpty()) {
        quint32 nodeId = 0;
        if (CanOpenDb::parseIntegerExpression(nodeIdText, 0, &nodeId)) {
            db.setConfiguredNodeId(static_cast<int>(nodeId & 0xFFu));
        }
    }

    static const QRegularExpression objectSectionRe(QStringLiteral("^([0-9A-Fa-f]{4})(?:sub([0-9A-Fa-f]{1,2}))?$"));

    const QStringList groups = ini.childGroups();
    for (const QString &group : groups) {
        const QRegularExpressionMatch match = objectSectionRe.match(group);
        if (!match.hasMatch()) {
            continue;
        }

        bool ok = false;
        const quint16 index = parseHexWord(match.captured(1), &ok);
        if (!ok) {
            continue;
        }
        const quint8 subIndex = match.captured(2).isEmpty()
            ? 0
            : static_cast<quint8>(match.captured(2).toUInt(&ok, 16));
        if (!ok && !match.captured(2).isEmpty()) {
            continue;
        }

        ini.beginGroup(group);

        CanOpenObjectEntry entry;
        entry.index = index;
        entry.subIndex = subIndex;
        entry.name = ini.value(QStringLiteral("ParameterName"), ini.value(QStringLiteral("ObjectName"))).toString();
        entry.objectType = ini.value(QStringLiteral("ObjectType")).toString();
        entry.dataType = static_cast<quint16>(ini.value(QStringLiteral("DataType")).toString().toUInt(&ok, 0));
        if (!ok) {
            entry.dataType = static_cast<quint16>(ini.value(QStringLiteral("DataType")).toString().toUInt(&ok, 16));
        }
        if (!ok) {
            entry.dataType = 0;
        }
        entry.dataTypeName = CanOpenDb::dataTypeName(entry.dataType);

        entry.bitLength = static_cast<quint8>(ini.value(QStringLiteral("BitLength")).toUInt(&ok));
        if (!ok || entry.bitLength == 0) {
            entry.bitLength = CanOpenDb::bitLengthForDataType(entry.dataType);
        }
        entry.accessType = ini.value(QStringLiteral("AccessType")).toString();
        entry.defaultValue = ini.value(QStringLiteral("DefaultValue")).toString().trimmed();
        entry.parameterValue = ini.value(QStringLiteral("ParameterValue")).toString().trimmed();

        const QString pdoMap = ini.value(QStringLiteral("PDOMapping")).toString().trimmed();
        entry.pdoMappable = pdoMap == QStringLiteral("1") || pdoMap.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;

        db.addObject(entry);
        ini.endGroup();
    }

    auto appendPdo = [&](bool transmit, quint16 commBase, quint16 mapBase)
    {
        for (int pdoIdx = 0; pdoIdx < 4; ++pdoIdx) {
            const quint16 commIndex = static_cast<quint16>(commBase + pdoIdx);
            const quint16 mapIndex = static_cast<quint16>(mapBase + pdoIdx);

            const CanOpenObjectEntry *cobIdEntry = db.findObject(commIndex, 1);
            const CanOpenObjectEntry *transmissionTypeEntry = db.findObject(commIndex, 2);
            const CanOpenObjectEntry *mapCountEntry = db.findObject(mapIndex, 0);
            if (!cobIdEntry && !mapCountEntry) {
                continue;
            }

            CanOpenPdo pdo;
            pdo.isTransmit = transmit;
            pdo.number = static_cast<quint8>(pdoIdx + 1);
            pdo.communicationIndex = commIndex;
            pdo.mappingIndex = mapIndex;
            if (cobIdEntry && !cobIdEntry->effectiveValue().isEmpty()) {
                pdo.cobIdExpression = cobIdEntry->effectiveValue();
            } else {
                static const quint16 kTpdoBases[] = { 0x180, 0x280, 0x380, 0x480 };
                static const quint16 kRpdoBases[] = { 0x200, 0x300, 0x400, 0x500 };
                const quint16 base = transmit ? kTpdoBases[pdoIdx] : kRpdoBases[pdoIdx];
                pdo.cobIdExpression = QStringLiteral("$NODEID+0x%1").arg(base, 0, 16);
            }
            pdo.transmissionType = transmissionTypeEntry
                ? static_cast<quint8>(parseValue(*transmissionTypeEntry) & 0xFFu)
                : 0xFF;

            const int mapCount = mapCountEntry ? static_cast<int>(parseValue(*mapCountEntry) & 0xFFu) : 0;
            for (int sub = 1; sub <= mapCount; ++sub) {
                const CanOpenObjectEntry *mapEntry = db.findObject(mapIndex, static_cast<quint8>(sub));
                if (!mapEntry) {
                    continue;
                }
                quint32 raw = 0;
                if (!CanOpenDb::parseIntegerExpression(mapEntry->effectiveValue(), 0, &raw) || raw == 0) {
                    continue;
                }

                CanOpenPdoMapping mapping;
                mapping.objectIndex = static_cast<quint16>((raw >> 16) & 0xFFFFu);
                mapping.objectSubIndex = static_cast<quint8>((raw >> 8) & 0xFFu);
                mapping.bitLength = static_cast<quint8>(raw & 0xFFu);
                if (const CanOpenObjectEntry *obj = db.findObject(mapping.objectIndex, mapping.objectSubIndex)) {
                    mapping.objectName = obj->name;
                    mapping.dataType = obj->dataType;
                    mapping.dataTypeName = obj->dataTypeName;
                    mapping.isSigned = obj->isSigned();
                    if (mapping.bitLength == 0) {
                        mapping.bitLength = obj->bitLength;
                    }
                }
                pdo.mappings.append(mapping);
            }
            db.addPdo(pdo);
        }
    };

    appendPdo(false, 0x1400, 0x1600);
    appendPdo(true,  0x1800, 0x1A00);

    if (db.objects().isEmpty()) {
        if (errorMsg) {
            *errorMsg = QStringLiteral("EDS file does not contain any object dictionary entries.");
        }
        return false;
    }

    return true;
}
