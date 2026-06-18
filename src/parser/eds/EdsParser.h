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

#pragma once

#include <QFile>
#include <QString>
#include <QMap>
#include <stdint.h>

#include "core/DBC/CanDb.h"

class CanDbMessage;
class CanDbNode;

// Parses CANopen Electronic Data Sheet (.eds) / Device Configuration File
// (.dcf) files (EN 50325-4 / CiA 306 ini-style object dictionaries) and
// turns the predefined connection set plus configured PDOs into a CanDb,
// so CANopen traffic can be decoded using the same signal model as DBC.
class EdsParser
{
public:
    using EdsKeyValues = QMap<QString, QString>;
    using EdsSections = QMap<QString, EdsKeyValues>; // keyed by lower-case section name

    EdsParser();
    bool parseFile(QFile *file, CanDb &candb);

private:
    uint8_t _nodeId;

    static EdsSections readSections(QIODevice &device);
    static const EdsKeyValues *findSection(const EdsSections &sections, uint16_t index, int subIndex = -1);
    static QString getValue(const EdsKeyValues &kv, const QString &key, const QString &defaultValue = QString());
    static bool parseEdsInteger(const QString &valueStr, qint64 *result);
    bool parseValueWithNodeId(const QString &valueStr, qint64 *result) const;

    bool getObjectDefinition(const EdsSections &sections, uint16_t index, uint8_t subIndex, QString *name, uint16_t *dataType) const;
    static bool isUnsignedDataType(uint16_t dataType);

    void addPredefinedMessages(CanDb &candb, const EdsSections &sections, CanDbNode *deviceNode, CanDbNode *masterNode);
    void addPdoMessages(CanDb &candb, const EdsSections &sections, CanDbNode *deviceNode, CanDbNode *masterNode, bool isTransmit);
};
