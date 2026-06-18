// pybind11/Python must come before Qt headers to avoid "slots" macro clash
#undef slots
#include <pybind11/embed.h>
#include <pybind11/stl.h>
#define slots Q_SLOTS

#include "PythonEngine.h"

#include <array>
#include <cstring>
#include <functional>

#include "core/portable_endian.h"
#include "core/AutosarE2E.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QStandardPaths>

#include "core/Backend.h"
#include "core/BusTrace.h"
#include "core/DBC/CanDb.h"
#include "core/DBC/CanDbMessage.h"
#include "core/DBC/CanDbSignal.h"
#include "core/DBC/CanOpenDb.h"
#include "core/DBC/LinDb.h"
#include "core/DBC/LinFrame.h"
#include "core/DBC/LinSignal.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/Log.h"
#include "driver/BusInterface.h"
#include "decoders/CanOpenDecoder.h"

namespace py = pybind11;
using namespace py::literals;

// ---------------------------------------------------------------------------
// Global pointer so the embedded module can reach the active engine
// ---------------------------------------------------------------------------
static PythonEngine *g_activeEngine = nullptr;

static void emitScriptOutputLine(PythonEngine *engine, const QString &text)
{
    if (!engine) {
        return;
    }

    QMetaObject::invokeMethod(engine, [engine, text]()
    {
        emit engine->scriptOutput(text);
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Helper: pack a raw value into a BusMessage at the given signal position.
// This is the inverse of BusMessage::extractRawSignal.
// ---------------------------------------------------------------------------
static void insertRawSignalIntoMsg(BusMessage &msg,
                                   uint16_t start_bit,
                                   uint16_t length,
                                   bool isBigEndian,
                                   uint64_t raw) noexcept
{
    if (length == 0 || start_bit >= BusMessage::k_maxDataBytes * 8) { return; }

    if (isBigEndian && length > 8)
    {
        // Inverse of extractRawSignal's big-endian path:
        //   extract: result = bswap64((data_raw >> bit_shift) & mask) >> (64 - length)
        //   insert:  A      = bswap64(raw << (64 - length))
        // where A is the Intel-order value to place at bit_shift in data_raw.
        raw <<= (64 - length);
        raw = __builtin_bswap64(raw);
    }

    const uint64_t mask       = (length < 64) ? ((1ULL << length) - 1) : ~0ULL;
    const int      byte_offset = start_bit / 8;
    const int      bit_shift   = start_bit % 8;

    // Mirror extractRawSignal: limit copy to the actual buffer size (64 bytes for FD).
    const int copy_len = std::min(8, 64 - byte_offset);

    uint8_t temp[8] = {0};
    for (int i = 0; i < copy_len; i++)
    {
        temp[i] = msg.getByte(static_cast<uint8_t>(byte_offset + i));
    }

    uint64_t data_raw;
    memcpy(&data_raw, temp, sizeof(data_raw));
    data_raw = le64toh(data_raw);
    data_raw &= ~(mask << bit_shift);
    data_raw |= (raw & mask) << bit_shift;
    data_raw = htole64(data_raw);
    memcpy(temp, &data_raw, sizeof(data_raw));

    for (int i = 0; i < copy_len; i++)
    {
        msg.setByte(static_cast<uint8_t>(byte_offset + i), temp[i]);
    }
}

// ---------------------------------------------------------------------------
// Helper: find a CanDbMessage by name across all loaded DBs
// ---------------------------------------------------------------------------
static CanDbMessage *findDbMessageByName(Backend &backend, const QString &name)
{
    MeasurementSetup &setup = backend.getSetup();
    for (MeasurementNetwork *net : setup.getNetworks())
    {
        for (const pCanDb &db : std::as_const(net->_canDbs))
        {
            for (CanDbMessage *dbMsg : db->getMessageList())
            {
                if (dbMsg->getName() == name)
                {
                    return dbMsg;
                }
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: build the signal-definition dict used by lookup() and find_message()
// ---------------------------------------------------------------------------
static py::dict buildMessageDict(CanDbMessage *dbMsg)
{
    py::list sigList;
    for (CanDbSignal *sig : dbMsg->getSignals())
    {
        py::dict s;
        s["name"]         = sig->name().toStdString();
        s["start_bit"]    = sig->startBit();
        s["length"]       = sig->length();
        s["is_big_endian"]= sig->isBigEndian();
        s["is_unsigned"]  = sig->isUnsigned();
        s["factor"]       = sig->getFactor();
        s["offset"]       = sig->getOffset();
        s["min"]          = sig->getMinimumValue();
        s["max"]          = sig->getMaximumValue();
        s["unit"]         = sig->getUnit().toStdString();
        s["comment"]      = sig->comment().toStdString();
        if (sig->isMuxed())  { s["mux_value"] = sig->getMuxValue(); }
        if (sig->isMuxer())  { s["is_muxer"]  = true; }
        sigList.append(s);
    }

    py::dict result;
    result["message"] = dbMsg->getName().toStdString();
    result["id"]      = dbMsg->getRaw_id();
    result["dlc"]     = dbMsg->getDlc();
    result["comment"] = dbMsg->getComment().toStdString();
    result["signals"] = sigList;

    CanDbNode *sender = dbMsg->getSender();
    if (sender) { result["sender"] = sender->name().toStdString(); }

    return result;
}

// ---------------------------------------------------------------------------
// Embedded "cangaroo" Python module
// ---------------------------------------------------------------------------
PYBIND11_EMBEDDED_MODULE(cangaroo, m)
{
    // --- BusMessage binding ---
    py::class_<BusMessage>(m, "Message")
        .def(py::init<>())
        .def(py::init<uint32_t>())
        .def_property("id",        &BusMessage::getId,       &BusMessage::setId)
        .def_property("dlc",       &BusMessage::getLength,   &BusMessage::setLength)
        .def_property("extended",  &BusMessage::isExtended,  &BusMessage::setExtended)
        .def_property("fd",        &BusMessage::isFD,        &BusMessage::setFD)
        .def_property("rtr",       &BusMessage::isRTR,       &BusMessage::setRTR)
        .def_property("brs",       &BusMessage::isBRS,       &BusMessage::setBRS)
        .def_property_readonly("interface_id", &BusMessage::getInterfaceId)
        .def_property_readonly("timestamp",    &BusMessage::getFloatTimestamp)
        .def_property_readonly("is_rx",        &BusMessage::isRX)
        .def_property_readonly("is_lin_sleep",  &BusMessage::isLinSleepFrame)
        .def_property_readonly("is_lin_wakeup", &BusMessage::isLinWakeupFrame)
        .def("get_byte", &BusMessage::getByte)
        .def("set_byte", &BusMessage::setByte)
        .def("get_data", [](const BusMessage &msg) -> py::bytes
        {
            std::string data(msg.getLength(), '\0');
            for (int i = 0; i < msg.getLength(); i++)
            {
                data[i] = static_cast<char>(msg.getByte(i));
            }
            return py::bytes(data);
        })
        .def("set_data", [](BusMessage &msg, py::bytes data)
        {
            std::string s = data;
            msg.setLength(static_cast<uint8_t>(std::min<size_t>(s.size(), 64)));
            for (size_t i = 0; i < s.size() && i < 64; i++)
            {
                msg.setByte(i, static_cast<uint8_t>(s[i]));
            }
        })
        .def_property("bustype",
            [](const BusMessage &msg) -> std::string
            {
                return msg.busType() == BusType::LIN ? "LIN" : "CAN";
            },
            [](BusMessage &msg, const std::string &s)
            {
                msg.setBusType(s == "LIN" ? BusType::LIN : BusType::CAN);
            })
        .def("__repr__", [](const BusMessage &msg) -> std::string
        {
            const std::string type = msg.busType() == BusType::LIN ? "LinMessage" : "Message";
            return "<cangaroo." + type + " id=0x"
                   + msg.getIdString().toStdString()
                   + " dlc=" + std::to_string(msg.getLength())
                   + " data=" + msg.getDataHexString().toStdString() + ">";
        });

    m.def("make_lin_message", [](uint8_t id, uint8_t dlc) -> BusMessage
    {
        BusMessage msg(id);
        msg.setBusType(BusType::LIN);
        msg.setLength(dlc);
        return msg;
    }, py::arg("id"), py::arg("dlc") = 0);

    // --- send / receive ---

    m.def("lin_sleep", [](uint16_t interface_id)
    {
        if (!g_activeEngine) { return; }
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (intf)
            intf->sendLinSleepWakeup(false);
    }, py::arg("interface_id") = 0);

    m.def("lin_wakeup", [](uint16_t interface_id)
    {
        if (!g_activeEngine) { return; }
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (intf)
            intf->sendLinSleepWakeup(true);
    }, py::arg("interface_id") = 0);

    m.def("send", [](BusMessage &msg, uint16_t interface_id)
    {
        if (!g_activeEngine) { return; }
        msg.setInterfaceId(interface_id);
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (intf)
        {
            intf->sendMessage(msg);
        }
    }, py::arg("msg"), py::arg("interface_id") = 0);

    m.def("receive", [](double timeout_sec) -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        const unsigned long wait_ms = static_cast<unsigned long>(timeout_sec * 1000);

        {
            // Destructor order is LIFO: lck destructs first (releases QMutex),
            // then release destructs (re-acquires GIL). This ordering is required:
            // the GIL must be released before acquiring any Qt mutex to prevent a
            // deadlock where the CanListener thread holds _msgQueueMutex while the
            // main thread waits for the GIL.
            py::gil_scoped_release release;
            QMutexLocker lck(&g_activeEngine->msgQueueMutex());
            QQueue<BusMessage> &q = g_activeEngine->msgQueue();

            if (q.isEmpty() && !g_activeEngine->stopRequested())
            {
                g_activeEngine->msgQueueCondition().wait(
                    &g_activeEngine->msgQueueMutex(), wait_ms);
            }
        }

        {
            QMutexLocker lck(&g_activeEngine->msgQueueMutex());
            QQueue<BusMessage> &q = g_activeEngine->msgQueue();
            while (!q.isEmpty())
            {
                result.append(q.dequeue());
            }
        }

        return result;
    }, py::arg("timeout") = 1.0);

    // --- RX filter (applied before messages enter the receive() queue) ---

    m.def("set_filter", [](uint32_t id, uint32_t mask, py::object extended)
    {
        if (!g_activeEngine) { return; }
        std::optional<bool> ext;
        if (!extended.is_none())
        {
            ext = extended.cast<bool>();
        }
        g_activeEngine->setRxFilter(id, mask, ext);
    },
    py::arg("id"),
    py::arg("mask")     = 0xFFFFFFFFu,
    py::arg("extended") = py::none());

    m.def("clear_filter", []()
    {
        if (g_activeEngine) { g_activeEngine->clearRxFilter(); }
    });

    // By default TX frames (echo-back of sent messages) are excluded from receive().
    // Call enable_tx_echo(True) to include them.
    m.def("enable_tx_echo", [](bool enabled)
    {
        if (g_activeEngine) { g_activeEngine->setTxEchoEnabled(enabled); }
    }, py::arg("enabled") = true);

    // --- Periodic TX ---

    m.def("send_periodic", [](BusMessage msg, unsigned interval_ms, uint16_t interface_id) -> int
    {
        if (!g_activeEngine) { return -1; }
        return g_activeEngine->startPeriodicTask(msg, interval_ms, interface_id);
    },
    py::arg("msg"),
    py::arg("interval_ms"),
    py::arg("interface_id") = 0);

    m.def("stop_periodic", [](int handle)
    {
        if (g_activeEngine) { g_activeEngine->stopPeriodicTask(handle); }
    }, py::arg("handle"));

    // --- Trace access ---

    m.def("get_trace", [](int count) -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        // getSnapshot holds the BusTrace mutex for the entire copy, avoiding a
        // TOCTOU race between size() and getMessage() calls.
        QVector<BusMessage> snapshot;
        {
            py::gil_scoped_release release;
            snapshot = g_activeEngine->backend().getTrace()->getSnapshot(count);
        }

        for (const BusMessage &msg : std::as_const(snapshot))
        {
            result.append(msg);
        }
        return result;
    }, py::arg("count") = 0);

    m.def("trace_size", []() -> int
    {
        if (!g_activeEngine) { return 0; }
        return static_cast<int>(g_activeEngine->backend().getTrace()->size());
    });

    m.def("clear_trace", []()
    {
        if (!g_activeEngine) { return; }
        Backend &backend = g_activeEngine->backend();
        py::gil_scoped_release release;  // must not hold GIL while blocking on main thread
        QMetaObject::invokeMethod(&backend, [&backend]()
        {
            backend.clearTrace();
        }, Qt::BlockingQueuedConnection);
    });

    // --- Measurement control ---

    m.def("measurement_running", []() -> bool
    {
        if (!g_activeEngine) { return false; }
        return g_activeEngine->backend().isMeasurementRunning();
    });

    m.def("start_measurement", []() -> bool
    {
        if (!g_activeEngine) { return false; }
        Backend &backend = g_activeEngine->backend();
        bool result = false;
        {
            py::gil_scoped_release release;  // must not hold GIL while blocking on main thread
            QMetaObject::invokeMethod(&backend, [&backend, &result]()
            {
                result = backend.startMeasurement();
            }, Qt::BlockingQueuedConnection);
        }
        return result;
    });

    m.def("stop_measurement", []() -> bool
    {
        if (!g_activeEngine) { return false; }
        Backend &backend = g_activeEngine->backend();
        bool result = false;
        {
            py::gil_scoped_release release;  // must not hold GIL while blocking on main thread
            QMetaObject::invokeMethod(&backend, [&backend, &result]()
            {
                result = backend.stopMeasurement();
            }, Qt::BlockingQueuedConnection);
        }
        return result;
    });

    // --- Interface helpers ---

    m.def("interfaces", []() -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }
        for (BusInterfaceId id : g_activeEngine->backend().getInterfaceList())
        {
            py::dict d;
            d["id"]   = id;
            d["name"] = g_activeEngine->backend().getInterfaceName(id).toStdString();
            BusInterface *intf = g_activeEngine->backend().getInterfaceById(id);
            d["bus_type"] = (intf && intf->busType() == BusType::LIN) ? "LIN" : "CAN";
            d["state"]    = intf ? intf->getStateText().toStdString() : "";
            result.append(d);
        }
        return result;
    });

    m.def("interface_name", [](uint16_t id) -> std::string
    {
        if (!g_activeEngine) { return ""; }
        return g_activeEngine->backend().getInterfaceName(id).toStdString();
    });

    m.def("interface_state", [](uint16_t interface_id) -> std::string
    {
        if (!g_activeEngine) { return ""; }
        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        return intf ? intf->getStateText().toStdString() : "";
    }, py::arg("interface_id"));

    // --- Logging ---

    m.def("log", [](const std::string &text)
    {
        if (g_activeEngine) { log_info(QString::fromStdString(text)); }
    });

    m.def("log_info", [](const std::string &text)
    {
        if (g_activeEngine) { log_info(QString::fromStdString(text)); }
    });

    m.def("log_warning", [](const std::string &text)
    {
        if (g_activeEngine) { log_warning(QString::fromStdString(text)); }
    });

    m.def("log_error", [](const std::string &text)
    {
        if (g_activeEngine) { log_error(QString::fromStdString(text)); }
    });

    auto buildProtocolDict = [](const ProtocolMessage &msg) -> py::dict
    {
        py::dict d;
        d["protocol"] = msg.protocol.toStdString();
        d["name"] = msg.name.toStdString();
        d["description"] = msg.description.toStdString();
        d["id"] = msg.id;
        d["payload"] = py::bytes(msg.payload.constData(), msg.payload.size());

        py::dict metadata;
        for (auto it = msg.metadata.begin(); it != msg.metadata.end(); ++it)
        {
            metadata[py::cast(it.key().toStdString())] = it.value().toString().toStdString();
        }
        d["metadata"] = metadata;
        return d;
    };

    auto parseSdoAbortCode = [](const BusMessage &msg) -> uint32_t
    {
        if (msg.getLength() < 8) { return 0; }
        return static_cast<uint32_t>(msg.getByte(4))
             | (static_cast<uint32_t>(msg.getByte(5)) << 8)
             | (static_cast<uint32_t>(msg.getByte(6)) << 16)
             | (static_cast<uint32_t>(msg.getByte(7)) << 24);
    };

    auto waitForMessage = [](double timeoutSec,
                             const std::function<bool(const BusMessage &)> &isMatch,
                             BusMessage &response) -> bool
    {
        if (!g_activeEngine) { return false; }

        if (timeoutSec < 0.0) { timeoutSec = 0.0; }
        QElapsedTimer timer;
        timer.start();
        const qint64 timeoutMs = static_cast<qint64>(timeoutSec * 1000.0);

        while (!g_activeEngine->stopRequested())
        {
            if (timeoutMs >= 0 && timer.elapsed() > timeoutMs) {
                return false;
            }

            bool found = false;
            {
                py::gil_scoped_release release;
                QMutexLocker lock(&g_activeEngine->msgQueueMutex());
                QQueue<BusMessage> &queue = g_activeEngine->msgQueue();

                if (queue.isEmpty())
                {
                    qint64 remaining = timeoutMs >= 0 ? qMax<qint64>(0, timeoutMs - timer.elapsed()) : 50;
                    unsigned long waitMs = static_cast<unsigned long>(qMin<qint64>(50, remaining));
                    if (waitMs > 0) {
                        g_activeEngine->msgQueueCondition().wait(&g_activeEngine->msgQueueMutex(), waitMs);
                    }
                }

                QQueue<BusMessage> keep;
                while (!queue.isEmpty())
                {
                    BusMessage msg = queue.dequeue();
                    if (!found && isMatch(msg))
                    {
                        response = msg;
                        found = true;
                    }
                    else
                    {
                        keep.enqueue(msg);
                    }
                }

                while (!keep.isEmpty())
                {
                    queue.enqueue(keep.dequeue());
                }
            }

            if (found) { return true; }
        }

        return false;
    };

    auto waitForSdoResponse = [waitForMessage](uint16_t responseCobId, uint16_t index, uint8_t subIndex,
                                               double timeoutSec, BusMessage &response) -> bool
    {
        return waitForMessage(timeoutSec, [responseCobId, index, subIndex](const BusMessage &msg) -> bool
        {
            if (msg.busType() != BusType::CAN || msg.isExtended()) { return false; }
            if (msg.getRawId() != responseCobId || msg.getLength() < 4) { return false; }
            const uint16_t msgIndex = static_cast<uint16_t>(msg.getByte(1))
                                    | (static_cast<uint16_t>(msg.getByte(2)) << 8);
            return (msgIndex == index) && (msg.getByte(3) == subIndex);
        }, response);
    };

    auto waitForSdoFrame = [waitForMessage](uint16_t responseCobId, double timeoutSec, BusMessage &response) -> bool
    {
        return waitForMessage(timeoutSec, [responseCobId](const BusMessage &msg) -> bool
        {
            return msg.busType() == BusType::CAN
                && !msg.isExtended()
                && msg.getRawId() == responseCobId
                && msg.getLength() >= 1;
        }, response);
    };

    // --- CANopen database access ---

    m.def("canopen_databases", []() -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        MeasurementSetup &setup = g_activeEngine->backend().getSetup();
        for (MeasurementNetwork *net : setup.getNetworks())
        {
            for (const pCanOpenDb &db : std::as_const(net->_canOpenDbs))
            {
                py::dict dbInfo;
                dbInfo["file"] = db->fileName().toStdString();
                dbInfo["path"] = db->path().toStdString();
                dbInfo["network"] = net->name().toStdString();
                dbInfo["device"] = db->deviceName().toStdString();
                if (db->configuredNodeId() >= 0) {
                    dbInfo["node_id"] = db->configuredNodeId();
                }

                py::list objects;
                for (auto it = db->objects().cbegin(); it != db->objects().cend(); ++it)
                {
                    const CanOpenObjectEntry &obj = it.value();
                    py::dict o;
                    o["index"] = obj.index;
                    o["sub_index"] = obj.subIndex;
                    o["name"] = obj.name.toStdString();
                    o["data_type"] = obj.dataTypeName.toStdString();
                    o["default"] = obj.defaultValue.toStdString();
                    o["value"] = obj.parameterValue.toStdString();
                    objects.append(o);
                }
                dbInfo["objects"] = objects;
                result.append(dbInfo);
            }
        }
        return result;
    });

    m.def("decode_canopen", [buildProtocolDict](const BusMessage &msg) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        CanOpenDecoder decoder(&g_activeEngine->backend());
        ProtocolMessage out;
        if (decoder.tryDecode(msg, out) != DecodeStatus::Completed) {
            return py::none();
        }
        return buildProtocolDict(out);
    }, py::arg("msg"));

    m.def("sdo_read", [waitForSdoResponse, waitForSdoFrame, parseSdoAbortCode](uint8_t node_id,
                                                               uint16_t index,
                                                               uint8_t sub_index,
                                                               uint16_t interface_id,
                                                               double timeout) -> py::dict
    {
        if (!g_activeEngine) {
            throw std::runtime_error("no active engine");
        }
        if (node_id == 0 || node_id > 127) {
            throw py::value_error("node_id must be in range 1..127");
        }

        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (!intf) {
            throw py::value_error("invalid interface_id");
        }

        BusMessage req;
        req.setRawId(static_cast<uint32_t>(0x600u + node_id));
        req.setLength(8);
        req.setByte(0, 0x40); // Initiate upload request
        req.setByte(1, static_cast<uint8_t>(index & 0xFF));
        req.setByte(2, static_cast<uint8_t>((index >> 8) & 0xFF));
        req.setByte(3, sub_index);
        req.setByte(4, 0);
        req.setByte(5, 0);
        req.setByte(6, 0);
        req.setByte(7, 0);
        intf->sendMessage(req);

        BusMessage resp;
        const uint16_t responseCobId = static_cast<uint16_t>(0x580u + node_id);
        QElapsedTimer totalTimer;
        totalTimer.start();
        const auto remainingTimeout = [&totalTimer, timeout]() -> double
        {
            const double remaining = timeout - (static_cast<double>(totalTimer.elapsed()) / 1000.0);
            return remaining > 0.0 ? remaining : 0.0;
        };

        if (!waitForSdoResponse(responseCobId, index, sub_index, remainingTimeout(), resp)) {
            throw std::runtime_error("timeout waiting for SDO read response");
        }

        const uint8_t cs = resp.getByte(0);
        if (cs == 0x80)
        {
            const uint32_t abortCode = parseSdoAbortCode(resp);
            throw std::runtime_error(QString("SDO read aborted (0x%1)")
                                     .arg(abortCode, 8, 16, QChar('0'))
                                     .toStdString());
        }

        if ((cs & 0xE0) != 0x40) {
            throw std::runtime_error("unexpected SDO read response");
        }

        const bool expedited = (cs & 0x02) != 0;
        const bool sizeIndicated = (cs & 0x01) != 0;
        std::string payload;

        if (expedited)
        {
            int dataLen = 4;
            if (sizeIndicated) {
                dataLen = 4 - static_cast<int>((cs >> 2) & 0x03);
            }
            dataLen = qBound(0, dataLen, 4);
            payload.resize(static_cast<size_t>(dataLen), '\0');
            for (int i = 0; i < dataLen; ++i)
            {
                payload[static_cast<size_t>(i)] = static_cast<char>(resp.getByte(4 + i));
            }
        }
        else
        {
            std::optional<uint32_t> expectedSize;
            if (sizeIndicated && resp.getLength() >= 8)
            {
                expectedSize = static_cast<uint32_t>(resp.getByte(4))
                             | (static_cast<uint32_t>(resp.getByte(5)) << 8)
                             | (static_cast<uint32_t>(resp.getByte(6)) << 16)
                             | (static_cast<uint32_t>(resp.getByte(7)) << 24);
                payload.reserve(static_cast<size_t>(*expectedSize));
            }

            bool toggle = false;
            bool done = false;
            int lastPercent = -1;
            if (expectedSize.has_value() && *expectedSize > 0)
            {
                emitScriptOutputLine(g_activeEngine,
                                     QStringLiteral("CANGAROO_SDO_PROGRESS:{\"cmd\":\"domain\",\"index\":%1,\"sub_index\":%2,\"received\":0,\"total\":%3,\"percent\":0}\n")
                                         .arg(index)
                                         .arg(sub_index)
                                         .arg(*expectedSize));
                lastPercent = 0;
            }
            while (!done)
            {
                BusMessage segReq;
                segReq.setRawId(static_cast<uint32_t>(0x600u + node_id));
                segReq.setLength(8);
                segReq.setByte(0, static_cast<uint8_t>(0x60u | (toggle ? 0x10u : 0x00u)));
                segReq.setByte(1, 0);
                segReq.setByte(2, 0);
                segReq.setByte(3, 0);
                segReq.setByte(4, 0);
                segReq.setByte(5, 0);
                segReq.setByte(6, 0);
                segReq.setByte(7, 0);
                intf->sendMessage(segReq);

                BusMessage segResp;
                if (!waitForSdoFrame(responseCobId, remainingTimeout(), segResp)) {
                    throw std::runtime_error("timeout waiting for SDO upload segment");
                }

                const uint8_t segCs = segResp.getByte(0);
                if (segCs == 0x80)
                {
                    const uint32_t abortCode = parseSdoAbortCode(segResp);
                    throw std::runtime_error(QString("SDO read aborted (0x%1)")
                                             .arg(abortCode, 8, 16, QChar('0'))
                                             .toStdString());
                }

                if ((segCs & 0xE0) != 0x00) {
                    throw std::runtime_error("unexpected SDO upload segment response");
                }

                const bool responseToggle = (segCs & 0x10) != 0;
                if (responseToggle != toggle) {
                    throw std::runtime_error("SDO upload toggle mismatch");
                }

                const int unusedBytes = static_cast<int>((segCs >> 1) & 0x07);
                if (unusedBytes > 7) {
                    throw std::runtime_error("invalid SDO upload segment size");
                }
                const int payloadBytes = 7 - unusedBytes;
                const int availableBytes = qMax(0, static_cast<int>(segResp.getLength()) - 1);
                const int copyBytes = qMin(payloadBytes, availableBytes);
                for (int i = 0; i < copyBytes; ++i)
                {
                    payload.push_back(static_cast<char>(segResp.getByte(1 + i)));
                }

                if (expectedSize.has_value() && *expectedSize > 0)
                {
                    const int percent = qBound(0,
                                               static_cast<int>((payload.size() * 100ULL) / static_cast<size_t>(*expectedSize)),
                                               100);
                    if (percent != lastPercent)
                    {
                        emitScriptOutputLine(g_activeEngine,
                                             QStringLiteral("CANGAROO_SDO_PROGRESS:{\"cmd\":\"domain\",\"index\":%1,\"sub_index\":%2,\"received\":%3,\"total\":%4,\"percent\":%5}\n")
                                                 .arg(index)
                                                 .arg(sub_index)
                                                 .arg(static_cast<qulonglong>(payload.size()))
                                                 .arg(*expectedSize)
                                                 .arg(percent));
                        lastPercent = percent;
                    }
                }

                done = (segCs & 0x01) != 0;
                toggle = !toggle;
            }

            if (expectedSize.has_value() && payload.size() > static_cast<size_t>(*expectedSize)) {
                payload.resize(static_cast<size_t>(*expectedSize));
            }
        }

        uint32_t raw = 0;
        const int rawBytes = qMin(4, static_cast<int>(payload.size()));
        for (int i = 0; i < rawBytes; ++i)
        {
            raw |= static_cast<uint32_t>(static_cast<uint8_t>(payload[static_cast<size_t>(i)])) << (8 * i);
        }

        py::dict result;
        result["node_id"] = node_id;
        result["index"] = index;
        result["sub_index"] = sub_index;
        result["raw"] = raw;
        result["size"] = static_cast<int>(payload.size());
        result["data"] = py::bytes(payload);
        return result;
    },
    py::arg("node_id"),
    py::arg("index"),
    py::arg("sub_index") = 0,
    py::arg("interface_id") = 0,
    py::arg("timeout") = 1.0);

    m.def("sdo_write", [waitForSdoResponse, waitForSdoFrame, parseSdoAbortCode](uint8_t node_id,
                                                                uint16_t index,
                                                                uint8_t sub_index,
                                                                py::object value,
                                                                py::object size,
                                                                uint16_t interface_id,
                                                                double timeout,
                                                                bool force_segment) -> py::dict
    {
        if (!g_activeEngine) {
            throw std::runtime_error("no active engine");
        }
        if (node_id == 0 || node_id > 127) {
            throw py::value_error("node_id must be in range 1..127");
        }

        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (!intf) {
            throw py::value_error("invalid interface_id");
        }

        QByteArray payload;
        int payloadSize = 0;
        uint32_t rawValue = 0;
        const bool bytesValue = py::isinstance<py::bytes>(value) || py::isinstance<py::bytearray>(value);

        if (bytesValue)
        {
            std::string bytes = py::bytes(value);
            payload = QByteArray(bytes.data(), static_cast<int>(bytes.size()));
            payloadSize = payload.size();
            if (!size.is_none()) {
                const int requestedSize = size.cast<int>();
                if (requestedSize != payloadSize) {
                    throw py::value_error("size must match bytes value length");
                }
            }
            for (int i = 0; i < qMin(4, payloadSize); ++i) {
                rawValue |= static_cast<uint32_t>(static_cast<uint8_t>(payload.at(i))) << (8 * i);
            }
        }
        else
        {
            const uint64_t raw64 = value.cast<uint64_t>();
            if (raw64 > 0xFFFFFFFFULL) {
                throw py::value_error("integer value must fit into 32 bits");
            }
            rawValue = static_cast<uint32_t>(raw64);

            if (size.is_none()) {
                payloadSize = (rawValue <= 0xFFu) ? 1
                            : (rawValue <= 0xFFFFu) ? 2
                            : (rawValue <= 0xFFFFFFu) ? 3
                            : 4;
            } else {
                payloadSize = size.cast<int>();
            }

            if (payloadSize < 1 || payloadSize > 4) {
                throw py::value_error("size must be in range 1..4");
            }

            if (payloadSize < 4)
            {
                const uint32_t maxValue = (1u << (payloadSize * 8)) - 1u;
                if (rawValue > maxValue) {
                    throw py::value_error("integer value does not fit requested size");
                }
            }

            payload.resize(payloadSize);
            for (int i = 0; i < payloadSize; ++i) {
                payload[i] = static_cast<char>((rawValue >> (8 * i)) & 0xFFu);
            }
        }

        const uint16_t responseCobId = static_cast<uint16_t>(0x580u + node_id);
        auto throwWriteAbort = [&](const BusMessage &response, const QString &prefix) {
            const uint32_t abortCode = parseSdoAbortCode(response);
            throw std::runtime_error(QString("%1 (0x%2)")
                                     .arg(prefix)
                                     .arg(abortCode, 8, 16, QChar('0'))
                                     .toStdString());
        };

        if (bytesValue && (force_segment || payloadSize == 0 || payloadSize > 4))
        {
            static constexpr int MAX_RETRIES = 3;

            BusMessage req;
            req.setRawId(static_cast<uint32_t>(0x600u + node_id));
            req.setLength(8);
            req.setByte(0, 0x21);
            req.setByte(1, static_cast<uint8_t>(index & 0xFF));
            req.setByte(2, static_cast<uint8_t>((index >> 8) & 0xFF));
            req.setByte(3, sub_index);
            req.setByte(4, static_cast<uint8_t>(payloadSize & 0xFF));
            req.setByte(5, static_cast<uint8_t>((payloadSize >> 8) & 0xFF));
            req.setByte(6, static_cast<uint8_t>((payloadSize >> 16) & 0xFF));
            req.setByte(7, static_cast<uint8_t>((payloadSize >> 24) & 0xFF));

            BusMessage resp;
            bool initiateOk = false;
            for (int attempt = 0; attempt < MAX_RETRIES && !initiateOk; ++attempt) {
                intf->sendMessage(req);
                if (!waitForSdoResponse(responseCobId, index, sub_index, timeout, resp)) {
                    if (attempt + 1 >= MAX_RETRIES) {
                        throw std::runtime_error("timeout waiting for SDO download initiate response");
                    }
                    continue;
                }
                initiateOk = true;
            }

            const uint8_t cs = resp.getByte(0);
            if (cs == 0x80) {
                throwWriteAbort(resp, QStringLiteral("SDO download initiate aborted"));
            }
            if ((cs >> 5) != 3) { // CiA 301: scs=3 (initiate download response)
                throw std::runtime_error("unexpected SDO download initiate response");
            }

            bool toggle = false;
            int offset = 0;
            const int segmentCount = qMax(1, (payloadSize + 6) / 7);
            for (int segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex)
            {
                const int chunkSize = qMin(7, payloadSize - offset);
                const bool last = (segmentIndex + 1) == segmentCount;
                const int unusedBytes = last ? 7 - chunkSize : 0;

                BusMessage segment;
                segment.setRawId(static_cast<uint32_t>(0x600u + node_id));
                segment.setLength(8);
                segment.setByte(0, static_cast<uint8_t>((toggle ? 0x10 : 0x00)
                                                        | (unusedBytes << 1)
                                                        | (last ? 0x01 : 0x00)));
                for (int i = 0; i < 7; ++i) {
                    segment.setByte(1 + i, i < chunkSize ? static_cast<uint8_t>(payload.at(offset + i)) : 0);
                }

                BusMessage segmentResp;
                bool segmentOk = false;
                for (int attempt = 0; attempt < MAX_RETRIES && !segmentOk; ++attempt) {
                    intf->sendMessage(segment);
                    if (!waitForSdoFrame(responseCobId, timeout, segmentResp)) {
                        if (attempt + 1 >= MAX_RETRIES) {
                            throw std::runtime_error("timeout waiting for SDO download segment response");
                        }
                        continue;
                    }
                    segmentOk = true;
                }

                const uint8_t segmentCs = segmentResp.getByte(0);
                if (segmentCs == 0x80) {
                    throwWriteAbort(segmentResp, QStringLiteral("SDO download segment aborted"));
                }
                if ((segmentCs & 0xE0) != 0x20) {
                    throw std::runtime_error("unexpected SDO download segment response");
                }
                const bool receivedToggle = (segmentCs & 0x10) != 0;
                if (receivedToggle != toggle) {
                    throw std::runtime_error("SDO download toggle mismatch");
                }

                offset += chunkSize;
                toggle = !toggle;
            }
        }
        else
        {
            BusMessage req;
            req.setRawId(static_cast<uint32_t>(0x600u + node_id));
            req.setLength(8);
            req.setByte(0, static_cast<uint8_t>(0x23 | ((4 - payloadSize) << 2))); // Expedited download
            req.setByte(1, static_cast<uint8_t>(index & 0xFF));
            req.setByte(2, static_cast<uint8_t>((index >> 8) & 0xFF));
            req.setByte(3, sub_index);
            req.setByte(4, payloadSize > 0 ? static_cast<uint8_t>(payload.at(0)) : 0);
            req.setByte(5, payloadSize > 1 ? static_cast<uint8_t>(payload.at(1)) : 0);
            req.setByte(6, payloadSize > 2 ? static_cast<uint8_t>(payload.at(2)) : 0);
            req.setByte(7, payloadSize > 3 ? static_cast<uint8_t>(payload.at(3)) : 0);
            intf->sendMessage(req);

            BusMessage resp;
            if (!waitForSdoResponse(responseCobId, index, sub_index, timeout, resp)) {
                throw std::runtime_error("timeout waiting for SDO write response");
            }

            const uint8_t cs = resp.getByte(0);
            if (cs == 0x80) {
                throwWriteAbort(resp, QStringLiteral("SDO write aborted"));
            }
            if ((cs >> 5) != 3) { // CiA 301: scs=3 (download response)
                throw std::runtime_error("unexpected SDO write response");
            }
        }

        py::dict result;
        result["node_id"] = node_id;
        result["index"] = index;
        result["sub_index"] = sub_index;
        result["raw"] = rawValue;
        result["size"] = payloadSize;
        result["ok"] = true;
        return result;
    },
    py::arg("node_id"),
    py::arg("index"),
    py::arg("sub_index"),
    py::arg("value"),
    py::arg("size") = py::none(),
    py::arg("interface_id") = 0,
    py::arg("timeout") = 1.0,
    py::arg("force_segment") = false);

    m.def("sdo_write_domain", [waitForSdoResponse, waitForSdoFrame, parseSdoAbortCode](uint8_t node_id,
                                                                                        uint16_t index,
                                                                                        uint8_t sub_index,
                                                                                        py::bytes data,
                                                                                        uint16_t interface_id,
                                                                                        double timeout) -> py::dict
    {
        if (!g_activeEngine) {
            throw std::runtime_error("no active engine");
        }
        if (node_id == 0 || node_id > 127) {
            throw py::value_error("node_id must be in range 1..127");
        }

        BusInterface *intf = g_activeEngine->backend().getInterfaceById(interface_id);
        if (!intf) {
            throw py::value_error("invalid interface_id");
        }

        const std::string payload = data;
        if (payload.empty()) {
            throw py::value_error("data must not be empty");
        }

        const uint32_t totalSize = static_cast<uint32_t>(payload.size());
        const uint16_t responseCobId = static_cast<uint16_t>(0x580u + node_id);

        BusMessage initReq;
        initReq.setRawId(static_cast<uint32_t>(0x600u + node_id));
        initReq.setLength(8);
        initReq.setByte(0, 0x21); // Initiate domain download, size indicated
        initReq.setByte(1, static_cast<uint8_t>(index & 0xFF));
        initReq.setByte(2, static_cast<uint8_t>((index >> 8) & 0xFF));
        initReq.setByte(3, sub_index);
        initReq.setByte(4, static_cast<uint8_t>(totalSize & 0xFFu));
        initReq.setByte(5, static_cast<uint8_t>((totalSize >> 8) & 0xFFu));
        initReq.setByte(6, static_cast<uint8_t>((totalSize >> 16) & 0xFFu));
        initReq.setByte(7, static_cast<uint8_t>((totalSize >> 24) & 0xFFu));
        intf->sendMessage(initReq);

        BusMessage resp;
        if (!waitForSdoResponse(responseCobId, index, sub_index, timeout, resp)) {
            throw std::runtime_error("timeout waiting for SDO download initiate response");
        }

        const uint8_t initCs = resp.getByte(0);
        if (initCs == 0x80)
        {
            const uint32_t abortCode = parseSdoAbortCode(resp);
            throw std::runtime_error(QString("SDO download aborted (0x%1)")
                                     .arg(abortCode, 8, 16, QChar('0'))
                                     .toStdString());
        }
        if (initCs != 0x60) {
            throw std::runtime_error("unexpected SDO download initiate response");
        }

        emitScriptOutputLine(g_activeEngine,
                             QStringLiteral("CANGAROO_SDO_PROGRESS:{\"cmd\":\"domain_write\",\"index\":%1,\"sub_index\":%2,\"sent\":0,\"total\":%3,\"percent\":0}\n")
                                 .arg(index)
                                 .arg(sub_index)
                                 .arg(totalSize));

        bool toggle = false;
        size_t offset = 0;
        int lastPercent = 0;
        while (offset < payload.size())
        {
            const size_t remaining = payload.size() - offset;
            const int chunkBytes = static_cast<int>(qMin<size_t>(remaining, 7));
            const bool lastSegment = (offset + static_cast<size_t>(chunkBytes)) >= payload.size();
            const int unusedBytes = 7 - chunkBytes;

            BusMessage segReq;
            segReq.setRawId(static_cast<uint32_t>(0x600u + node_id));
            segReq.setLength(8);
            segReq.setByte(0, static_cast<uint8_t>((toggle ? 0x10u : 0x00u)
                                                    | static_cast<uint8_t>(unusedBytes << 1)
                                                    | (lastSegment ? 0x01u : 0x00u)));
            for (int i = 0; i < 7; ++i)
            {
                const uint8_t b = (i < chunkBytes)
                    ? static_cast<uint8_t>(payload[offset + static_cast<size_t>(i)])
                    : 0;
                segReq.setByte(1 + i, b);
            }
            intf->sendMessage(segReq);

            BusMessage segResp;
            if (!waitForSdoFrame(responseCobId, timeout, segResp)) {
                throw std::runtime_error("timeout waiting for SDO download segment ack");
            }

            const uint8_t segCs = segResp.getByte(0);
            if (segCs == 0x80)
            {
                const uint32_t abortCode = parseSdoAbortCode(segResp);
                throw std::runtime_error(QString("SDO download aborted (0x%1)")
                                         .arg(abortCode, 8, 16, QChar('0'))
                                         .toStdString());
            }
            if ((segCs & 0xE0) != 0x20) {
                throw std::runtime_error("unexpected SDO download segment response");
            }

            const bool responseToggle = (segCs & 0x10) != 0;
            if (responseToggle != toggle) {
                throw std::runtime_error("SDO download toggle mismatch");
            }

            offset += static_cast<size_t>(chunkBytes);
            toggle = !toggle;

            const int percent = qBound(0, static_cast<int>((offset * 100ULL) / payload.size()), 100);
            if (percent != lastPercent || lastSegment)
            {
                emitScriptOutputLine(g_activeEngine,
                                     QStringLiteral("CANGAROO_SDO_PROGRESS:{\"cmd\":\"domain_write\",\"index\":%1,\"sub_index\":%2,\"sent\":%3,\"total\":%4,\"percent\":%5}\n")
                                         .arg(index)
                                         .arg(sub_index)
                                         .arg(static_cast<qulonglong>(offset))
                                         .arg(totalSize)
                                         .arg(percent));
                lastPercent = percent;
            }
        }

        py::dict result;
        result["node_id"] = node_id;
        result["index"] = index;
        result["sub_index"] = sub_index;
        result["size"] = static_cast<int>(payload.size());
        result["ok"] = true;
        return result;
    },
    py::arg("node_id"),
    py::arg("index"),
    py::arg("sub_index"),
    py::arg("data"),
    py::arg("interface_id") = 0,
    py::arg("timeout") = 30.0);

    // --- DBC / database access ---

    m.def("databases", []() -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        MeasurementSetup &setup = g_activeEngine->backend().getSetup();
        for (MeasurementNetwork *net : setup.getNetworks())
        {
            for (const pCanDb &db : std::as_const(net->_canDbs))
            {
                py::dict dbInfo;
                dbInfo["file"]    = db->getFileName().toStdString();
                dbInfo["path"]    = db->getPath().toStdString();
                dbInfo["network"] = net->name().toStdString();

                py::list msgs;
                for (CanDbMessage *dbMsg : db->getMessageList())
                {
                    py::dict mInfo;
                    mInfo["name"] = dbMsg->getName().toStdString();
                    mInfo["id"]   = dbMsg->getRaw_id();
                    mInfo["dlc"]  = dbMsg->getDlc();

                    py::list sigNames;
                    for (CanDbSignal *sig : dbMsg->getSignals())
                    {
                        sigNames.append(sig->name().toStdString());
                    }
                    mInfo["signals"] = sigNames;
                    msgs.append(mInfo);
                }
                dbInfo["messages"] = msgs;
                result.append(dbInfo);
            }
        }
        return result;
    });

    // Decode signals from a received message using loaded DBCs.
    // Returns { "message": str, "id": int, "signals": { name: { "value", "raw", "unit", "min", "max" } } }
    // or None if no DBC definition is found.
    m.def("decode", [](const BusMessage &msg) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        CanDbMessage *dbMsg = g_activeEngine->backend().findDbMessage(msg);
        if (!dbMsg) { return py::none(); }

        py::dict sigDict;
        for (CanDbSignal *sig : dbMsg->getSignals())
        {
            if (!sig->isPresentInMessage(msg)) { continue; }

            const uint64_t raw  = sig->extractRawDataFromMessage(msg);
            const double   phys = sig->convertRawValueToPhysical(raw);

            py::dict sigInfo;
            sigInfo["value"] = phys;
            sigInfo["raw"]   = raw;
            sigInfo["unit"]  = sig->getUnit().toStdString();
            sigInfo["min"]   = sig->getMinimumValue();
            sigInfo["max"]   = sig->getMaximumValue();

            const QString valueName = sig->getValueName(raw);
            if (!valueName.isEmpty())
            {
                sigInfo["value_name"] = valueName.toStdString();
            }

            sigDict[py::cast(sig->name().toStdString())] = sigInfo;
        }

        py::dict result;
        result["message"] = dbMsg->getName().toStdString();
        result["id"]      = dbMsg->getRaw_id();
        result["signals"] = sigDict;

        CanDbNode *sender = dbMsg->getSender();
        if (sender) { result["sender"] = sender->name().toStdString(); }

        return result;
    }, py::arg("msg"));

    // Look up the DBC signal layout for a message (by live BusMessage).
    m.def("lookup", [](const BusMessage &msg) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }
        CanDbMessage *dbMsg = g_activeEngine->backend().findDbMessage(msg);
        if (!dbMsg) { return py::none(); }
        return buildMessageDict(dbMsg);
    }, py::arg("msg"));

    // Find a DBC message definition by name (str) or raw ID (int).
    // Returns the same dict as lookup(), or None.
    m.def("find_message", [](py::object name_or_id) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        CanDbMessage *dbMsg = nullptr;

        if (py::isinstance<py::str>(name_or_id))
        {
            const QString qname = QString::fromStdString(name_or_id.cast<std::string>());
            dbMsg = findDbMessageByName(g_activeEngine->backend(), qname);
        }
        else
        {
            // Numeric ID — build a dummy BusMessage with that raw_id
            BusMessage dummy;
            dummy.setRawId(name_or_id.cast<uint32_t>());
            dbMsg = g_activeEngine->backend().findDbMessage(dummy);
        }

        if (!dbMsg) { return py::none(); }
        return buildMessageDict(dbMsg);
    }, py::arg("name_or_id"));

    // Convenience: extract a single named signal's physical value from a message.
    // Returns float, or None if the message or signal is not found in the DBC.
    m.def("signal_value", [](const BusMessage &msg, const std::string &signal_name) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        CanDbMessage *dbMsg = g_activeEngine->backend().findDbMessage(msg);
        if (!dbMsg) { return py::none(); }

        CanDbSignal *sig = dbMsg->getSignalByName(QString::fromStdString(signal_name));
        if (!sig) { return py::none(); }

        return py::cast(sig->extractPhysicalFromMessage(msg));
    }, py::arg("msg"), py::arg("signal_name"));

    // Build a BusMessage by encoding signal physical values according to the DBC.
    // name_or_id: str (message name) or int (raw CAN ID)
    // signals:    dict mapping signal name -> physical value
    // Returns the populated Message, or raises ValueError on error.
    m.def("encode", [](py::object name_or_id, py::dict values) -> BusMessage
    {
        if (!g_activeEngine)
        {
            throw std::runtime_error("no active engine");
        }

        CanDbMessage *dbMsg = nullptr;
        if (py::isinstance<py::str>(name_or_id))
        {
            const QString qname = QString::fromStdString(name_or_id.cast<std::string>());
            dbMsg = findDbMessageByName(g_activeEngine->backend(), qname);
        }
        else
        {
            BusMessage dummy;
            dummy.setRawId(name_or_id.cast<uint32_t>());
            dbMsg = g_activeEngine->backend().findDbMessage(dummy);
        }

        if (!dbMsg)
        {
            throw py::value_error("message not found in loaded DBC databases");
        }

        // Create a zero-initialised message with the DBC id and DLC
        BusMessage msg;
        msg.setRawId(dbMsg->getRaw_id());
        msg.setLength(dbMsg->getDlc());

        for (auto item : values)
        {
            const QString sigName = QString::fromStdString(item.first.cast<std::string>());
            CanDbSignal *sig = dbMsg->getSignalByName(sigName);
            if (!sig)
            {
                throw py::value_error(
                    std::string("signal not found in DBC: ") + sigName.toStdString());
            }

            const double phys   = item.second.cast<double>();
            const double factor = sig->getFactor();
            const double offset = sig->getOffset();

            // Convert physical → raw
            if (factor == 0.0)
            {
                throw py::value_error(
                    std::string("signal '") + sigName.toStdString()
                    + "' has factor=0 in DBC — cannot encode");
            }

            uint64_t raw = 0;
            {
                const double rawD = (phys - offset) / factor;
                if (sig->isUnsigned())
                {
                    raw = static_cast<uint64_t>(rawD < 0.0 ? 0.0 : rawD + 0.5);
                    const uint64_t maxRaw = (sig->length() < 64)
                                           ? ((1ULL << sig->length()) - 1)
                                           : ~0ULL;
                    if (raw > maxRaw) { raw = maxRaw; }
                }
                else
                {
                    const uint8_t sigLen = sig->length();
                    const int64_t minRaw = (sigLen > 0 && sigLen < 64) ? -(1LL << (sigLen - 1)) : INT64_MIN;
                    const int64_t maxRaw = (sigLen > 0 && sigLen < 64) ? ((1LL << (sigLen - 1)) - 1) : INT64_MAX;
                    const int64_t s = std::clamp(
                        static_cast<int64_t>(rawD < 0.0 ? rawD - 0.5 : rawD + 0.5),
                        minRaw, maxRaw);
                    raw = static_cast<uint64_t>(s);
                }
            }

            insertRawSignalIntoMsg(msg, sig->startBit(), sig->length(), sig->isBigEndian(), raw);
        }

        return msg;
    }, py::arg("name_or_id"), py::arg("values"));

    // --- LIN database access ---

    // Build a signal-info dict for a LinSignal
    auto buildLinSignalDict = [](LinSignal *sig) -> py::dict
    {
        py::dict s;
        s["name"]       = sig->name().toStdString();
        s["bit_offset"] = sig->bitOffset();
        s["bit_length"] = sig->bitLength();
        s["factor"]     = sig->factor();
        s["offset"]     = sig->offset();
        s["min"]        = sig->minValue();
        s["max"]        = sig->maxValue();
        s["unit"]       = sig->unit().toStdString();
        s["publisher"]  = sig->publisher().toStdString();
        s["init_value"] = sig->initValue();
        return s;
    };

    // Build a frame-info dict for a LinFrame
    auto buildLinFrameDict = [&buildLinSignalDict](LinFrame *frame) -> py::dict
    {
        py::dict d;
        d["id"]        = frame->id();
        d["name"]      = frame->name().toStdString();
        d["publisher"] = frame->publisher().toStdString();
        d["length"]    = frame->length();

        py::list sigs;
        for (LinSignal *sig : frame->signalList())
        {
            sigs.append(buildLinSignalDict(sig));
        }
        d["signals"] = sigs;
        return d;
    };

    // List all loaded LDF databases with their frames.
    m.def("lin_databases", [buildLinFrameDict]() -> py::list
    {
        py::list result;
        if (!g_activeEngine) { return result; }

        MeasurementSetup &setup = g_activeEngine->backend().getSetup();
        for (MeasurementNetwork *net : setup.getNetworks())
        {
            for (const pLinDb &db : std::as_const(net->_linDbs))
            {
                py::dict dbInfo;
                dbInfo["file"]    = db->fileName().toStdString();
                dbInfo["path"]    = db->path().toStdString();
                dbInfo["network"] = net->name().toStdString();
                dbInfo["speed"]   = db->speedBps();
                dbInfo["master"]  = db->masterNode().toStdString();

                py::list frames;
                for (LinFrame *frame : db->frames())
                {
                    frames.append(buildLinFrameDict(frame));
                }
                dbInfo["frames"] = frames;
                result.append(dbInfo);
            }
        }
        return result;
    });

    // Look up a LIN frame definition by name (str) or ID (int).
    // Returns a frame-info dict, or None if not found.
    m.def("find_lin_frame", [buildLinFrameDict](py::object name_or_id) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        MeasurementSetup &setup = g_activeEngine->backend().getSetup();
        for (MeasurementNetwork *net : setup.getNetworks())
        {
            for (const pLinDb &db : std::as_const(net->_linDbs))
            {
                LinFrame *frame = nullptr;
                if (py::isinstance<py::str>(name_or_id))
                {
                    frame = db->frameByName(QString::fromStdString(name_or_id.cast<std::string>()));
                }
                else
                {
                    frame = db->frameById(static_cast<uint8_t>(name_or_id.cast<int>()));
                }
                if (frame) { return buildLinFrameDict(frame); }
            }
        }
        return py::none();
    }, py::arg("name_or_id"));

    // Decode a received LIN BusMessage using loaded LDF databases.
    // Returns { "frame": str, "id": int, "signals": { name: { "value", "raw", "unit" } } }
    // or None if no LDF definition is found.
    m.def("decode_lin", [buildLinFrameDict](const BusMessage &msg) -> py::object
    {
        if (!g_activeEngine) { return py::none(); }

        LinFrame *frame = g_activeEngine->backend().findLinFrame(msg);
        if (!frame) { return py::none(); }

        const uint8_t *data = msg.getData();
        const uint8_t  len  = msg.getLength();
        std::span<const uint8_t> payload{data, len};

        py::dict sigDict;
        for (LinSignal *sig : frame->signalList())
        {
            const uint64_t raw  = sig->extractRawValue(payload);
            const double   phys = sig->convertToPhysical(raw);

            py::dict sigInfo;
            sigInfo["value"]  = phys;
            sigInfo["raw"]    = raw;
            sigInfo["unit"]   = sig->unit().toStdString();

            const QString valueName = sig->getValueName(raw);
            if (!valueName.isEmpty())
            {
                sigInfo["value_name"] = valueName.toStdString();
            }

            sigDict[py::cast(sig->name().toStdString())] = sigInfo;
        }

        py::dict result;
        result["frame"]   = frame->name().toStdString();
        result["id"]      = frame->id();
        result["signals"] = sigDict;
        return result;
    }, py::arg("msg"));

    m.def("e2e_p2_compute_crc",
        [](const BusMessage &msg, uint16_t dataID) -> uint8_t
        {
            return e2e_p2_compute_crc(msg, dataID);
        },
        py::arg("msg"), py::arg("data_id"),
        "Compute AUTOSAR E2E Profile 2 CRC-8H2F over msg. "
        "Byte 0 is treated as 0x00; byte 1 must already hold the counter nibble. "
        "Returns the CRC byte without modifying the message.");

    m.def("e2e_p2_protect",
        [](BusMessage &msg, uint16_t dataID, uint8_t counter)
        {
            msg.setByte(1, counter & 0x0Fu);
            msg.setByte(0, e2e_p2_compute_crc(msg, dataID));
        },
        py::arg("msg"), py::arg("data_id"), py::arg("counter"),
        "Write AUTOSAR E2E Profile 2 header into msg in-place: "
        "sets counter nibble in byte 1, then computes and writes CRC into byte 0.");
}


