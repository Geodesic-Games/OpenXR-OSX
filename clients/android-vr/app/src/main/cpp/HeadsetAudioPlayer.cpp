// SPDX-License-Identifier: MPL-2.0

#include "HeadsetAudioPlayer.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>

#define LOG_TAG "OXRSys-Audio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace oxr
{

namespace
{

constexpr auto kReopenDelay = std::chrono::seconds(1);
constexpr auto kStatsInterval = std::chrono::seconds(10);

} // namespace

HeadsetAudioPlayer::~HeadsetAudioPlayer()
{
    Stop();
}

void HeadsetAudioPlayer::Play(uint32_t sampleRateHz, uint32_t channels, const uint8_t* sampleBytes,
                              uint32_t frameCount)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ != nullptr && (streamLost_.load() || sampleRateHz != streamSampleRateHz_))
    {
        CloseStream();
    }

    const auto now = std::chrono::steady_clock::now();
    if (stream_ == nullptr)
    {
        if (now < nextOpenTime_)
        {
            return;
        }
        if (!OpenStream(sampleRateHz))
        {
            nextOpenTime_ = now + kReopenDelay;
            return;
        }
        lastStatsTime_ = now;
    }

    buffer_.Write(sampleBytes, frameCount, channels);
    if (now - lastStatsTime_ >= kStatsInterval)
    {
        LogStats(now);
    }
}

void HeadsetAudioPlayer::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    CloseStream();
}

bool HeadsetAudioPlayer::OpenStream(uint32_t sampleRateHz)
{
    // No callback runs while the stream is closed, so the cushion can start over
    buffer_.Reset(sampleRateHz);
    streamLost_.store(false);

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK)
    {
        LOGE("Headset audio: couldn't create an AAudio builder (%s)", AAudio_convertResultToText(result));
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, HeadsetAudioBuffer::Channels);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(sampleRateHz));
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(builder, &HeadsetAudioPlayer::OnAudioNeeded, this);
    AAudioStreamBuilder_setErrorCallback(builder, &HeadsetAudioPlayer::OnStreamError, this);
    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK)
    {
        LOGE("Headset audio: couldn't open a %u Hz output (%s)", sampleRateHz, AAudio_convertResultToText(result));
        stream_ = nullptr;
        return false;
    }

    // Playing at any other rate or layout would change the pitch or garble the sound
    if (AAudioStream_getSampleRate(stream_) != static_cast<int32_t>(sampleRateHz) ||
        AAudioStream_getChannelCount(stream_) != static_cast<int32_t>(HeadsetAudioBuffer::Channels) ||
        AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_I16)
    {
        LOGE("Headset audio: the output opened as %d Hz, %d channels, format %d instead of %u Hz stereo 16-bit",
             AAudioStream_getSampleRate(stream_), AAudioStream_getChannelCount(stream_),
             AAudioStream_getFormat(stream_), sampleRateHz);
        CloseStream();
        return false;
    }

    // Two bursts is the least the output can hold without glitching
    const int32_t burstFrames = AAudioStream_getFramesPerBurst(stream_);
    if (burstFrames > 0)
    {
        AAudioStream_setBufferSizeInFrames(stream_, burstFrames * 2);
    }

    result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK)
    {
        LOGE("Headset audio: couldn't start the output (%s)", AAudio_convertResultToText(result));
        CloseStream();
        return false;
    }

    streamSampleRateHz_ = sampleRateHz;
    LOGI("Headset audio: playing %u Hz stereo, burst %d frames, output buffer %d frames, %s, %s",
         sampleRateHz, burstFrames, AAudioStream_getBufferSizeInFrames(stream_),
         AAudioStream_getPerformanceMode(stream_) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY ? "low latency"
                                                                                      : "normal latency",
         AAudioStream_getSharingMode(stream_) == AAUDIO_SHARING_MODE_EXCLUSIVE ? "exclusive" : "shared");
    return true;
}

void HeadsetAudioPlayer::CloseStream()
{
    if (stream_ == nullptr)
    {
        return;
    }
    AAudioStream_requestStop(stream_);
    // Waits for the callback to finish, so the cushion is ours again afterwards
    AAudioStream_close(stream_);
    stream_ = nullptr;
    streamSampleRateHz_ = 0;
}

void HeadsetAudioPlayer::LogStats(std::chrono::steady_clock::time_point now)
{
    const HeadsetAudioBuffer::Stats stats = buffer_.TakeStats();
    const uint32_t framesPerMs = std::max(buffer_.MillisecondsToFrames(1), 1u);
    const double peakDb = stats.peak > 0 ? 20.0 * std::log10(stats.peak / 32767.0) : -120.0;
    LOGI("Headset audio: %.1f s received, cushion %u ms of %u ms, %u underruns, %llu ms cut as late, "
         "%llu ms dropped, %llu frames nudged, peak %.0f dBFS",
         static_cast<double>(stats.receivedFrames) / (framesPerMs * 1000.0),
         stats.bufferedFrames / framesPerMs, stats.targetFrames / framesPerMs, stats.underruns,
         static_cast<unsigned long long>(stats.trimmedFrames / framesPerMs),
         static_cast<unsigned long long>(stats.droppedFrames / framesPerMs),
         static_cast<unsigned long long>(stats.nudgedFrames), peakDb);
    lastStatsTime_ = now;
}

aaudio_data_callback_result_t HeadsetAudioPlayer::OnAudioNeeded(AAudioStream* /*stream*/, void* userData,
                                                                void* audioData, int32_t frameCount)
{
    auto* player = static_cast<HeadsetAudioPlayer*>(userData);
    player->buffer_.Read(static_cast<int16_t*>(audioData), static_cast<uint32_t>(std::max(frameCount, 0)));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void HeadsetAudioPlayer::OnStreamError(AAudioStream* /*stream*/, void* userData, aaudio_result_t error)
{
    // Android closes the output when the audio route changes, for example when headphones are
    // plugged in. A stream can't be reopened from its own callback, so the next record does it.
    LOGI("Headset audio: the output stopped (%s); reopening it", AAudio_convertResultToText(error));
    static_cast<HeadsetAudioPlayer*>(userData)->streamLost_.store(true);
}

} // namespace oxr
