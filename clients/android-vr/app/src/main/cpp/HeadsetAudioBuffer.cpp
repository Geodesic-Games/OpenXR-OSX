// SPDX-License-Identifier: MPL-2.0

#include "HeadsetAudioBuffer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace oxr
{

namespace
{

// How quickly the smoothed cushion level follows the real one, per output callback
constexpr float kLevelSmoothing = 0.02f;

int16_t ReadLittleEndianSample(const uint8_t* bytes)
{
    int16_t sample = 0;
    memcpy(&sample, bytes, sizeof(sample));
    return sample;
}

} // namespace

HeadsetAudioBuffer::HeadsetAudioBuffer(const Settings& settings)
    : settings_(settings)
{
}

uint32_t HeadsetAudioBuffer::MillisecondsToFrames(uint32_t milliseconds) const
{
    return static_cast<uint32_t>(static_cast<uint64_t>(sampleRateHz_) * milliseconds / 1000);
}

void HeadsetAudioBuffer::Reset(uint32_t sampleRateHz)
{
    sampleRateHz_ = sampleRateHz;
    capacityFrames_ = std::max(MillisecondsToFrames(settings_.capacityMs), 1u);
    samples_.assign(static_cast<size_t>(capacityFrames_) * Channels, 0);
    writtenFrames_.store(0);
    readFrames_.store(0);

    playing_ = false;
    smoothedLevel_ = 0.0f;
    targetFrames_ = MillisecondsToFrames(settings_.targetMs);
    framesSinceNudge_ = 0;
    framesSinceUnderrun_ = 0;

    receivedFrames_.store(0);
    droppedFrames_.store(0);
    trimmedFrames_.store(0);
    nudgedFrames_.store(0);
    underruns_.store(0);
    reportedTargetFrames_.store(targetFrames_);
    peak_.store(0);
}

bool HeadsetAudioBuffer::Write(const uint8_t* sampleBytes, uint32_t frameCount, uint32_t channels)
{
    const uint64_t written = writtenFrames_.load(std::memory_order_relaxed);
    const uint64_t read = readFrames_.load(std::memory_order_acquire);
    if (capacityFrames_ == 0 || channels == 0 ||
        frameCount > capacityFrames_ - static_cast<uint32_t>(written - read))
    {
        droppedFrames_.fetch_add(frameCount, std::memory_order_relaxed);
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(channels) * sizeof(int16_t);
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        const uint8_t* source = sampleBytes + frame * frameBytes;
        int16_t* target = &samples_[((written + frame) % capacityFrames_) * Channels];
        target[0] = ReadLittleEndianSample(source);
        // A mono source plays on both sides
        target[1] = channels > 1 ? ReadLittleEndianSample(source + sizeof(int16_t)) : target[0];
    }
    writtenFrames_.store(written + frameCount, std::memory_order_release);
    receivedFrames_.fetch_add(frameCount, std::memory_order_relaxed);
    return true;
}

void HeadsetAudioBuffer::Read(int16_t* out, uint32_t frameCount)
{
    const uint64_t written = writtenFrames_.load(std::memory_order_acquire);
    uint64_t read = readFrames_.load(std::memory_order_relaxed);
    uint32_t available = static_cast<uint32_t>(written - read);
    smoothedLevel_ += (static_cast<float>(available) - smoothedLevel_) * kLevelSmoothing;

    if (!playing_)
    {
        if (capacityFrames_ == 0 || available < targetFrames_ || frameCount == 0)
        {
            std::fill(out, out + static_cast<size_t>(frameCount) * Channels, 0);
            return;
        }
        playing_ = true;
        smoothedLevel_ = static_cast<float>(available);
        framesSinceNudge_ = 0;
    }

    // A burst after a stall would only be heard late, so cut it back to the cushion
    if (available > targetFrames_ + MillisecondsToFrames(settings_.trimAboveTargetMs))
    {
        const uint32_t excess = available - targetFrames_;
        read += excess;
        available -= excess;
        trimmedFrames_.fetch_add(excess, std::memory_order_relaxed);
        smoothedLevel_ = static_cast<float>(available);
    }

    // The Mac's and the headset's audio clocks run at slightly different speeds, which slowly
    // fills or drains the cushion. Skipping a frame, or playing one twice, at most once per
    // interval keeps it level without an audible jump.
    bool repeatFirstFrame = false;
    framesSinceNudge_ += frameCount;
    if (framesSinceNudge_ >= MillisecondsToFrames(settings_.driftIntervalMs) && frameCount > 1)
    {
        const float band = static_cast<float>(MillisecondsToFrames(settings_.driftBandMs));
        if (smoothedLevel_ > static_cast<float>(targetFrames_) + band && available > frameCount)
        {
            read += 1;
            available -= 1;
            framesSinceNudge_ = 0;
            nudgedFrames_.fetch_add(1, std::memory_order_relaxed);
        }
        else if (smoothedLevel_ + band < static_cast<float>(targetFrames_) && available > 0)
        {
            repeatFirstFrame = true;
            framesSinceNudge_ = 0;
            nudgedFrames_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const uint32_t firstOutFrame = repeatFirstFrame ? 1 : 0;
    const uint32_t copied = std::min(frameCount - firstOutFrame, available);
    for (uint32_t frame = 0; frame < copied; ++frame)
    {
        memcpy(out + static_cast<size_t>(firstOutFrame + frame) * Channels,
               &samples_[((read + frame) % capacityFrames_) * Channels],
               Channels * sizeof(int16_t));
    }
    if (repeatFirstFrame)
    {
        out[0] = out[Channels];
        out[1] = out[Channels + 1];
    }
    read += copied;
    readFrames_.store(read, std::memory_order_release);

    const uint32_t filledFrames = copied + firstOutFrame;
    const uint32_t baseTargetFrames = MillisecondsToFrames(settings_.targetMs);
    const uint32_t targetStepFrames = MillisecondsToFrames(settings_.targetStepMs);
    if (filledFrames < frameCount)
    {
        // Ran dry: finish with silence and wait for a bigger cushion before playing again
        std::fill(out + static_cast<size_t>(filledFrames) * Channels,
                  out + static_cast<size_t>(frameCount) * Channels, 0);
        playing_ = false;
        targetFrames_ = std::min(targetFrames_ + targetStepFrames, MillisecondsToFrames(settings_.maxTargetMs));
        framesSinceUnderrun_ = 0;
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        framesSinceUnderrun_ += frameCount;
        if (framesSinceUnderrun_ >= MillisecondsToFrames(settings_.relaxAfterMs) &&
            targetFrames_ > baseTargetFrames)
        {
            targetFrames_ = std::max(targetFrames_ - targetStepFrames, baseTargetFrames);
            framesSinceUnderrun_ = 0;
        }
    }
    reportedTargetFrames_.store(targetFrames_, std::memory_order_relaxed);

    int32_t peak = 0;
    for (size_t index = 0; index < static_cast<size_t>(filledFrames) * Channels; ++index)
    {
        peak = std::max(peak, std::abs(static_cast<int32_t>(out[index])));
    }
    int32_t previousPeak = peak_.load(std::memory_order_relaxed);
    while (peak > previousPeak && !peak_.compare_exchange_weak(previousPeak, peak, std::memory_order_relaxed))
    {
    }
}

HeadsetAudioBuffer::Stats HeadsetAudioBuffer::TakeStats()
{
    Stats stats;
    stats.receivedFrames = receivedFrames_.load(std::memory_order_relaxed);
    stats.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
    stats.trimmedFrames = trimmedFrames_.load(std::memory_order_relaxed);
    stats.nudgedFrames = nudgedFrames_.load(std::memory_order_relaxed);
    stats.underruns = underruns_.load(std::memory_order_relaxed);
    stats.bufferedFrames = static_cast<uint32_t>(writtenFrames_.load(std::memory_order_acquire) -
                                                 readFrames_.load(std::memory_order_acquire));
    stats.targetFrames = reportedTargetFrames_.load(std::memory_order_relaxed);
    stats.peak = static_cast<int16_t>(std::min(peak_.exchange(0, std::memory_order_relaxed), 32767));
    return stats;
}

} // namespace oxr
