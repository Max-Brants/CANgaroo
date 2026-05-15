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

#include "api/candle.h"

#include <QList>
#include <QMutex>
#include <QWaitCondition>

#include <atomic>
#include <thread>

// Shared state for all CAN channels on a single physical gs_usb device.
//
// gs_usb devices multiplex all channels through one USB bulk IN endpoint.
// Opening the device multiple times creates competing overlapped reads on that
// endpoint, so frames for channel 1 are silently consumed by channel 0's reads
// and lost. This struct owns exactly ONE USB handle and ONE reader thread per
// physical device; all CandleApiInterface instances for the same device share
// it and read from their per-channel queue.
struct CandleSharedDevice
{
    static constexpr int MAX_CHANNELS = 8;

    candle_handle handle{nullptr};

    // Open/close reference count — protected by openMutex.
    // The USB device is opened when the count reaches 1 and closed when it
    // drops back to 0.
    QMutex openMutex;
    int openCount{0};

    // Serialises candle_frame_send / candle_fd_frame_send calls. Both channels
    // share the same USB bulk OUT endpoint; without a mutex two concurrent sends
    // could interleave. The tryLock timeout in sendMessage() ensures that a
    // stuck write on one channel does not block the other channel indefinitely.
    QMutex writeMutex;

    // Per-channel receive queues fed by the background reader thread.
    QMutex queueMutex;
    QWaitCondition queueCond;
    QList<candle_fd_frame_t> rxQueues[MAX_CHANNELS];

    // Shared timestamp epoch captured once at device open time so that all
    // channels on the same physical device use an identical baseline.
    uint64_t hostOffsetStart{0};
    uint32_t deviceTicksStart{0};

    // Background reader thread: calls candle_fd_frame_read() in a loop and
    // routes each received frame to the appropriate channel queue.
    std::thread readerThread;
    std::atomic<bool> readerRunning{false};

    ~CandleSharedDevice();

    void startReader();
    void stopReader();

    // Block until a frame for channel arrives or timeout_ms elapses.
    bool readFrame(uint8_t channel, candle_fd_frame_t &frame, unsigned timeout_ms);
};
