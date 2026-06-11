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

#include "LinDb.h"
#include "LinFrame.h"
#include "LinSignal.h"

#include <QFileInfo>

#include <type_traits>
#include <unordered_map>

// Qt defines 'signals' as 'public', which clashes with the 'signals' field
// name used in ldf_parser.h structs. This TU uses no Qt signals/slots, so
// it is safe to suppress the macro before including the parser header.
#ifdef signals
#  undef signals
#endif
#include "parser/ldf/ldf_parser.h"

LinDb::LinDb() = default;

LinDb::~LinDb()
{
    qDeleteAll(_frames);
}

void    LinDb::setPath(const QString &path) { _path = path; }
QString LinDb::path() const { return _path; }

QString LinDb::fileName() const
{
    return QFileInfo(_path).fileName();
}

QString LinDb::directory() const
{
    return QFileInfo(_path).absolutePath();
}

QString     LinDb::protocolVersion()     const { return _protocolVersion; }
double      LinDb::speedBps()            const { return _speedBps; }
QString     LinDb::channelName()         const { return _channelName; }
QString     LinDb::masterNode()          const { return _masterNode; }
QStringList LinDb::slaveNodes()          const { return _slaveNodes; }
QStringList LinDb::scheduleTableNames()  const { return _scheduleTableNames; }

QVector<LinScheduleEntry> LinDb::scheduleTableEntries(int tableIndex) const
{
    if (tableIndex < 0 || tableIndex >= _scheduleTables.size())
        return {};
    return _scheduleTables.at(tableIndex);
}
double      LinDb::masterTimebaseMs()    const { return _masterTimebaseMs; }
double      LinDb::masterJitterMs()      const { return _masterJitterMs; }

LinDiagTiming LinDb::diagTiming(const QString &nodeName) const
{
    return _diagTimings.value(nodeName);
}

uint8_t LinDb::nodeNad(const QString &nodeName) const
{
    return _nodeNads.value(nodeName, 0u);
}
QString     LinDb::lastError()           const { return _lastError; }

LinFrame *LinDb::frameById(uint8_t id) const
{
    return _frames.value(id, nullptr);
}

LinFrame *LinDb::frameByName(const QString &name) const
{
    for (LinFrame *f : _frames)
    {
        if (f->name() == name)
            return f;
    }
    return nullptr;
}

const LinFrameMap &LinDb::frames() const
{
    return _frames;
}

