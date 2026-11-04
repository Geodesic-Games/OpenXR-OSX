// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace oxr
{

// Stereo 16-bit audio between the thread that receives it and the audio output callback. The
// receiving thread writes and the callback reads, and neither locks or allocates. The callback
// plays from a small cushion of buffered audio that absorbs network jitter: it waits for the
// cushion before it starts, builds a bigger one after running dry, and drops old audio when
// the cushion grows, so what the headset plays stays close to live.
class HeadsetAudioBuffer
{
public:
    static constexpr uint32_t Channels = 2;

    struct Settings
    {
        uint32_t capacityMs = 500;
        uint32_t targetMs = 40;          // cushion played from
        uint32_t maxTargetMs = 120;      // the cushion grows to at most this after running dry
        uint32_t targetStepMs = 10;      // and by this much each time
        uint32_t trimAboveTargetMs = 60; // a burst this far past the cushion is cut back to it
        uint32_t driftBandMs = 5;        // the cushion may wander this far before it is nudged back
        uint32_t driftIntervalMs = 10;   // at most one frame nudged per interval
        uint32_t relaxAfterMs = 20000;   // the cushion shrinks a step after this long without running dry
    };

    struct Stats
    {
        uint64_t receivedFrames = 0;
        uint64_t droppedFrames = 0;  // arrived while the buffer was full
        uint64_t trimmedFrames = 0;  // played late enough to be cut
        uint64_t nudgedFrames = 0;   // repeated or skipped to follow the Mac's clock
        uint32_t underruns = 0;
        uint32_t bufferedFrames = 0;
        uint32_t targetFrames = 0;
        int16_t peak = 0;            // loudest sample played since the last TakeStats
    };

    HeadsetAudioBuffer() = default;
    explicit HeadsetAudioBuffer(const Settings& settings);

    // Only while neither side is running
    void Reset(uint32_t sampleRateHz);

    // Receiving thread. Appends interleaved little-endian 16-bit samples, mono or stereo;
    // keeps nothing and returns false when they don't fit.
    bool Write(const uint8_t* sampleBytes, uint32_t frameCount, uint32_t channels);

    // Output callback. Always fills frameCount stereo frames, with silence while there is
    // nothing to play.
    void Read(int16_t* out, uint32_t frameCount);

    // Counters since Reset; the peak starts over with every call
    Stats TakeStats();

    uint32_t MillisecondsToFrames(uint32_t milliseconds) const;

private:
    Settings settings_;
    std::vector<int16_t> samples_;
    uint32_t sampleRateHz_ = 0;
    uint32_t capacityFrames_ = 0;
    std::atomic<uint64_t> writtenFrames_{0};
    std::atomic<uint64_t> readFrames_{0};

    // Owned by the output callback
    bool playing_ = false;
    float smoothedLevel_ = 0.0f;
    uint32_t targetFrames_ = 0;
    uint32_t framesSinceNudge_ = 0;
    uint32_t framesSinceUnderrun_ = 0;

    std::atomic<uint64_t> receivedFrames_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<uint64_t> trimmedFrames_{0};
    std::atomic<uint64_t> nudgedFrames_{0};
    std::atomic<uint32_t> underruns_{0};
    std::atomic<uint32_t> reportedTargetFrames_{0};
    std::atomic<int32_t> peak_{0};
};

} // namespace oxr