// ---------------------------------------------------------------------------
// PythonEngine implementation
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN
static QString findPythonHome()
{
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString pyDir = QString("%1/lib/python%2.%3")
                                  .arg(appDir)
                                  .arg(PY_MAJOR_VERSION)
                                  .arg(PY_MINOR_VERSION);
        if (QDir(pyDir).exists())
        {
            return appDir;
        }
    }

    for (const char *name : {"python3", "python"})
    {
        const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (exe.isEmpty()) { continue; }

        QDir dir = QFileInfo(exe).absoluteDir();
        if (dir.dirName().compare("bin", Qt::CaseInsensitive) == 0 ||
            dir.dirName().compare("Scripts", Qt::CaseInsensitive) == 0)
        {
            dir.cdUp();
        }
        return dir.absolutePath();
    }

    return {};
}
#endif // Q_OS_WIN

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
struct PythonEngine::PyInterpreterHolder
{
    bool _env{ prepareEnvironment() };
    py::scoped_interpreter guard{};
    PyThreadState *savedState = nullptr;

    static bool prepareEnvironment()
    {
#ifdef Q_OS_WIN
        if (qEnvironmentVariableIsEmpty("PYTHONHOME"))
        {
            const QString home = findPythonHome();
            if (!home.isEmpty())
            {
                qputenv("PYTHONHOME", home.toLocal8Bit());
            }
        }
#endif
        return true;
    }

