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

#include <QString>
#include <QList>
#include <QMap>
#include <QSharedPointer>
#include <QVector>

class LinFrame;

using LinFrameMap = QMap<uint8_t, LinFrame*>;

struct LinDiagTiming
{
    uint16_t p2MinMs  {25};
    uint16_t stMinMs  {0};
    uint16_t nAsMs    {1000};
    uint16_t nCrMs    {1000};
};

struct LinScheduleEntry
{
    uint8_t frameId          {0};
    QString frameName;
    QString publisherName;
    uint8_t dlc              {0};
    uint8_t delayMs          {0};
    bool    isMasterPublisher {false};
    bool    isSporadic        {false};
};

class LinDb
{
public:
    LinDb();
    ~LinDb();

    // File path helpers
    void    setPath(const QString &path);
    QString path() const;
    QString fileName() const;
    QString directory() const;

    // LDF metadata
    QString     protocolVersion() const;
    double      speedBps() const;
    QString     channelName() const;
    QString     masterNode() const;
    QStringList slaveNodes() const;
    double      masterTimebaseMs() const;
    double      masterJitterMs() const;

    // Schedule tables
    QStringList scheduleTableNames() const;
    QVector<LinScheduleEntry> scheduleTableEntries(int tableIndex) const;

    // Diagnostic timings for a node (returns defaults if node not found)
    LinDiagTiming diagTiming(const QString &nodeName) const;

    // NAD (Node Address for Diagnostics) for a slave node (returns 0 if not found)
    uint8_t nodeNad(const QString &nodeName) const;

    // Frame access
    LinFrame           *frameById(uint8_t id) const;
    LinFrame           *frameByName(const QString &name) const;
    const LinFrameMap  &frames() const;

    // Load from an LDF file. Returns false and sets lastError() on failure.
    bool    loadFile(const QString &path);
    QString lastError() const;

private:
    QString     _path;
    QString     _protocolVersion;
    double      _speedBps {19200.0};
    QString     _channelName;
    QString     _masterNode;
    QStringList _slaveNodes;
    LinFrameMap _frames;
    QStringList _scheduleTableNames;
    QVector<QVector<LinScheduleEntry>> _scheduleTables;
    QMap<QString, LinDiagTiming> _diagTimings;
    QMap<QString, uint8_t>      _nodeNads;
    double      _masterTimebaseMs {0.0};
    double      _masterJitterMs   {0.0};
    QString     _lastError;
};

using pLinDb = QSharedPointer<LinDb>;
