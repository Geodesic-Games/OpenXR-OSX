// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "HeadsetAudioCapture.h"

#import <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace
{

constexpr double kToneAmplitude = 0.003; // About -50 dBFS, too quiet to hear

// Plays a quiet 440 Hz tone from this process, so the tap has something to hear
class TonePlayer
{
public:
    ~TonePlayer()
    {
        if (queue_ != nullptr)
        {
            AudioQueueStop(queue_, true);
            AudioQueueDispose(queue_, true);
        }
    }

    bool Start()
    {
        AudioStreamBasicDescription format = {};
        format.mSampleRate = 48000;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = sizeof(float) * 2;
        format.mBytesPerPacket = format.mBytesPerFrame;
        if (AudioQueueNewOutput(&format, &TonePlayer::Fill, this, nullptr, nullptr, 0, &queue_) != noErr)
        {
            queue_ = nullptr;
            return false;
        }
        for (int bufferIndex = 0; bufferIndex < 3; ++bufferIndex)
        {
            AudioQueueBufferRef buffer = nullptr;
            if (AudioQueueAllocateBuffer(queue_, 480 * format.mBytesPerFrame, &buffer) != noErr)
            {
                return false;
            }
            Fill(this, queue_, buffer);
        }
        return AudioQueueStart(queue_, nullptr) == noErr;
    }

private:
    static void Fill(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer)
    {
        auto* player = static_cast<TonePlayer*>(userData);
        auto* samples = static_cast<float*>(buffer->mAudioData);
        const uint32_t frameCount = buffer->mAudioDataBytesCapacity / (sizeof(float) * 2);
        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            const float value = static_cast<float>(kToneAmplitude * std::sin(player->phase_));
            player->phase_ += 2.0 * M_PI * 440.0 / 48000.0;
            samples[frame * 2] = value;
            samples[frame * 2 + 1] = value;
        }
        buffer->mAudioDataByteSize = frameCount * sizeof(float) * 2;
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }

    AudioQueueRef queue_ = nullptr;
    double phase_ = 0.0;
};

} // namespace

TEST_CASE("Headset audio capture taps this process's own sound", "[headset-audio][core-audio]")
{
    if (!HeadsetAudioCapture::IsSupported())
    {
        SKIP("process taps need macOS 14.2");
    }
    TonePlayer tone;
    if (!tone.Start())
    {
        SKIP("this Mac has no audio output to play the test tone on");
    }

    std::atomic<uint64_t> frames{0};
    std::atomic<float> peak{0.0f};
    auto onBlock = [&](const float* left, const float* right, uint32_t stride, uint32_t frameCount, int64_t) {
        float blockPeak = 0.0f;
        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            blockPeak = std::max({blockPeak, std::fabs(left[frame * stride]), std::fabs(right[frame * stride])});
        }
        if (blockPeak > peak.load())
        {
            peak.store(blockPeak);
        }
        frames.fetch_add(frameCount);
    };

    // Core Audio only knows this process once the tone has started playing. The tap doesn't
    // mute, so the test never changes what this Mac plays.
    HeadsetAudioCapture capture;
    bool started = false;
    for (int attempt = 0; attempt < 20 && !started; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        started = capture.Start(HeadsetAudioCapture::Source::App, false, onBlock);
    }
    REQUIRE(started);
    CHECK(capture.IsRunning());
    CHECK_FALSE(capture.IsMutedOnMac());
    const uint32_t sampleRateHz = capture.GetSampleRateHz();
    CHECK(sampleRateHz >= 8000);
    CHECK(capture.IsSourcePlaying());

    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint64_t callbacks = capture.GetCallbackCount();
    capture.Stop();
    CHECK_FALSE(capture.IsRunning());
    CHECK(callbacks >= 20);
    CHECK(frames.load() >= sampleRateHz / 2);

    // macOS hands the tap silence unless whatever launched the tests may record system audio,
    // so the tone itself is only checked when sound came through
    if (peak.load() > 0.0f)
    {
        CHECK(peak.load() > kToneAmplitude * 0.5);
        CHECK(peak.load() < kToneAmplitude * 1.5);
    }
    else
    {
        WARN("The tap heard silence; allow the terminal running the tests to record system audio to check the sound");
    }

    // Stopping twice is harmless, and the capture can start again
    capture.Stop();
    REQUIRE(capture.Start(HeadsetAudioCapture::Source::App, false, onBlock));
    capture.Stop();
}

TEST_CASE("Headset audio capture taps every app's sound", "[headset-audio][core-audio]")
{
    if (!HeadsetAudioCapture::IsSupported())
    {
        SKIP("process taps need macOS 14.2");
    }
    TonePlayer tone;
    if (!tone.Start())
    {
        SKIP("this Mac has no audio output to play the test tone on");
    }

    std::atomic<uint64_t> frames{0};
    std::atomic<float> peak{0.0f};
    auto onBlock = [&](const float* left, const float* right, uint32_t stride, uint32_t frameCount, int64_t) {
        float blockPeak = 0.0f;
        for (uint32_t frame = 0; frame < frameCount; ++frame)
        {
            blockPeak = std::max({blockPeak, std::fabs(left[frame * stride]), std::fabs(right[frame * stride])});
        }
        if (blockPeak > peak.load())
        {
            peak.store(blockPeak);
        }
        frames.fetch_add(frameCount);
    };

    // Other apps may be playing too, so only a floor is checked for the sound
    HeadsetAudioCapture capture;
    REQUIRE(capture.Start(HeadsetAudioCapture::Source::System, false, onBlock));
    CHECK(capture.IsSourcePlaying());
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint64_t callbacks = capture.GetCallbackCount();
    capture.Stop();
    CHECK(callbacks >= 20);
    CHECK(frames.load() >= 24000);
    if (peak.load() > 0.0f)
    {
        CHECK(peak.load() > kToneAmplitude * 0.5);
    }
    else
    {
        WARN("The tap heard silence; allow the terminal running the tests to record system audio to check the sound");
    }
}
