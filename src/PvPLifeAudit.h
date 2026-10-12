/*
 * PvP Life Audit Logger (Thread-Safe Asynchronous Bounded File Writer)
 * Copyright (C) 2026 iCore
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PVP_LIFE_AUDIT_H
#define PVP_LIFE_AUDIT_H

#include "GameTime.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <chrono>

namespace PvPLife
{
    class AuditLogger
    {
    public:
        static AuditLogger& Instance()
        {
            static AuditLogger instance;
            return instance;
        }

        void Configure(bool enabled, std::string const& filePath)
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            _enabled = enabled;
            if (_filePath != filePath)
            {
                _filePath = filePath;
                _pathChanged = true;
            }

            if (_enabled && !_running)
                StartWorker();
        }

        void Log(std::string const& tag, std::string const& message)
        {
            if (!_enabled)
                return;

            std::string line = std::to_string(GameTime::GetGameTime().count()) + " [" + tag + "] " + message + "\n";

            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                if (_queue.size() < MaxQueueSize)
                {
                    _queue.push(std::move(line));
                }
                else
                {
                    ++_droppedLines;
                }
            }
            _cv.notify_one();
        }

        uint32_t GetDroppedCount() const { return _droppedLines.load(std::memory_order_relaxed); }
        uint32_t GetOpenFailures() const { return _openFailures.load(std::memory_order_relaxed); }
        uint32_t GetWriteFailures() const { return _writeFailures.load(std::memory_order_relaxed); }
        uint32_t GetLinesWritten() const { return _linesWritten.load(std::memory_order_relaxed); }
        size_t GetQueueSize()
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            return _queue.size();
        }

        void Shutdown()
        {
            if (!_running.load(std::memory_order_acquire))
                return;

            _stopping.store(true, std::memory_order_release);
            _cv.notify_all();

            if (_workerThread.joinable())
                _workerThread.join();

            _running.store(false, std::memory_order_release);
            _stopping.store(false, std::memory_order_release);
        }

        ~AuditLogger()
        {
            Shutdown();
        }

    private:
        AuditLogger() = default;

        void StartWorker()
        {
            if (_running.load(std::memory_order_acquire))
                return;

            _stopping.store(false, std::memory_order_release);
            _running.store(true, std::memory_order_release);
            _workerThread = std::thread([this]() { WorkerLoop(); });
        }

        void WorkerLoop()
        {
            std::ofstream file;
            std::string currentPath;
            std::queue<std::string> pendingBuffer;
            auto lastRetryTime = std::chrono::steady_clock::now();

            while (true)
            {
                std::string targetPath;
                bool needReopen = false;
                bool stopping = false;

                {
                    std::unique_lock<std::mutex> lock(_queueMutex);
                    _cv.wait_for(lock, std::chrono::milliseconds(500), [this]()
                    {
                        return !_queue.empty() || _stopping.load(std::memory_order_acquire);
                    });

                    stopping = _stopping.load(std::memory_order_acquire);

                    if (_pathChanged || currentPath.empty())
                    {
                        targetPath = _filePath;
                        _pathChanged = false;
                        needReopen = true;
                    }

                    while (!_queue.empty())
                    {
                        if (pendingBuffer.size() < MaxQueueSize)
                        {
                            pendingBuffer.push(std::move(_queue.front()));
                        }
                        else
                        {
                            ++_droppedLines;
                        }
                        _queue.pop();
                    }
                }

                auto now = std::chrono::steady_clock::now();
                if (needReopen || (!file.is_open() && !targetPath.empty() && now - lastRetryTime >= std::chrono::seconds(5)))
                {
                    lastRetryTime = now;
                    if (file.is_open())
                        file.close();

                    currentPath = targetPath.empty() ? _filePath : targetPath;
                    file.open(currentPath, std::ios::app);
                    if (!file.is_open())
                    {
                        ++_openFailures;
                    }
                }

                if (file.is_open())
                {
                    while (!pendingBuffer.empty())
                    {
                        file << pendingBuffer.front();
                        if (file.fail())
                        {
                            ++_writeFailures;
                            file.clear();
                            break;
                        }
                        pendingBuffer.pop();
                        ++_linesWritten;
                    }
                    file.flush();
                }
                else if (stopping)
                {
                    _writeFailures += static_cast<uint32_t>(pendingBuffer.size());
                    while (!pendingBuffer.empty())
                        pendingBuffer.pop();
                    break;
                }

                if (stopping && pendingBuffer.empty())
                    break;
            }

            if (file.is_open())
                file.close();
        }

        static constexpr size_t MaxQueueSize = 5000;
        std::atomic<bool> _enabled{false};
        std::atomic<bool> _running{false};
        std::atomic<bool> _stopping{false};
        bool _pathChanged{false};
        std::string _filePath{"Logs/pvplife_audit.log"};

        std::mutex _queueMutex;
        std::condition_variable _cv;
        std::queue<std::string> _queue;
        std::atomic<uint32_t> _droppedLines{0};
        std::atomic<uint32_t> _openFailures{0};
        std::atomic<uint32_t> _writeFailures{0};
        std::atomic<uint32_t> _linesWritten{0};
        std::thread _workerThread;
    };
}

#define sPvPLifeAudit PvPLife::AuditLogger::Instance()

#endif
