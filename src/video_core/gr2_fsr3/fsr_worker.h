// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "common/types.h"
#include "common/unique_function.h"

namespace Gr2Fsr3 {

/// A thread that runs queued tasks one at a time, in order. It starts with the first task.
class Worker {
public:
    explicit Worker(const char* thread_name);

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Callable from any thread.
    void Enqueue(Common::UniqueFunction<void>&& task);

private:
    void Loop(std::stop_token stop_token);

    const char* thread_name;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Common::UniqueFunction<void>> tasks;
    std::jthread thread;
};

// A failure of a provider call recorded through Scheduler::Record, kept with the generation of
// its context. The recorded calls report it; the GPU command thread takes it.
class RecordedFailure {
public:
    void Report(u64 generation, std::string text) {
        std::scoped_lock lock{mutex};
        failed_generation = generation;
        message = std::move(text);
    }

    bool HasFailed(u64 generation) {
        std::scoped_lock lock{mutex};
        return failed_generation == generation;
    }

    std::optional<std::string> Take(u64 generation) {
        std::scoped_lock lock{mutex};
        if (failed_generation != generation) {
            return std::nullopt;
        }
        return std::exchange(message, std::nullopt);
    }

private:
    std::mutex mutex;
    u64 failed_generation{}; ///< 0 until a context fails; generations count from 1.
    std::optional<std::string> message;
};

/// Whether the future is valid and its get() returns without blocking.
template <typename T>
bool IsReady(const std::future<T>& future) {
    return future.valid() && future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
}

} // namespace Gr2Fsr3
