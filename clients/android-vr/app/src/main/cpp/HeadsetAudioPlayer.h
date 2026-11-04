// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "HeadsetAudioBuffer.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace oxr
{

// Plays the Mac's sound in the headset through a low-latency AAudio stream. Records arrive on
// the network thread, and the stream's callback plays them from a jitter cushion.
class HeadsetAudioPlayer
{
public:
    HeadsetAudioPlayer() = default;
    ~HeadsetAudioPlayer();

    HeadsetAudioPlayer(const HeadsetAudioPlayer&) = delete;
    HeadsetAudioPlayer& operator=(const HeadsetAudioPlayer&) = delete;

    // Network thread. Queues one record's interleaved 16-bit samples, opening the output on
    // first use and again when the rate changes or Android closed it.
    void Play(uint32_t sampleRateHz, uint32_t channels, const uint8_t* sampleBytes, uint32_t frameCount);
    void Stop();

private:
    bool OpenStream(uint32_t sampleRateHz);
    void CloseStream();
    void LogStats(std::chrono::steady_clock::time_point now);

    static aaudio_data_callback_result_t OnAudioNeeded(AAudioStream* stream, void* userData, void* audioData,
                                                       int32_t frameCount);
    static void OnStreamError(AAudioStream* stream, void* userData, aaudio_result_t error);

    std::mutex mutex_;
    AAudioStream* stream_ = nullptr;
    uint32_t streamSampleRateHz_ = 0;
    std::atomic_bool streamLost_{false};
    std::chrono::steady_clock::time_point nextOpenTime_ = {};
    std::chrono::steady_clock::time_point lastStatsTime_ = {};
    HeadsetAudioBuffer buffer_;
};

} // namespace oxr
