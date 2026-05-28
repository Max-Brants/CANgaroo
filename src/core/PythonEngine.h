#pragma once

#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <thread>

#include "core/BusMessage.h"

class Backend;

class PythonEngine : public QObject
{
    Q_OBJECT

public:
    explicit PythonEngine(Backend &backend, QObject *parent = nullptr);
    ~PythonEngine() override;

    void runScript(const QString &code);
    void stopScript();
    bool isRunning() const;

    Backend &backend() { return _backend; }

    void enqueueMessage(const BusMessage &msg);

    QMutex &msgQueueMutex() { return _msgQueueMutex; }
    QWaitCondition &msgQueueCondition() { return _msgQueueCondition; }
    QQueue<BusMessage> &msgQueue() { return _msgQueue; }
    bool stopRequested() const { return _stopRequested.load(); }

    // RX filter — applied in enqueueMessage before the message enters the queue
    void setRxFilter(uint32_t id, uint32_t mask, std::optional<bool> extended);
    void clearRxFilter();

    // TX echo — when disabled (default), sent frames are not fed back into receive()
    void setTxEchoEnabled(bool enabled) { _echoTxEnabled = enabled; }

    // Periodic TX tasks — each runs on its own std::thread
    int  startPeriodicTask(BusMessage msg, unsigned interval_ms, uint16_t interface_id);
    void stopPeriodicTask(int handle);
    void stopAllPeriodicTasks();

signals:
    void scriptOutput(const QString &text);
    void scriptError(const QString &text);
    void scriptStarted();
    void scriptFinished();

private:
    Backend &_backend;

    struct PyInterpreterHolder;
    static std::shared_ptr<PyInterpreterHolder> sharedInterpreter();
    std::shared_ptr<PyInterpreterHolder> _pyInterp;
    QString _initError;

    std::unique_ptr<std::thread> _workerThread;
    std::atomic<bool> _running{false};
    std::atomic<bool> _stopRequested{false};
    std::atomic<bool> _echoTxEnabled{false};

    QMutex _msgQueueMutex;
    QWaitCondition _msgQueueCondition;
    QQueue<BusMessage> _msgQueue;

    // RX filter state
    struct RxFilter
    {
        uint32_t id{0};
        uint32_t mask{0};
        std::optional<bool> extended;
        bool active{false};
    };
    RxFilter _rxFilter;
    mutable QMutex _rxFilterMutex;

    [[nodiscard]] bool passesRxFilter(const BusMessage &msg) const;

    // Periodic TX tasks
    struct PeriodicTask
    {
        std::atomic<bool> stop{false};
        std::thread thread;

        PeriodicTask() = default;
        PeriodicTask(const PeriodicTask &) = delete;
        PeriodicTask &operator=(const PeriodicTask &) = delete;
    };

    QMutex _periodicMutex;
    std::map<int, std::shared_ptr<PeriodicTask>> _periodicTasks;
    int _nextHandle{0};

    void workerFunc(std::string code);
};
