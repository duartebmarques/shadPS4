// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>

#include "common/thread.h"
#include "video_core/gr2_fsr3/fsr_worker.h"

namespace Gr2Fsr3 {

Worker::Worker(const char* thread_name) : thread_name{thread_name} {}

void Worker::Enqueue(Common::UniqueFunction<void>&& task) {
    {
        std::scoped_lock lock{mutex};
        tasks.push_back(std::move(task));
        if (!thread.joinable()) {
            thread = std::jthread{[this](std::stop_token stop_token) { Loop(stop_token); }};
        }
    }
    cv.notify_one();
}

void Worker::Loop(std::stop_token stop_token) {
    Common::SetCurrentThreadName(thread_name);
    // On Linux the thread inherits the affinity of the thread that starts it.
    if (const u64 strip = Common::GetExclusionStripMask()) {
        Common::SetCurrentThreadAffinityMask(~strip);
    }
    while (!stop_token.stop_requested()) {
        Common::UniqueFunction<void> task;
        {
            std::unique_lock lock{mutex};
            cv.wait(lock, stop_token, [this] { return !tasks.empty(); });
            if (stop_token.stop_requested()) {
                break;
            }
            task = std::move(tasks.front());
            tasks.pop_front();
        }
        task();
    }
}

} // namespace Gr2Fsr3
