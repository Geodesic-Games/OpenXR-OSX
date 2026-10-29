// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Frame pacing locked to the headset's display, factored out of Session and TrackingReceiver
// so it is unit-testable. The app and the headset each run at the display rate on their own
// clocks. Left free-running, their phase is wherever the app happened to start, and after
// every hitch it lands somewhere new. When frames reach the headset right as it picks up the
// next one, it shows stale, repeated frames for as long as the phase stays there.
namespace oxrsys
{

// Places the headset's frames on this machine's clock. Every tracking packet carries the
// display time of the headset frame it was sent from, in the headset's clock, and arrives
// once per headset frame. Its receive time minus that display time is the clock offset plus
// the packet's delay, so the smallest value over the last second is the offset with the
// least delay.
class HeadsetDisplayClock
{
public:
    void AddSample(int64_t receiveTimeNs, int64_t displayTimeNs)
    {
        // After a pause the headset may have reconnected, so older offsets no longer apply
        if (count_ > 0 && receiveTimeNs - latestReceiveTimeNs_ > kResetGapNs)
        {
            count_ = 0;
            next_ = 0;
        }
        offsetsNs_[next_] = receiveTimeNs - displayTimeNs;
        next_ = (next_ + 1) % offsetsNs_.size();
        count_ = std::min(count_ + 1, offsetsNs_.size());
        latestReceiveTimeNs_ = receiveTimeNs;
        latestDisplayTimeNs_ = displayTimeNs;
    }

    // When the headset's latest frame went out, on this machine's clock. False when no
    // packet arrived within maxAgeNs.
    bool GetLatestFrameTime(int64_t nowNs, int64_t maxAgeNs, int64_t& outTimeNs) const
    {
        if (count_ == 0 || nowNs - latestReceiveTimeNs_ > maxAgeNs)
        {
            return false;
        }
        const int64_t offsetNs = *std::min_element(offsetsNs_.begin(), offsetsNs_.begin() + count_);
        outTimeNs = latestDisplayTimeNs_ + offsetNs;
        return true;
    }

private:
    static constexpr int64_t kResetGapNs = 1'000'000'000;

    // About a second of packets at 72 Hz
    std::array<int64_t, 72> offsetsNs_ = {};
    size_t next_ = 0;
    size_t count_ = 0;
    int64_t latestReceiveTimeNs_ = 0;
    int64_t latestDisplayTimeNs_ = 0;
};

// The next frame deadline on the headset's frame grid: the first headset frame time, shifted
// by the phase offset, at least half a period after the previous deadline. A frame that ran a
// little late is made up by the next one instead of two frames starting back to back.
inline int64_t NextHeadsetLockedDeadline(int64_t headsetFrameTimeNs, int64_t phaseOffsetNs,
                                         int64_t periodNs, int64_t previousDeadlineNs)
{
    const int64_t anchorNs = headsetFrameTimeNs + phaseOffsetNs;
    const int64_t earliestNs = previousDeadlineNs + periodNs / 2;
    // Division truncates toward zero, which rounds up below the anchor and down above it
    int64_t periods = (earliestNs - anchorNs) / periodNs;
    if (anchorNs + periods * periodNs < earliestNs)
    {
        ++periods;
    }
    return anchorNs + periods * periodNs;
}

// Stale frames in one headset report (about a second) that count against the current phase
constexpr uint32_t kHeadsetPhaseStaleFrames = 4;

// Reports in a row it takes. The headset itself sometimes shows stale frames for a second or two
// while frames arrive on time; a bad phase keeps showing them.
constexpr uint32_t kHeadsetPhaseStaleReports = 3;

struct HeadsetPhaseState
{
    uint64_t lastReportCount = 0;
    uint32_t staleReportsInARow = 0;
    bool skipNextReport = false;
    int64_t phaseOffsetNs = 0;
};

// Moves frames half a period once several headset reports in a row show stale frames while the
// app kept its frame rate, which puts them midway between the headset's pickups. Reports from
// a second where the app itself ran late say nothing about the phase and don't count, and
// neither does the report after a move, which covers frames from before it. Returns true when
// the phase moved.
inline bool UpdateHeadsetPhase(HeadsetPhaseState& state, uint64_t reportCount, uint32_t staleFrames,
                               bool appKeptUp, int64_t periodNs)
{
    if (reportCount == state.lastReportCount)
    {
        return false;
    }
    state.lastReportCount = reportCount;

    if (state.skipNextReport || !appKeptUp || staleFrames < kHeadsetPhaseStaleFrames)
    {
        state.skipNextReport = false;
        state.staleReportsInARow = 0;
        return false;
    }
    if (++state.staleReportsInARow < kHeadsetPhaseStaleReports)
    {
        return false;
    }

    state.staleReportsInARow = 0;
    state.skipNextReport = true;
    state.phaseOffsetNs = (state.phaseOffsetNs + periodNs / 2) % periodNs;
    return true;
}

} // namespace oxrsys
