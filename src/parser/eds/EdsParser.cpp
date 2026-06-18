/*

  Copyright (c) 2015, 2016 Hubert Denkmair <hubert@denkmair.de>

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

#include "EdsParser.h"

#include <QTextStream>
#include <QRegularExpression>

#include "core/Log.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/CanDbSignal.h"
#include "core/DBC/CanDbNode.h"

namespace {

QString sanitizeSignalName(const QString &raw)
{
    QString s = raw.trimmed();
    if (s.isEmpty()) {
        return QStringLiteral("Signal");
    }

    QString out;
    out.reserve(s.size());
    for (const QChar &c : s) {
        out.append((c.isLetterOrNumber() || c == '_') ? c : QChar('_'));
    }

    if (out.isEmpty()) {
        out = QStringLiteral("Signal");
    }
    if (out.at(0).isDigit()) {
        out.prepend('_');
    }
    return out;
}

} // namespace

EdsParser::EdsParser()
  : _nodeId(1)
{
}

EdsParser::EdsSections EdsParser::readSections(QIODevice &device)
{
    EdsSections sections;
    QTextStream in(&device);
    QString currentKey;

    while (!in.atEnd()) {
        const QString line = in.readLine();
        const QString trimmed = line.trimmed();

        if (trimmed.isEmpty() || trimmed.startsWith(';')) {
            continue;
        }

        if (trimmed.startsWith('[')) {
            const int end = trimmed.indexOf(']');
            if (end < 0) {
                continue;
            }
            currentKey = trimmed.mid(1, end - 1).trimmed().toLower();
            if (!sections.contains(currentKey)) {
                sections.insert(currentKey, EdsKeyValues());
            }
            continue;
        }

        if (currentKey.isEmpty()) {
            continue;
        }

        const int eq = trimmed.indexOf('=');
        if (eq < 0) {
            continue;
        }

        const QString key = trimmed.left(eq).trimmed();
        const QString value = trimmed.mid(eq + 1).trimmed();
        sections[currentKey].insert(key, value);
    }

    return sections;
}

const EdsParser::EdsKeyValues *EdsParser::findSection(const EdsSections &sections, uint16_t index, int subIndex)
{
    const QString base = QString("%1").arg(index, 4, 16, QChar('0')).toLower();
    const QString key = (subIndex < 0) ? base : QString("%1sub%2").arg(base).arg(subIndex);
    const auto it = sections.constFind(key);
    return (it != sections.constEnd()) ? &it.value() : nullptr;
}

QString EdsParser::getValue(const EdsKeyValues &kv, const QString &key, const QString &defaultValue)
{
    for (auto it = kv.constBegin(); it != kv.constEnd(); ++it) {
        if (it.key().compare(key, Qt::CaseInsensitive) == 0) {
            return it.value();
        }
    }
    return defaultValue;
}

bool EdsParser::parseEdsInteger(const QString &valueStr, qint64 *result)
{
    const QString s = valueStr.trimmed();
    if (s.isEmpty()) {
        return false;
    }

    bool ok = false;
    qint64 value = 0;

    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        value = s.mid(2).toLongLong(&ok, 16);
    } else if (s.startsWith(QStringLiteral("-0x"), Qt::CaseInsensitive)) {
        value = -s.mid(3).toLongLong(&ok, 16);
    } else {
        value = s.toLongLong(&ok, 10);
    }

    if (!ok) {
        return false;
    }

    *result = value;
    return true;
}

bool EdsParser::parseValueWithNodeId(const QString &valueStr, qint64 *result) const
{
    const QString s = valueStr.trimmed();
    if (s.isEmpty()) {
        return false;
    }

    static const QRegularExpression re(QStringLiteral("\\$?NODEID"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(s);
    if (m.hasMatch()) {
        QString remainder = s;
        remainder.remove(m.capturedStart(), m.capturedLength());
        remainder = remainder.trimmed();

        if (remainder.isEmpty()) {
            *result = _nodeId;
            return true;
        }

        if (remainder.startsWith('+')) {
            remainder.remove(0, 1);
        }

        qint64 offset = 0;
        if (!parseEdsInteger(remainder.trimmed(), &offset)) {
            return false;
        }

        *result = static_cast<qint64>(_nodeId) + offset;
        return true;
    }

    return parseEdsInteger(s, result);
}

bool EdsParser::getObjectDefinition(const EdsSections &sections, uint16_t index, uint8_t subIndex, QString *name, uint16_t *dataType) const
{
    const EdsKeyValues *kv = nullptr;
    if (subIndex == 0) {
        kv = findSection(sections, index, -1);
        if (!kv) {
            kv = findSection(sections, index, 0);
        }
    } else {
        kv = findSection(sections, index, subIndex);
        if (!kv) {
            kv = findSection(sections, index, -1);
        }
    }
    if (!kv) {
        return false;
    }

    const QString pname = getValue(*kv, "ParameterName");
    if (pname.isEmpty()) {
        return false;
    }
    *name = pname;

    qint64 dtVal = 0;
    *dataType = parseEdsInteger(getValue(*kv, "DataType"), &dtVal) ? static_cast<uint16_t>(dtVal) : 0;
    return true;
}

bool EdsParser::isUnsignedDataType(uint16_t dataType)
{
    switch (dataType) {
        case 0x0002: // INTEGER8
        case 0x0003: // INTEGER16
        case 0x0004: // INTEGER32
        case 0x0010: // INTEGER24
        case 0x0012: // INTEGER40
        case 0x0013: // INTEGER48
        case 0x0014: // INTEGER56
        case 0x0015: // INTEGER64
            return false;
        default:
            return true;
    }
}

void EdsParser::addPredefinedMessages(CanDb &candb, const EdsSections &sections, CanDbNode *deviceNode, CanDbNode *masterNode)
{
    // NMT module control - always present in the predefined connection set
    {
        CanDbMessage *msg = new CanDbMessage(&candb);
        msg->setRaw_id(0x000);
        msg->setName("NMT");
        msg->setSender(masterNode);
        msg->setDlc(2);

        CanDbSignal *cmd = new CanDbSignal(msg);
        cmd->setName("CommandSpecifier");
        cmd->setStartBit(0);
        cmd->setLength(8);
        cmd->setUnsigned(true);
        msg->addSignal(cmd);

        CanDbSignal *node = new CanDbSignal(msg);
        node->setName("NodeID");
        node->setStartBit(8);
        node->setLength(8);
        node->setUnsigned(true);
        msg->addSignal(node);

        candb.addMessage(msg);
    }

    // SYNC
    {
        qint64 cobId = 0x80;
        if (const EdsKeyValues *kv = findSection(sections, 0x1005, -1)) {
            const QString s = getValue(*kv, "ParameterValue", getValue(*kv, "DefaultValue"));
            if (!s.isEmpty()) {
                parseValueWithNodeId(s, &cobId);
            }
        }

        CanDbMessage *msg = new CanDbMessage(&candb);
        msg->setRaw_id(static_cast<uint32_t>(cobId) & 0x7FFu);
        msg->setName("SYNC");
        msg->setSender(masterNode);
        msg->setDlc(0);
        candb.addMessage(msg);
    }

    // EMCY
    {
        qint64 cobId = 0x80 + _nodeId;
        if (const EdsKeyValues *kv = findSection(sections, 0x1014, -1)) {
            const QString s = getValue(*kv, "ParameterValue", getValue(*kv, "DefaultValue"));
            if (!s.isEmpty()) {
                parseValueWithNodeId(s, &cobId);
            }
        }

        CanDbMessage *msg = new CanDbMessage(&candb);
        msg->setRaw_id(static_cast<uint32_t>(cobId) & 0x7FFu);
        msg->setName("EMCY");
        msg->setSender(deviceNode);
        msg->setDlc(8);

        CanDbSignal *errorCode = new CanDbSignal(msg);
        errorCode->setName("ErrorCode");
        errorCode->setStartBit(0);
        errorCode->setLength(16);
        errorCode->setUnsigned(true);
        msg->addSignal(errorCode);

        CanDbSignal *errorRegister = new CanDbSignal(msg);
        errorRegister->setName("ErrorRegister");
        errorRegister->setStartBit(16);
        errorRegister->setLength(8);
        errorRegister->setUnsigned(true);
        msg->addSignal(errorRegister);

        CanDbSignal *manufacturerField = new CanDbSignal(msg);
        manufacturerField->setName("ManufacturerSpecificErrorField");
        manufacturerField->setStartBit(24);
        manufacturerField->setLength(40);
        manufacturerField->setUnsigned(true);
        msg->addSignal(manufacturerField);

        candb.addMessage(msg);
    }

    // Heartbeat / boot-up - fixed by the predefined connection set
    {
        CanDbMessage *msg = new CanDbMessage(&candb);
        msg->setRaw_id((0x700u + _nodeId) & 0x7FFu);
        msg->setName("Heartbeat");
        msg->setSender(deviceNode);
        msg->setDlc(1);

        CanDbSignal *state = new CanDbSignal(msg);
        state->setName("State");
        state->setStartBit(0);
        state->setLength(8);
        state->setUnsigned(true);
        msg->addSignal(state);

        candb.addMessage(msg);
    }

    // SDO server channel (only if the object dictionary actually defines one)
    if (findSection(sections, 0x1200, -1) || findSection(sections, 0x1200, 0)) {
        qint64 rxCobId = 0x600 + _nodeId;
        qint64 txCobId = 0x580 + _nodeId;

        if (const EdsKeyValues *kv = findSection(sections, 0x1200, 1)) {
            const QString s = getValue(*kv, "ParameterValue", getValue(*kv, "DefaultValue"));
            if (!s.isEmpty()) {
                parseValueWithNodeId(s, &rxCobId);
            }
        }
        if (const EdsKeyValues *kv = findSection(sections, 0x1200, 2)) {
            const QString s = getValue(*kv, "ParameterValue", getValue(*kv, "DefaultValue"));
            if (!s.isEmpty()) {
                parseValueWithNodeId(s, &txCobId);
            }
        }

        auto addSdoMessage = [&](const QString &name, qint64 cobId, CanDbNode *sender) {
            CanDbMessage *msg = new CanDbMessage(&candb);
            msg->setRaw_id(static_cast<uint32_t>(cobId) & 0x7FFu);
            msg->setName(name);
            msg->setSender(sender);
            msg->setDlc(8);

            CanDbSignal *cmd = new CanDbSignal(msg);
            cmd->setName("CommandByte");
            cmd->setStartBit(0);
            cmd->setLength(8);
            cmd->setUnsigned(true);
            msg->addSignal(cmd);

            CanDbSignal *idx = new CanDbSignal(msg);
            idx->setName("Index");
            idx->setStartBit(8);
            idx->setLength(16);
            idx->setUnsigned(true);
            msg->addSignal(idx);

            CanDbSignal *sub = new CanDbSignal(msg);
            sub->setName("SubIndex");
            sub->setStartBit(24);
            sub->setLength(8);
            sub->setUnsigned(true);
            msg->addSignal(sub);

            CanDbSignal *data = new CanDbSignal(msg);
            data->setName("Data");
            data->setStartBit(32);
            data->setLength(32);
            data->setUnsigned(true);
            msg->addSignal(data);

            candb.addMessage(msg);
        };

        addSdoMessage("SDO_RX", rxCobId, masterNode);
        addSdoMessage("SDO_TX", txCobId, deviceNode);
    }
}

void EdsParser::addPdoMessages(CanDb &candb, const EdsSections &sections, CanDbNode *deviceNode, CanDbNode *masterNode, bool isTransmit)
{
    const uint16_t commBase = isTransmit ? 0x1800 : 0x1400;
    const uint16_t mapBase  = isTransmit ? 0x1A00 : 0x1600;

    for (int n = 0; n < 512; n++) {
        const uint16_t commIndex = static_cast<uint16_t>(commBase + n);

        const EdsKeyValues *cobKv = findSection(sections, commIndex, 1);
        if (!cobKv) {
            continue;
        }

        const QString cobStr = getValue(*cobKv, "ParameterValue", getValue(*cobKv, "DefaultValue"));
        qint64 cobRaw = 0;
        if (cobStr.isEmpty() || !parseValueWithNodeId(cobStr, &cobRaw)) {
            continue;
        }

        const uint32_t cobId = static_cast<uint32_t>(cobRaw);
        if (cobId & 0x80000000u) {
            continue; // PDO not valid / not used
        }
        const uint32_t canId = cobId & 0x7FFu;

        CanDbMessage *msg = new CanDbMessage(&candb);
        msg->setRaw_id(canId);
        msg->setName(QString("%1%2").arg(isTransmit ? "TPDO" : "RPDO").arg(n + 1));
        msg->setSender(isTransmit ? deviceNode : masterNode);

        if (const EdsKeyValues *ttKv = findSection(sections, commIndex, 2)) {
            const QString ttStr = getValue(*ttKv, "ParameterValue", getValue(*ttKv, "DefaultValue"));
            qint64 tt = 0;
            if (!ttStr.isEmpty() && parseEdsInteger(ttStr, &tt)) {
                msg->setComment(QString("Transmission type: %1").arg(tt));
            }
        }

        const uint16_t mapIndex = static_cast<uint16_t>(mapBase + n);
        int mappedCount = 0;
        if (const EdsKeyValues *mapCountKv = findSection(sections, mapIndex, 0)) {
            const QString cntStr = getValue(*mapCountKv, "ParameterValue", getValue(*mapCountKv, "DefaultValue"));
            qint64 cnt = 0;
            if (!cntStr.isEmpty() && parseEdsInteger(cntStr, &cnt)) {
                mappedCount = static_cast<int>(cnt);
            }
        }
        mappedCount = qBound(0, mappedCount, 8);

        uint16_t bitOffset = 0;
        for (int m = 1; m <= mappedCount; m++) {
            const EdsKeyValues *mapKv = findSection(sections, mapIndex, m);
            if (!mapKv) {
                break;
            }

            const QString mapStr = getValue(*mapKv, "ParameterValue", getValue(*mapKv, "DefaultValue"));
            qint64 mapRaw = 0;
            if (mapStr.isEmpty() || !parseEdsInteger(mapStr, &mapRaw)) {
                continue;
            }

            const uint32_t mapVal = static_cast<uint32_t>(mapRaw);
            const uint16_t objIndex = static_cast<uint16_t>((mapVal >> 16) & 0xFFFFu);
            const uint8_t objSubIndex = static_cast<uint8_t>((mapVal >> 8) & 0xFFu);
            const uint8_t bitLength = static_cast<uint8_t>(mapVal & 0xFFu);

            if (bitLength == 0 || (bitOffset + bitLength) > 64) {
                break;
            }

            if (objIndex == 0) {
                // dummy/padding mapping entry: reserve the bits, no signal
                bitOffset = static_cast<uint16_t>(bitOffset + bitLength);
                continue;
            }

            QString objName;
            uint16_t dataType = 0;
            if (!getObjectDefinition(sections, objIndex, objSubIndex, &objName, &dataType)) {
                objName = QString("Obj_%1_%2").arg(objIndex, 4, 16, QChar('0')).arg(objSubIndex);
            }

            QString sigName = sanitizeSignalName(objName);
            if (msg->getSignalByName(sigName)) {
                sigName = QString("%1_%2").arg(sigName).arg(m);
            }

            CanDbSignal *sig = new CanDbSignal(msg);
            sig->setName(sigName);
            sig->setStartBit(bitOffset);
            sig->setLength(bitLength);
            sig->setIsBigEndian(false);
            sig->setUnsigned(isUnsignedDataType(dataType));
            msg->addSignal(sig);

            bitOffset = static_cast<uint16_t>(bitOffset + bitLength);
        }

        // mapping not described in this file: assume a full-length PDO
        msg->setDlc(mappedCount > 0 ? static_cast<uint8_t>((bitOffset + 7) / 8) : 8);
        candb.addMessage(msg);
    }
}

bool EdsParser::parseFile(QFile *file, CanDb &candb)
{
    if (!file->isOpen() && !file->open(QIODevice::ReadOnly | QIODevice::Text)) {
        log_error(QString("could not open EDS file %1").arg(file->fileName()));
        return false;
    }

    const EdsSections sections = readSections(*file);

    _nodeId = 1;
    {
        const EdsKeyValues dc = sections.value(QStringLiteral("devicecomissioning"));
        qint64 nid = 0;
        if (parseEdsInteger(getValue(dc, "NodeID"), &nid) && nid > 0 && nid < 128) {
            _nodeId = static_cast<uint8_t>(nid);
        }
    }

    QString deviceName;
    {
        const EdsKeyValues di = sections.value(QStringLiteral("deviceinfo"));
        const QString vendor = getValue(di, "VendorName");
        const QString product = getValue(di, "ProductName");
        if (!vendor.isEmpty() && !product.isEmpty()) {
            deviceName = QString("%1 %2").arg(vendor, product);
        } else if (!product.isEmpty()) {
            deviceName = product;
        } else if (!vendor.isEmpty()) {
            deviceName = vendor;
        } else {
            deviceName = QString("CANopen_Node_%1").arg(_nodeId);
        }
    }

    candb.setPath(file->fileName());
    CanDbNode *deviceNode = candb.getOrCreateNode(deviceName);
    CanDbNode *masterNode = candb.getOrCreateNode("Master");

    addPredefinedMessages(candb, sections, deviceNode, masterNode);
    addPdoMessages(candb, sections, deviceNode, masterNode, true);
    addPdoMessages(candb, sections, deviceNode, masterNode, false);

    return true;
}
