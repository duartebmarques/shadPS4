// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <utility>
#include <vector>

#include "common/interval_set.h"
#include "common/types.h"

namespace Vulkan {

/// Buffer access sets for barrier tracking, with the same answers as sorted interval lists.
/// Accesses are appended to per-buffer logs in recording order and folded into the sorted lists
/// only when a query needs them, so the many reads no write ever checks cost one append each.
/// A guest sync point only bumps a counter: the log entries recorded before it are the ordered
/// accesses. Buffers are found through an open-addressing table and every set is dropped at once
/// by an epoch bump, keeping the allocations for the next round.
class BarrierTracker {
public:
    struct Stats {
        u64 records;
        u64 folds;
        u64 states;
    };

    /// `track_order` keeps what OverlapsOrdered needs.
    explicit BarrierTracker(bool track_order);

    /// Makes `handle` current, creating its empty sets when `create` is set. Returns false when
    /// the buffer has no sets.
    bool Select(const void* handle, bool create);

    /// Adds a range to the current buffer's sets. A guest access stays unordered until the next
    /// guest sync; any other access is ordered at once.
    void Record(u64 offset, u64 size, bool read, bool write, bool guest);

    /// Returns true when the range overlaps a recorded write, or also a read with `check_read`.
    bool Overlaps(u64 offset, u64 size, bool check_read);

    /// Returns true when the range overlaps an ordered access, for a range Overlaps found with
    /// `check_read`.
    bool OverlapsOrdered(u64 offset, u64 size);

    /// Orders every access recorded so far.
    void NoteGuestSync() {
        ++sync_gen;
        current = nullptr;
    }

    /// Drops every set.
    void Clear();

    Stats DrainStats() {
        return std::exchange(stats, Stats{});
    }

private:
    struct List : IntervalList<Interval> {
        /// Adds sorted, disjoint ranges in one pass over the list.
        void AddSorted(std::span<const Interval> ranges, std::vector<Interval>& scratch);
    };

    struct State {
        std::vector<Interval> read_log;
        std::vector<Interval> write_log;
        std::vector<Interval> ordered_log;
        List reads;
        List writes;
        List ordered;
        std::size_t reads_folded;
        std::size_t writes_folded;
        std::size_t ordered_reads_folded;
        std::size_t ordered_writes_folded;
        std::size_t ordered_log_folded;
        std::size_t read_mark;
        std::size_t write_mark;
        u64 sync_seen;
        bool unordered;
    };

    struct Slot {
        const void* key;
        u32 epoch;
        u32 state;
    };

    void Fold(List& list, const std::vector<Interval>& log, std::size_t& folded, std::size_t end);
    void Rehash(std::size_t capacity);

    std::vector<Slot> slots;
    std::vector<State> states;
    std::vector<Interval> batch;
    std::vector<Interval> scratch;
    std::size_t num_states{};
    u32 epoch{1};
    const void* current_key{};
    State* current{};
    u64 sync_gen{};
    bool track_order;
    Stats stats{};
};

} // namespace Vulkan
