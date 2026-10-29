// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "HeadsetFramePacing.h"

#include <cstdint>

using oxrsys::HeadsetDisplayClock;
using oxrsys::HeadsetPhaseState;
using oxrsys::NextHeadsetLockedDeadline;
using oxrsys::UpdateHeadsetPhase;

namespace
{
constexpr int64_t kMs = 1'000'000;
constexpr int64_t kPeriodNs = 13'888'889; // 72 Hz
} // namespace

TEST_CASE("Headset display clock: the least delayed packet sets the offset", "[headset-pacing]")
{
    HeadsetDisplayClock clock;
    // The headset's clock runs 5 s behind this machine's, and packets take 1 to 3 ms to arrive
    const int64_t clockOffsetNs = 5'000 * kMs;
    const int64_t delaysNs[] = {3 * kMs, 1 * kMs, 2 * kMs};
    int64_t displayTimeNs = 100 * kMs;
    int64_t receiveTimeNs = 0;
    for (const int64_t delayNs : delaysNs)
    {
        displayTimeNs += kPeriodNs;
        receiveTimeNs = displayTimeNs + clockOffsetNs + delayNs;
        clock.AddSample(receiveTimeNs, displayTimeNs);
    }

    int64_t frameTimeNs = 0;
    REQUIRE(clock.GetLatestFrameTime(receiveTimeNs, 100 * kMs, frameTimeNs));
    // The latest frame is placed with the smallest delay seen, not its own 2 ms
    CHECK(frameTimeNs == displayTimeNs + clockOffsetNs + 1 * kMs);
}

TEST_CASE("Headset display clock: no frame time once packets stop", "[headset-pacing]")
{
    HeadsetDisplayClock clock;
    int64_t frameTimeNs = 0;
    CHECK_FALSE(clock.GetLatestFrameTime(0, 100 * kMs, frameTimeNs));

    clock.AddSample(1'000 * kMs, 10 * kMs);
    CHECK(clock.GetLatestFrameTime(1'050 * kMs, 100 * kMs, frameTimeNs));
    CHECK_FALSE(clock.GetLatestFrameTime(1'200 * kMs, 100 * kMs, frameTimeNs));
}

TEST_CASE("Headset display clock: a pause forgets the offsets from before it", "[headset-pacing]")
{
    HeadsetDisplayClock clock;
    // The offset before the pause is smaller, so it would win if it were kept
    clock.AddSample(1'000 * kMs, 990 * kMs);
    clock.AddSample(5'000 * kMs, 4'986 * kMs);

    int64_t frameTimeNs = 0;
    REQUIRE(clock.GetLatestFrameTime(5'000 * kMs, 100 * kMs, frameTimeNs));
    CHECK(frameTimeNs == 4'986 * kMs + 14 * kMs);
}

TEST_CASE("Headset locked deadline: the first headset frame after the previous deadline",
          "[headset-pacing]")
{
    const int64_t headsetFrameNs = 1'000 * kMs;

    const int64_t earlierPreviousNs = headsetFrameNs - 3 * kPeriodNs;
    CHECK(NextHeadsetLockedDeadline(headsetFrameNs, 0, kPeriodNs, earlierPreviousNs) ==
          headsetFrameNs - 2 * kPeriodNs);
    CHECK(NextHeadsetLockedDeadline(headsetFrameNs, 5 * kMs, kPeriodNs, earlierPreviousNs) ==
          headsetFrameNs - 2 * kPeriodNs + 5 * kMs);

    const int64_t laterPreviousNs = headsetFrameNs + 2 * kPeriodNs;
    CHECK(NextHeadsetLockedDeadline(headsetFrameNs, 0, kPeriodNs, laterPreviousNs) ==
          headsetFrameNs + 3 * kPeriodNs);
}

TEST_CASE("Headset locked deadline: never sooner than half a period after the previous one",
          "[headset-pacing]")
{
    const int64_t headsetFrameNs = 1'000 * kMs;
    // The previous deadline fell 2 ms before a headset frame, too close to use that one
    const int64_t previousNs = headsetFrameNs - 2 * kMs;
    CHECK(NextHeadsetLockedDeadline(headsetFrameNs, 0, kPeriodNs, previousNs) ==
          headsetFrameNs + kPeriodNs);
}

TEST_CASE("Headset phase: three stale reports in a row move frames half a period", "[headset-pacing]")
{
    HeadsetPhaseState state;
    CHECK_FALSE(UpdateHeadsetPhase(state, 1, 10, true, kPeriodNs));
    // The same report seen on a later frame doesn't count twice
    CHECK_FALSE(UpdateHeadsetPhase(state, 1, 10, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 2, 12, true, kPeriodNs));
    CHECK(state.phaseOffsetNs == 0);

    CHECK(UpdateHeadsetPhase(state, 3, 12, true, kPeriodNs));
    CHECK(state.phaseOffsetNs == kPeriodNs / 2);

    // The report after a move still covers frames from before it, so it doesn't count. Three
    // more stale ones move frames on around the period rather than past it.
    CHECK_FALSE(UpdateHeadsetPhase(state, 4, 9, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 5, 9, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 6, 9, true, kPeriodNs));
    CHECK(UpdateHeadsetPhase(state, 7, 9, true, kPeriodNs));
    CHECK(state.phaseOffsetNs < kPeriodNs);
}

TEST_CASE("Headset phase: a short headset hiccup doesn't move frames", "[headset-pacing]")
{
    HeadsetPhaseState state;
    CHECK_FALSE(UpdateHeadsetPhase(state, 1, 14, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 2, 5, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 3, 0, true, kPeriodNs));
    CHECK(state.phaseOffsetNs == 0);
}

TEST_CASE("Headset phase: a clean report or a late app starts the count over", "[headset-pacing]")
{
    HeadsetPhaseState state;
    CHECK_FALSE(UpdateHeadsetPhase(state, 1, 10, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 2, 10, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 3, 0, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 4, 10, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 5, 10, true, kPeriodNs));
    // The app ran late that second, so its stale frames don't count against the phase
    CHECK_FALSE(UpdateHeadsetPhase(state, 6, 10, false, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 7, 10, true, kPeriodNs));
    CHECK_FALSE(UpdateHeadsetPhase(state, 8, 10, true, kPeriodNs));
    CHECK(state.phaseOffsetNs == 0);
}
