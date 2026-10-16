// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "video_core/renderer_vulkan/vk_barrier_tracker.h"

namespace Vulkan {

namespace {

std::size_t SlotHash(const void* key, std::size_t mask) {
    const u64 x = reinterpret_cast<uintptr_t>(key) * 0x9E3779B97F4A7C15ULL;
    return static_cast<std::size_t>(x >> 32) & mask;
}

void Append(std::vector<Interval>& log, const Interval& range) {
    if (!log.empty() && log.back().start <= range.start && range.end <= log.back().end) {
        return;
    }
    log.push_back(range);
}

} // Anonymous namespace

BarrierTracker::BarrierTracker(bool track_order_) : track_order{track_order_} {
    slots.resize(256);
}

void BarrierTracker::List::AddSorted(std::span<const Interval> ranges,
                                     std::vector<Interval>& scratch) {
    if (ranges.empty()) {
        return;
    }
    // Intervals ending before the first range stay where they are: only the tail from there is
    // merged, and accesses mostly land near the end of a buffer's list.
    const auto first = std::ranges::lower_bound(intervals, ranges.front().start, {}, &Interval::end);
    scratch.clear();
    scratch.reserve(std::distance(first, intervals.end()) + ranges.size());
    const auto push = [&scratch](const Interval& range) {
        if (!scratch.empty() && range.start <= scratch.back().end) {
            scratch.back().end = std::max(scratch.back().end, range.end);
        } else {
            scratch.push_back(range);
        }
    };
    auto it = first;
    for (const Interval& range : ranges) {
        for (; it != intervals.end() && it->start < range.start; ++it) {
            push(*it);
        }
        push(range);
    }
    for (; it != intervals.end(); ++it) {
        push(*it);
    }
    intervals.erase(first, intervals.end());
    intervals.insert(intervals.end(), scratch.begin(), scratch.end());
}

void BarrierTracker::Fold(List& list, const std::vector<Interval>& log, std::size_t& folded,
                          std::size_t end) {
    if (folded == end) {
        return;
    }
    ++stats.folds;
    if (end - folded == 1) {
        const Interval& range = log[folded];
        if (list.FindCovering(range.start, range.end) == list.end()) {
            list.Add(range);
        }
        folded = end;
        return;
    }
    batch.assign(log.begin() + folded, log.begin() + end);
    folded = end;
    std::ranges::sort(batch, {}, &Interval::start);
    std::size_t n = 0;
    for (const Interval& range : batch) {
        if (n != 0 && range.start <= batch[n - 1].end) {
            batch[n - 1].end = std::max(batch[n - 1].end, range.end);
        } else {
            batch[n++] = range;
        }
    }
    list.AddSorted(std::span{batch.data(), n}, scratch);
}

void BarrierTracker::Rehash(std::size_t capacity) {
    std::vector<Slot> old = std::exchange(slots, std::vector<Slot>(capacity));
    const std::size_t mask = capacity - 1;
    for (const Slot& slot : old) {
        if (slot.epoch != epoch) {
            continue;
        }
        std::size_t i = SlotHash(slot.key, mask);
        while (slots[i].epoch == epoch) {
            i = (i + 1) & mask;
        }
        slots[i] = slot;
    }
}

bool BarrierTracker::Select(const void* handle, bool create) {
    if (current && current_key == handle) {
        return true;
    }
    const std::size_t mask = slots.size() - 1;
    std::size_t i = SlotHash(handle, mask);
    while (slots[i].epoch == epoch && slots[i].key != handle) {
        i = (i + 1) & mask;
    }
    if (slots[i].epoch != epoch) {
        if (!create) {
            return false;
        }
        if ((num_states + 1) * 2 > slots.size()) {
            Rehash(slots.size() * 2);
            return Select(handle, create);
        }
        if (num_states == states.size()) {
            states.emplace_back();
        }
        State& state = states[num_states];
        state.read_log.clear();
        state.write_log.clear();
        state.ordered_log.clear();
        state.reads.Clear();
        state.writes.Clear();
        state.ordered.Clear();
        state.reads_folded = state.writes_folded = 0;
        state.ordered_reads_folded = state.ordered_writes_folded = state.ordered_log_folded = 0;
        state.read_mark = state.write_mark = 0;
        state.sync_seen = sync_gen;
        state.unordered = false;
        slots[i] = {handle, epoch, static_cast<u32>(num_states)};
        ++num_states;
        ++stats.states;
    }
    current_key = handle;
    current = &states[slots[i].state];
    if (current->sync_seen != sync_gen) {
        current->sync_seen = sync_gen;
        current->read_mark = current->read_log.size();
        current->write_mark = current->write_log.size();
        current->unordered = false;
    }
    return true;
}

void BarrierTracker::Record(u64 offset, u64 size, bool read, bool write, bool guest) {
    if (size == 0 || (!read && !write)) {
        return;
    }
    ++stats.records;
    const Interval range{offset, offset + size};
    if (write) {
        Append(current->write_log, range);
    }
    if (read) {
        Append(current->read_log, range);
    }
    if (!track_order) {
        return;
    }
    if (guest) {
        current->unordered = true;
    } else {
        current->ordered_log.push_back(range);
    }
}

bool BarrierTracker::Overlaps(u64 offset, u64 size, bool check_read) {
    State& state = *current;
    Fold(state.writes, state.write_log, state.writes_folded, state.write_log.size());
    if (state.writes.Overlaps(offset, offset + size)) {
        return true;
    }
    if (!check_read) {
        return false;
    }
    Fold(state.reads, state.read_log, state.reads_folded, state.read_log.size());
    return state.reads.Overlaps(offset, offset + size);
}

bool BarrierTracker::OverlapsOrdered(u64 offset, u64 size) {
    State& state = *current;
    // Without guest accesses since the last sync, every recorded access is ordered.
    if (!state.unordered) {
        return true;
    }
    Fold(state.ordered, state.read_log, state.ordered_reads_folded, state.read_mark);
    Fold(state.ordered, state.write_log, state.ordered_writes_folded, state.write_mark);
    Fold(state.ordered, state.ordered_log, state.ordered_log_folded, state.ordered_log.size());
    return state.ordered.Overlaps(offset, offset + size);
}

void BarrierTracker::Clear() {
    num_states = 0;
    current = nullptr;
    current_key = nullptr;
    if (++epoch == 0) {
        for (Slot& slot : slots) {
            slot.epoch = 0;
        }
        epoch = 1;
    }
}

} // namespace Vulkan
