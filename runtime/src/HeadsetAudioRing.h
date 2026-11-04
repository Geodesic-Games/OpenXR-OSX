// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace oxrsys
{

// Stereo 16-bit audio on its way from Core Audio's IO thread to the thread that sends it to
// the headset. One thread writes and one reads, and neither locks or allocates, so the IO
// thread never waits. Reading favors fresh audio: a reader that fell behind skips to the
// newest frames instead of sending a backlog the headset would hear late.
class HeadsetAudioRing
{
public:
    static constexpr uint32_t Channels = 2;

    explicit HeadsetAudioRing(uint32_t capacityFrames)
        : samples_(static_cast<size_t>(capacityFrames) * Channels),
          capacityFrames_(capacityFrames)
    {
    }

    // Writer side. left and right point at the first frame's two channels and stride is the
    // distance in floats from one frame to the next, so interleaved, planar and mono sources
    // all fit. Keeps nothing and returns false when the block doesn't fit, which only happens
    // while the reader is stalled.
    bool WriteFloat(const float* left, const float* right, uint32_t stride, uint32_t frameCount,
                    int64_t endTimeNs)
    {
        const uint64_t written = writtenFrames_.load(std::memory_order_relaxed);
        const uint64_t read = readFrames_.load(std::memory_order_acquire);
        if (frameCount > capacityFrames_ - static_cast<uint32_t>(written - read))
        {
            droppedFrames_.fetch_add(frameCount, std::memory_order_relaxed);
            return false;
        }

        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            int16_t* target = &samples_[((written + frame) % capacityFrames_) * Channels];
            target[0] = ToInt16(left[static_cast<size_t>(frame) * stride]);
            target[1] = ToInt16(right[static_cast<size_t>(frame) * stride]);
        }
        lastWriteEndTimeNs_.store(endTimeNs, std::memory_order_relaxed);
        writtenFrames_.store(written + frameCount, std::memory_order_release);
        return true;
    }

    // Reader side. Copies up to maxFrames of the oldest unread frames into `out`, after
    // skipping whatever is older than the newest maxBacklogFrames. Returns the frames copied.
    uint32_t Read(int16_t* out, uint32_t maxFrames, uint32_t maxBacklogFrames)
    {
        const uint64_t written = writtenFrames_.load(std::memory_order_acquire);
        uint64_t read = readFrames_.load(std::memory_order_relaxed);
        if (written - read > maxBacklogFrames)
        {
            skippedFrames_.fetch_add(written - read - maxBacklogFrames, std::memory_order_relaxed);
            read = written - maxBacklogFrames;
        }

        const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(maxFrames, written - read));
        uint32_t copied = 0;
        while (copied < count)
        {
            // Copy up to the end of the storage, then wrap around for the rest
            const uint32_t start = static_cast<uint32_t>((read + copied) % capacityFrames_);
            const uint32_t span = std::min(count - copied, capacityFrames_ - start);
            memcpy(out + static_cast<size_t>(copied) * Channels,
                   &samples_[static_cast<size_t>(start) * Channels],
                   static_cast<size_t>(span) * Channels * sizeof(int16_t));
            copied += span;
        }
        readFrames_.store(read + count, std::memory_order_release);
        return count;
    }

    uint32_t GetAvailableFrames() const
    {
        return static_cast<uint32_t>(writtenFrames_.load(std::memory_order_acquire) -
                                     readFrames_.load(std::memory_order_acquire));
    }

    // When the newest written frame was captured, in the writer's clock
    int64_t GetLastWriteEndTimeNs() const { return lastWriteEndTimeNs_.load(std::memory_order_relaxed); }
    uint64_t GetDroppedFrames() const { return droppedFrames_.load(std::memory_order_relaxed); }
    uint64_t GetSkippedFrames() const { return skippedFrames_.load(std::memory_order_relaxed); }

    // Only while neither side is running
    void Reset()
    {
        writtenFrames_.store(0, std::memory_order_relaxed);
        readFrames_.store(0, std::memory_order_relaxed);
        lastWriteEndTimeNs_.store(0, std::memory_order_relaxed);
        droppedFrames_.store(0, std::memory_order_relaxed);
        skippedFrames_.store(0, std::memory_order_relaxed);
    }

    static int16_t ToInt16(float value)
    {
        if (std::isnan(value))
        {
            return 0;
        }
        return static_cast<int16_t>(std::lrintf(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    }

private:
    std::vector<int16_t> samples_;
    const uint32_t capacityFrames_;
    std::atomic<uint64_t> writtenFrames_{0};
    std::atomic<uint64_t> readFrames_{0};
    std::atomic<int64_t> lastWriteEndTimeNs_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> skippedFrames_{0};
};

} // namespace oxrsys
