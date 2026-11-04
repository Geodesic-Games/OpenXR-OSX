// SPDX-License-Identifier: MPL-2.0

#pragma once

// Headset speaker audio records, shared by the runtime that sends them and the clients that
// play them. Each TCP Audio record on the USB control connection holds a TcpAudioHeader and
// then interleaved little-endian 16-bit PCM.

#include <oxrsys/protocol/Protocol.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace oxr
{
namespace protocol
{

constexpr uint32_t HEADSET_AUDIO_MAX_RECORD_FRAMES = 4800; // 100 ms at 48 kHz
constexpr uint32_t HEADSET_AUDIO_MIN_SAMPLE_RATE_HZ = 8000;
constexpr uint32_t HEADSET_AUDIO_MAX_SAMPLE_RATE_HZ = 192000;
constexpr uint16_t HEADSET_AUDIO_MAX_CHANNELS = 2;

inline size_t HeadsetAudioRecordPayloadSize(uint32_t frameCount, uint16_t channels)
{
    return sizeof(TcpAudioHeader) + static_cast<size_t>(frameCount) * channels * sizeof(int16_t);
}

// Fills `out` with one record payload for frameCount interleaved frames and returns its size.
// `out` must hold HeadsetAudioRecordPayloadSize(frameCount, channels) bytes.
inline size_t WriteHeadsetAudioRecord(uint8_t* out, int64_t presentationTimeNs, uint32_t sampleRateHz,
                                      uint16_t channels, const int16_t* samples, uint32_t frameCount)
{
    TcpAudioHeader header = {};
    header.presentationTimeNs = presentationTimeNs;
    header.frameCount = frameCount;
    header.payloadSize = frameCount * channels * static_cast<uint32_t>(sizeof(int16_t));
    header.sampleRateHz = sampleRateHz;
    header.channels = channels;
    header.format = static_cast<uint16_t>(AudioSampleFormat::Int16);
    memcpy(out, &header, sizeof(header));
    memcpy(out + sizeof(header), samples, header.payloadSize);
    return sizeof(header) + header.payloadSize;
}

// Checks a record payload and points at its sample bytes. False for anything a client can't
// play, including sample formats added later, so those records are skipped rather than
// played as noise.
inline bool ReadHeadsetAudioRecord(const uint8_t* payload, size_t payloadSize, TcpAudioHeader& header,
                                   const uint8_t*& sampleBytes)
{
    if (payload == nullptr || payloadSize < sizeof(TcpAudioHeader))
    {
        return false;
    }
    memcpy(&header, payload, sizeof(header));
    if (header.format != static_cast<uint16_t>(AudioSampleFormat::Int16) ||
        header.channels == 0 || header.channels > HEADSET_AUDIO_MAX_CHANNELS ||
        header.sampleRateHz < HEADSET_AUDIO_MIN_SAMPLE_RATE_HZ ||
        header.sampleRateHz > HEADSET_AUDIO_MAX_SAMPLE_RATE_HZ ||
        header.frameCount == 0 || header.frameCount > HEADSET_AUDIO_MAX_RECORD_FRAMES)
    {
        return false;
    }
    const size_t sampleSize = static_cast<size_t>(header.frameCount) * header.channels * sizeof(int16_t);
    if (header.payloadSize != sampleSize || payloadSize < sizeof(header) + sampleSize)
    {
        return false;
    }
    sampleBytes = payload + sizeof(header);
    return true;
}

} // namespace protocol
} // namespace oxr