bool LinDb::loadFile(const QString &path)
{
    auto result = ldf::parse_file(path.toStdString());
    if (!result)
    {
        _lastError = QString::fromStdString(result.error());
        return false;
    }

    const ldf::LdfFile &ldf = *result;

    // Metadata
    _path            = path;
    _protocolVersion = QString::fromStdString(ldf.lin_protocol_version);
    _speedBps        = ldf.lin_speed_bps;
    _channelName     = QString::fromStdString(ldf.channel_name);
    _masterNode      = QString::fromStdString(ldf.nodes.master);
    _masterTimebaseMs = ldf.nodes.master_time_base_s * 1000.0;
    _masterJitterMs   = ldf.nodes.master_jitter_s    * 1000.0;

    _slaveNodes.clear();
    for (const auto &slave : ldf.nodes.slaves)
        _slaveNodes.append(QString::fromStdString(slave));

    _diagTimings.clear();
    _nodeNads.clear();
    for (const auto &attr : ldf.node_attributes)
    {
        const QString name = QString::fromStdString(attr.name);

        LinDiagTiming t;
        t.p2MinMs = static_cast<uint16_t>(attr.p2_min_s       * 1000.0);
        t.stMinMs = static_cast<uint16_t>(attr.st_min_s       * 1000.0);
        t.nAsMs   = static_cast<uint16_t>(attr.n_as_timeout_s * 1000.0);
        t.nCrMs   = static_cast<uint16_t>(attr.n_cr_timeout_s * 1000.0);
        _diagTimings.insert(name, t);

        // Prefer configured_NAD if present, then initial_NAD, then NAD.
        const uint8_t nad = attr.configured_nad.has_value() ? *attr.configured_nad
                          : attr.initial_nad.has_value()    ? *attr.initial_nad
                          : attr.nad;
        _nodeNads.insert(name, nad);
    }

    _scheduleTableNames.clear();
    _scheduleTables.clear();

    for (const auto &tbl : ldf.schedule_tables)
    {
        _scheduleTableNames.append(QString::fromStdString(tbl.name));

        // Build a lookup from sporadic frame name → its underlying unconditional frame names.
        // Used below to expand sporadic schedule entries.
        std::unordered_map<std::string, const ldf::SporadicFrame*> sporadicByName;
        for (const auto &sf : ldf.sporadic_frames)
            sporadicByName.emplace(sf.name, &sf);

        // Helper: fill resolved fields of a LinScheduleEntry from a raw ldf::Frame.
        const auto resolveFrame = [&](LinScheduleEntry &entry, const ldf::Frame &ldfFrame)
        {
            entry.frameId           = ldfFrame.id;
            entry.dlc               = ldfFrame.length;
            entry.publisherName     = QString::fromStdString(ldfFrame.publisher);
            entry.isMasterPublisher = (ldfFrame.publisher == ldf.nodes.master);
        };

        QVector<LinScheduleEntry> entries;
        for (const auto &e : tbl.entries)
        {
            if (!std::holds_alternative<ldf::UnconditionalCmd>(e.command))
                continue;

            const auto &cmd = std::get<ldf::UnconditionalCmd>(e.command);
            const uint8_t delayMs = static_cast<uint8_t>(qBound(0.0, e.delay_s * 1000.0, 255.0));

            // Check if this name refers to a sporadic frame group.
            if (auto it = sporadicByName.find(cmd.frame_name); it != sporadicByName.end())
            {
                // Expand: one LinScheduleEntry per underlying unconditional frame.
                for (const auto &underlyingName : it->second->frames)
                {
                    LinScheduleEntry entry;
                    entry.frameName  = QString::fromStdString(underlyingName);
                    entry.delayMs    = delayMs;
                    entry.isSporadic = true;

                    for (const auto &ldfFrame : ldf.frames)
                    {
                        if (ldfFrame.name == underlyingName)
                        {
                            resolveFrame(entry, ldfFrame);
                            break;
                        }
                    }

                    entries.append(entry);
                }
                continue;
            }

            // Unconditional frame — resolve directly.
            LinScheduleEntry entry;
            entry.frameName = QString::fromStdString(cmd.frame_name);
            entry.delayMs   = delayMs;

            for (const auto &ldfFrame : ldf.frames)
            {
                if (ldfFrame.name == cmd.frame_name)
                {
                    resolveFrame(entry, ldfFrame);
                    break;
                }
            }

            entries.append(entry);
        }
        _scheduleTables.append(entries);
    }

    // Build signal-name → encoding-type lookup
    std::unordered_map<std::string, const ldf::SignalEncodingType*> sigEncoding;
    for (const auto &[encName, sigNames] : ldf.signal_representation)
    {
        const ldf::SignalEncodingType *encType = nullptr;
        for (const auto &enc : ldf.signal_encoding_types)
        {
            if (enc.name == encName) { encType = &enc; break; }
        }
        if (!encType) continue;
        for (const auto &sigName : sigNames)
            sigEncoding.emplace(sigName, encType);
    }

    // Build signal-name → Signal definition lookup
    std::unordered_map<std::string, const ldf::Signal*> sigDefs;
    for (const auto &sig : ldf.signals)
        sigDefs.emplace(sig.name, &sig);

    // Clear old data and populate frames
    qDeleteAll(_frames);
    _frames.clear();

    for (const auto &ldfFrame : ldf.frames)
    {
        auto *frame = new LinFrame(this);
        frame->setId(ldfFrame.id);
        frame->setName(QString::fromStdString(ldfFrame.name));
        frame->setPublisher(QString::fromStdString(ldfFrame.publisher));
        frame->setLength(ldfFrame.length);

        for (const auto &sigRef : ldfFrame.signals)
        {
            auto *signal = new LinSignal(frame);
            signal->setBitOffset(static_cast<uint8_t>(sigRef.bit_offset));

            // Apply base signal definition
            auto defIt = sigDefs.find(sigRef.signal_name);
            if (defIt != sigDefs.end())
            {
                const ldf::Signal *def = defIt->second;
                signal->setName(QString::fromStdString(def->name));
                signal->setBitLength(static_cast<uint8_t>(def->bit_length));
                signal->setPublisher(QString::fromStdString(def->publisher));
                signal->setInitValue(def->init_value);
            }
            else
            {
                signal->setName(QString::fromStdString(sigRef.signal_name));
            }

            // Apply encoding (physical range and/or logical values)
            auto encIt = sigEncoding.find(sigRef.signal_name);
            if (encIt != sigEncoding.end())
            {
                for (const auto &encVal : encIt->second->values)
                {
                    std::visit(
                        [&](const auto &v)
                        {
                            using T = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<T, ldf::PhysicalRange>)
                            {
                                signal->setFactor(v.scale);
                                signal->setOffset(v.offset);
                                signal->setMinValue(v.min_value);
                                signal->setMaxValue(v.max_value);
                                signal->setUnit(QString::fromStdString(v.unit));
                            }
                            else if constexpr (std::is_same_v<T, ldf::LogicalValue>)
                            {
                                signal->setValueName(v.signal_value,
                                                     QString::fromStdString(v.text));
                            }
                        },
                        encVal);
                }
            }

            frame->addSignal(signal);
        }

        _frames.insert(ldfFrame.id, frame);
    }

    return true;
}