    PyInterpreterHolder()
    {
        savedState = PyEval_SaveThread();
    }

    ~PyInterpreterHolder()
    {
        PyEval_RestoreThread(savedState);
    }
};
#pragma GCC diagnostic pop

std::shared_ptr<PythonEngine::PyInterpreterHolder> PythonEngine::sharedInterpreter()
{
    struct SharedInterpreterState
    {
        QMutex mutex;
        std::weak_ptr<PyInterpreterHolder> shared;
    };
    static SharedInterpreterState state;

    QMutexLocker locker(&state.mutex);
    std::shared_ptr<PyInterpreterHolder> interp = state.shared.lock();
    if (!interp)
    {
        interp = std::make_shared<PyInterpreterHolder>();
        state.shared = interp;
    }
    return interp;
}

PythonEngine::PythonEngine(Backend &backend, QObject *parent)
    : QObject(parent)
    , _backend(backend)
{
    try
    {
        _pyInterp = sharedInterpreter();
    }
    catch (const std::exception &e)
    {
        _initError = QString::fromStdString(e.what());
    }
}

PythonEngine::~PythonEngine()
{
    stopScript();
}

void PythonEngine::runScript(const QString &code)
{
    if (!_pyInterp)
    {
        emit scriptError(_initError.isEmpty()
            ? "Python interpreter failed to initialize."
            : _initError);
        return;
    }

    if (_running) { return; }

    _stopRequested = false;
    _running = true;

    {
        QMutexLocker lck(&_msgQueueMutex);
        _msgQueue.clear();
    }

    emit scriptStarted();

    if (_workerThread && _workerThread->joinable())
    {
        _workerThread->join();
    }

    _workerThread = std::make_unique<std::thread>(&PythonEngine::workerFunc, this, code.toStdString());
}

