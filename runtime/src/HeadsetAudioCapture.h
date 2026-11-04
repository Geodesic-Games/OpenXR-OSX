// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

// Records what this Mac plays, for the headset's speakers, with a Core Audio process tap
// (macOS 14.2 or later): either this process's own sound or every app's. With muteOnMac, the
// tapped sound stops coming out of the Mac while the tap is being read. Blocks arrive on Core
// Audio's IO thread about every 10 ms, as float samples at the tap's own rate, usually 48 kHz.
//
// macOS only fills the tap when the app responsible for this process may record system
// audio (System Settings > Privacy & Security > Screen & System Audio Recording). Without
// that it delivers silence and reports no error, yet a muting tap still silences the Mac, so
// mute only once the tap has delivered real sound.
class HeadsetAudioCapture
{
public:
    enum class Source
    {
        App,    // only this process
        System, // every process on the Mac
    };

    // left and right point at the first frame's two channels, and stride is the distance in
    // floats from one frame to the next. Runs on Core Audio's IO thread, so it must not lock,
    // allocate or block.
    using BlockHandler = std::function<void(const float* left, const float* right, uint32_t stride,
                                            uint32_t frameCount, int64_t captureTimeNs)>;

    HeadsetAudioCapture();
    ~HeadsetAudioCapture();

    HeadsetAudioCapture(const HeadsetAudioCapture&) = delete;
    HeadsetAudioCapture& operator=(const HeadsetAudioCapture&) = delete;

    // Process taps need macOS 14.2
    static bool IsSupported();

    // False when the tap or the device that reads it can't be created; App also fails until
    // this process has opened audio once, so callers retry
    bool Start(Source source, bool muteOnMac, BlockHandler onBlock);
    void Stop();
    bool IsRunning() const;
    bool IsMutedOnMac() const;

    uint32_t GetSampleRateHz() const;
    // Grows with every IO callback; it stops growing if the device behind the tap dies
    uint64_t GetCallbackCount() const;
    // Whether the tapped process is playing to an output right now; always true for System
    bool IsSourcePlaying() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
