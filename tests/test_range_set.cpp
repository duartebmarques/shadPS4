// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <tuple>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

#include "video_core/buffer_cache/range_set.h"

namespace {

using Map = VideoCore::RangeMap<u64>;
using Dumped = std::vector<std::tuple<VAddr, VAddr, u64>>;
using Gaps = std::vector<std::pair<VAddr, u64>>;

Dumped Dump(const Map& map) {
    Dumped out;
    map.ForEach([&](VAddr lo, VAddr hi, u64 tick) { out.emplace_back(lo, hi, tick); });
    return out;
}

Dumped DumpInRange(const Map& map, VAddr addr, u64 size) {
    Dumped out;
    map.ForEachInRange(addr, size,
                       [&](VAddr lo, VAddr hi, u64 tick) { out.emplace_back(lo, hi, tick); });
    return out;
}

} // namespace

TEST(RangeMap, SetJoinsEqualValues) {
    Map map;
    map.Set(0x100, 0x100, 1);
    map.Set(0x200, 0x100, 1);
    EXPECT_EQ(Dump(map), (Dumped{{0x100, 0x300, 1}}));
    EXPECT_EQ(map.Size(), 1u);
    EXPECT_TRUE(map.Contains(0x100, 0x200));
    EXPECT_FALSE(map.Intersects(0x300, 0x10));
    EXPECT_TRUE(map.Intersects(0x2FF, 0x10));
}

TEST(RangeMap, SetOverwritesTheOverlap) {
    Map map;
    map.Set(0x100, 0x400, 1);
    map.Set(0x200, 0x100, 2); // strictly inside: the newer value splits the old range
    EXPECT_EQ(Dump(map), (Dumped{{0x100, 0x200, 1}, {0x200, 0x300, 2}, {0x300, 0x500, 1}}));
    map.Set(0x000, 0x180, 3); // head overlap
    map.Set(0x480, 0x100, 3); // tail overlap
    EXPECT_EQ(Dump(map), (Dumped{{0x000, 0x180, 3},
                                 {0x180, 0x200, 1},
                                 {0x200, 0x300, 2},
                                 {0x300, 0x480, 1},
                                 {0x480, 0x580, 3}}));
    map.Set(0x000, 0x1000, 4); // everything
    EXPECT_EQ(Dump(map), (Dumped{{0x000, 0x1000, 4}}));
}

TEST(RangeMap, SetInsideAnEqualValueKeepsOneRange) {
    Map map;
    map.Set(0x100, 0x400, 1);
    map.Set(0x200, 0x100, 1);
    map.Set(0x100, 0x400, 1);
    EXPECT_EQ(Dump(map), (Dumped{{0x100, 0x500, 1}}));
    map.Set(0x200, 0, 2); // empty: no-op
    EXPECT_EQ(Dump(map), (Dumped{{0x100, 0x500, 1}}));
}

TEST(RangeMap, SubtractSplitsAndClips) {
    Map map;
    map.Set(0x100, 0x400, 1);
    map.Subtract(0x200, 0x100); // strictly inside: split
    EXPECT_EQ(Dump(map), (Dumped{{0x100, 0x200, 1}, {0x300, 0x500, 1}}));
    map.Subtract(0x000, 0x180); // head clip of the first
    map.Subtract(0x480, 0x100); // tail clip of the second
    EXPECT_EQ(Dump(map), (Dumped{{0x180, 0x200, 1}, {0x300, 0x480, 1}}));
    map.Subtract(0x000, 0x1000); // everything
    EXPECT_EQ(Dump(map), Dumped{});
    EXPECT_EQ(map.Size(), 0u);
}

TEST(RangeMap, ForEachInRangeClipsToTheQuery) {
    Map map;
    map.Set(0x100, 0x100, 1);
    map.Set(0x300, 0x100, 2);
    EXPECT_EQ(DumpInRange(map, 0x180, 0x200), (Dumped{{0x180, 0x200, 1}, {0x300, 0x380, 2}}));
    EXPECT_EQ(DumpInRange(map, 0x200, 0x100), Dumped{});
    Gaps gaps;
    map.ForEachNotInRange(0x000, 0x500,
                          [&](VAddr addr, u64 size) { gaps.emplace_back(addr, size); });
    EXPECT_EQ(gaps, (Gaps{{0x000, 0x100}, {0x200, 0x100}, {0x400, 0x100}}));
}