void PythonEngine::stopScript()
{
    _stopRequested = true;
    _msgQueueCondition.wakeAll();

    stopAllPeriodicTasks();

    if (!_workerThread || !_workerThread->joinable()) { return; }

    for (int i = 0; i < 50 && _running; i++)
    {
        _msgQueueCondition.wakeAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (_workerThread->joinable())
    {
        if (_running)
        {
            // Script did not stop within the timeout — detach to avoid blocking the UI.
            // Null g_activeEngine first so any subsequent pybind11 module API calls
            // inside the still-running thread return early instead of touching engine state.
            // Note: the thread still holds 'this' via member captures; PythonEngine
            // must not be destroyed until the thread naturally exits.
            g_activeEngine = nullptr;
            _workerThread->detach();
        }
        else
        {
            _workerThread->join();
        }
    }
    _workerThread.reset();
}

bool PythonEngine::isRunning() const
{
    return _running;
}

void PythonEngine::enqueueMessage(const BusMessage &msg)
{
    if (!_running) { return; }
    if (msg.busType() == BusType::CAN && !msg.isRX() && !_echoTxEnabled.load()) { return; }
    if (!passesRxFilter(msg)) { return; }

    QMutexLocker lck(&_msgQueueMutex);
    if (_msgQueue.size() < 10000)
    {
        _msgQueue.enqueue(msg);
        _msgQueueCondition.wakeOne();
    }
}

// ---------------------------------------------------------------------------
// RX filter
// ---------------------------------------------------------------------------

void PythonEngine::setRxFilter(uint32_t id, uint32_t mask, std::optional<bool> extended)
{
    QMutexLocker lck(&_rxFilterMutex);
    _rxFilter.id       = id;
    _rxFilter.mask     = mask;
    _rxFilter.extended = extended;
    _rxFilter.active   = true;
}

void PythonEngine::clearRxFilter()
{
    QMutexLocker lck(&_rxFilterMutex);
    _rxFilter.active = false;
}

bool PythonEngine::passesRxFilter(const BusMessage &msg) const
{
    QMutexLocker lck(&_rxFilterMutex);
    if (!_rxFilter.active) { return true; }
    if ((msg.getId() & _rxFilter.mask) != (_rxFilter.id & _rxFilter.mask)) { return false; }
    if (_rxFilter.extended.has_value() && msg.isExtended() != *_rxFilter.extended) { return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Periodic TX tasks
// ---------------------------------------------------------------------------

int PythonEngine::startPeriodicTask(BusMessage msg, unsigned interval_ms, uint16_t interface_id)
{
    // Allocate handle before taking the lock — only ever called from the Python
    // worker thread, so no concurrent access to _nextHandle.
    const int handle = _nextHandle++;
    auto task = std::make_shared<PeriodicTask>();

    // Construct the thread outside the lock so we don't hold _periodicMutex
    // during thread creation (which may briefly block on OS resources).
    task->thread = std::thread([this, msg, interval_ms, interface_id,
                                task_ptr = std::weak_ptr<PeriodicTask>(task)]() mutable
    {
        while (true)
        {
            auto task = task_ptr.lock();
            if (!task || task->stop.load() || _stopRequested.load()) { break; }

            BusInterface *intf = _backend.getInterfaceById(interface_id);
            if (intf)
            {
                BusMessage tx = msg;
                tx.setInterfaceId(interface_id);
                intf->sendMessage(tx);
            }

            const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds(interval_ms);
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto t = task_ptr.lock();
                if (!t || t->stop.load() || _stopRequested.load()) { return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });

    QMutexLocker lck(&_periodicMutex);
    _periodicTasks.emplace(handle, std::move(task));
    return handle;
}

void PythonEngine::stopPeriodicTask(int handle)
{
    std::shared_ptr<PeriodicTask> task;
    {
        QMutexLocker lck(&_periodicMutex);
        auto it = _periodicTasks.find(handle);
        if (it == _periodicTasks.end()) { return; }
        task = it->second;
        _periodicTasks.erase(it);
    }

    task->stop = true;
    if (task->thread.joinable()) { task->thread.join(); }
}

void PythonEngine::stopAllPeriodicTasks()
{
    std::map<int, std::shared_ptr<PeriodicTask>> tasks;
    {
        QMutexLocker lck(&_periodicMutex);
        tasks.swap(_periodicTasks);
    }

    for (auto &[handle, task] : tasks)
    {
        task->stop = true;
    }
    for (auto &[handle, task] : tasks)
    {
        if (task->thread.joinable()) { task->thread.join(); }
    }
}

// ---------------------------------------------------------------------------
// Script worker
// ---------------------------------------------------------------------------

void PythonEngine::workerFunc(std::string code)
{
    g_activeEngine = this;

    PyGILState_STATE gstate = PyGILState_Ensure();

    try
    {
        auto globals = py::globals();

        globals["_cangaroo_output"] = py::cpp_function(
            [this](const std::string &text, bool is_err)
            {
                const QString qtext = QString::fromStdString(text);
                if (is_err) { emit scriptError(qtext); }
                else        { emit scriptOutput(qtext); }
            });

        globals["_cangaroo_stop_check"] = py::cpp_function(
            [this]() -> bool { return _stopRequested.load(); });

        py::exec(R"(
import sys

class _SignalWriter:
    def __init__(self, is_err):
        self._is_err = is_err
    def write(self, text):
        if text:
            _cangaroo_output(text, self._is_err)
    def flush(self):
        pass

sys.stdout = _SignalWriter(False)
sys.stderr = _SignalWriter(True)
)");

        py::exec(R"(
import threading as _threading

def _cangaroo_trace(frame, event, arg):
    if _cangaroo_stop_check():
        raise KeyboardInterrupt("Script stopped by user")
    return _cangaroo_trace

sys.settrace(_cangaroo_trace)
_threading.settrace(_cangaroo_trace)
)");

        try
        {
            py::exec(code);
        }
        catch (py::error_already_set &e)
        {
            const QString err = QString::fromStdString(e.what());
            if (!err.contains("KeyboardInterrupt"))
            {
                emit scriptError(err + "\n");
            }
        }
    }
    catch (std::exception &e)
    {
        emit scriptError(QString::fromStdString(e.what()) + "\n");
    }

    PyGILState_Release(gstate);

    g_activeEngine = nullptr;
    _running = false;
    emit scriptFinished();
}
