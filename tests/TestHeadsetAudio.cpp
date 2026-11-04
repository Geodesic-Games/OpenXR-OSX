// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "HeadsetAudioBuffer.h"
#include "HeadsetAudioRing.h"

#include <oxrsys/protocol/HeadsetAudio.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using oxr::HeadsetAudioBuffer;
using oxrsys::HeadsetAudioRing;
using namespace oxr::protocol;

namespace
{

// Interleaved stereo bytes where frame n holds n on the left and -n on the right
std::vector<uint8_t> CountingFrames(int16_t first, uint32_t frameCount)
{
    std::vector<int16_t> samples;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        samples.push_back(static_cast<int16_t>(first + frame));
        samples.push_back(static_cast<int16_t>(-(first + frame)));
    }
    std::vector<uint8_t> bytes(samples.size() * sizeof(int16_t));
    memcpy(bytes.data(), samples.data(), bytes.size());
    return bytes;
}

// At 1000 Hz a frame is a millisecond, which keeps the cushion arithmetic readable
constexpr uint32_t kMillisecondRate = 1000;

struct BufferFeeder
{
    HeadsetAudioBuffer& buffer;
    int16_t next = 1;

    bool Write(uint32_t frameCount)
    {
        const std::vector<uint8_t> bytes = CountingFrames(next, frameCount);
        next = static_cast<int16_t>(next + frameCount);
        return buffer.Write(bytes.data(), frameCount, 2);
    }
};

std::vector<int16_t> ReadFrames(HeadsetAudioBuffer& buffer, uint32_t frameCount)
{
    std::vector<int16_t> out(static_cast<size_t>(frameCount) * 2, 1234);
    buffer.Read(out.data(), frameCount);
    return out;
}

bool IsSilent(const std::vector<int16_t>& samples)
{
    for (const int16_t sample : samples)
    {
        if (sample != 0)
        {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("Headset audio records round-trip and reject what a client can't play", "[headset-audio]")
{
    const int16_t samples[] = {1, -1, 2, -2, 3, -3};
    std::vector<uint8_t> payload(HeadsetAudioRecordPayloadSize(3, 2));
    REQUIRE(payload.size() == sizeof(TcpAudioHeader) + 12);
    REQUIRE(WriteHeadsetAudioRecord(payload.data(), 123456789, 48000, 2, samples, 3) == payload.size());

    TcpAudioHeader header = {};
    const uint8_t* sampleBytes = nullptr;
    REQUIRE(ReadHeadsetAudioRecord(payload.data(), payload.size(), header, sampleBytes));
    CHECK(header.presentationTimeNs == 123456789);
    CHECK(header.frameCount == 3);
    CHECK(header.payloadSize == 12);
    CHECK(header.sampleRateHz == 48000);
    CHECK(header.channels == 2);
    CHECK(header.format == static_cast<uint16_t>(AudioSampleFormat::Int16));
    CHECK(memcmp(sampleBytes, samples, sizeof(samples)) == 0);

    auto rejects = [&](auto change) {
        std::vector<uint8_t> broken = payload;
        TcpAudioHeader brokenHeader = {};
        memcpy(&brokenHeader, broken.data(), sizeof(brokenHeader));
        change(brokenHeader, broken);
        memcpy(broken.data(), &brokenHeader, sizeof(brokenHeader));
        TcpAudioHeader parsed = {};
        const uint8_t* parsedSamples = nullptr;
        return !ReadHeadsetAudioRecord(broken.data(), broken.size(), parsed, parsedSamples);
    };
    CHECK(rejects([](TcpAudioHeader& value, std::vector<uint8_t>&) {
        value.format = static_cast<uint16_t>(AudioSampleFormat::Float32);
    }));
    CHECK(rejects([](TcpAudioHeader& value, std::vector<uint8_t>&) { value.channels = 0; }));
    CHECK(rejects([](TcpAudioHeader& value, std::vector<uint8_t>&) { value.channels = 3; }));
    CHECK(rejects([](TcpAudioHeader& value, std::vector<uint8_t>&) { value.frameCount = 0; }));
    CHECK(rejects([](TcpAudioHeader& value, std::vector<uint8_t>&) {
        value.frameCount = HEADSET_AUDIO_MAX_RECORD_FRAMES + 1;
    }));
    CHECK(rejects([](TcpAudioHeader& value, std::vector<uint8_t>&) { value.payloadSize = 10; }));
    CHECK(rejects([](TcpAudioHeader& value, std::vector<uint8_t>&) { value.sampleRateHz = 1000; }));
    CHECK(rejects([](TcpAudioHeader&, std::vector<uint8_t>& bytes) { bytes.pop_back(); }));

    TcpAudioHeader parsed = {};
    const uint8_t* parsedSamples = nullptr;
    CHECK_FALSE(ReadHeadsetAudioRecord(payload.data(), sizeof(TcpAudioHeader) - 1, parsed, parsedSamples));
    CHECK_FALSE(ReadHeadsetAudioRecord(nullptr, payload.size(), parsed, parsedSamples));
}

TEST_CASE("Headset audio ring converts float samples to 16-bit stereo", "[headset-audio]")
{
    HeadsetAudioRing ring(16);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float interleaved[] = {0.5f, -0.5f, 2.0f, -2.0f, nan, 1.0f};
    REQUIRE(ring.WriteFloat(interleaved, interleaved + 1, 2, 3, 77));

    const float left[] = {0.25f, -1.0f};
    const float right[] = {-0.25f, 1.0f};
    REQUIRE(ring.WriteFloat(left, right, 1, 2, 88));

    const float mono[] = {0.1f};
    REQUIRE(ring.WriteFloat(mono, mono, 1, 1, 99));

    CHECK(ring.GetAvailableFrames() == 6);
    CHECK(ring.GetLastWriteEndTimeNs() == 99);

    std::vector<int16_t> out(12);
    REQUIRE(ring.Read(out.data(), 6, 16) == 6);
    const std::vector<int16_t> expected = {
        HeadsetAudioRing::ToInt16(0.5f), HeadsetAudioRing::ToInt16(-0.5f),
        32767, -32767,
        0, 32767,
        HeadsetAudioRing::ToInt16(0.25f), HeadsetAudioRing::ToInt16(-0.25f),
        -32767, 32767,
        HeadsetAudioRing::ToInt16(0.1f), HeadsetAudioRing::ToInt16(0.1f),
    };
    CHECK(out == expected);
    CHECK(HeadsetAudioRing::ToInt16(0.5f) == 16384);
    CHECK(ring.GetAvailableFrames() == 0);
}

TEST_CASE("Headset audio ring wraps around and keeps nothing once full", "[headset-audio]")
{
    HeadsetAudioRing ring(8);
    std::vector<float> block;
    for (int frame = 0; frame < 6; ++frame)
    {
        block.push_back(frame / 100.0f);
        block.push_back(-frame / 100.0f);
    }

    std::vector<int16_t> out(16);
    for (int round = 0; round < 3; ++round)
    {
        REQUIRE(ring.WriteFloat(block.data(), block.data() + 1, 2, 6, round));
        REQUIRE(ring.Read(out.data(), 8, 8) == 6);
        CHECK(out[10] == HeadsetAudioRing::ToInt16(0.05f));
        CHECK(out[11] == HeadsetAudioRing::ToInt16(-0.05f));
    }

    REQUIRE(ring.WriteFloat(block.data(), block.data() + 1, 2, 6, 0));
    CHECK_FALSE(ring.WriteFloat(block.data(), block.data() + 1, 2, 3, 0));
    CHECK(ring.GetDroppedFrames() == 3);
    CHECK(ring.GetAvailableFrames() == 6);

    ring.Reset();
    CHECK(ring.GetAvailableFrames() == 0);
    CHECK(ring.GetDroppedFrames() == 0);
}

TEST_CASE("Headset audio ring reader skips a backlog to the newest frames", "[headset-audio]")
{
    HeadsetAudioRing ring(100);
    std::vector<float> block;
    for (int frame = 0; frame < 50; ++frame)
    {
        block.push_back(frame / 1000.0f);
        block.push_back(0.0f);
    }
    REQUIRE(ring.WriteFloat(block.data(), block.data() + 1, 2, 50, 0));

    std::vector<int16_t> out(200);
    REQUIRE(ring.Read(out.data(), 100, 20) == 20);
    CHECK(ring.GetSkippedFrames() == 30);
    CHECK(out[0] == HeadsetAudioRing::ToInt16(0.030f));
    CHECK(out[38] == HeadsetAudioRing::ToInt16(0.049f));
}

TEST_CASE("Headset audio buffer waits for its cushion and then plays in order", "[headset-audio]")
{
    HeadsetAudioBuffer buffer;
    buffer.Reset(kMillisecondRate);
    BufferFeeder feeder{buffer};

    REQUIRE(feeder.Write(39));
    CHECK(IsSilent(ReadFrames(buffer, 10)));

    REQUIRE(feeder.Write(1));
    const std::vector<int16_t> played = ReadFrames(buffer, 10);
    for (int frame = 0; frame < 10; ++frame)
    {
        CHECK(played[frame * 2] == frame + 1);
        CHECK(played[frame * 2 + 1] == -(frame + 1));
    }
    CHECK(buffer.TakeStats().bufferedFrames == 30);
}

TEST_CASE("Headset audio buffer finishes with silence when it runs dry and builds a bigger cushion",
          "[headset-audio]")
{
    HeadsetAudioBuffer buffer;
    buffer.Reset(kMillisecondRate);
    BufferFeeder feeder{buffer};

    REQUIRE(feeder.Write(45));
    ReadFrames(buffer, 40);
    const std::vector<int16_t> dry = ReadFrames(buffer, 10);
    CHECK(dry[8] == 45);
    CHECK(IsSilent(std::vector<int16_t>(dry.begin() + 10, dry.end())));

    HeadsetAudioBuffer::Stats stats = buffer.TakeStats();
    CHECK(stats.underruns == 1);
    CHECK(stats.targetFrames == 50);

    // 45 frames are no longer enough to start again
    REQUIRE(feeder.Write(45));
    CHECK(IsSilent(ReadFrames(buffer, 10)));
    REQUIRE(feeder.Write(5));
    CHECK(ReadFrames(buffer, 10)[0] == 46);
}

TEST_CASE("Headset audio buffer cuts a late burst back to its cushion", "[headset-audio]")
{
    HeadsetAudioBuffer buffer;
    buffer.Reset(kMillisecondRate);
    BufferFeeder feeder{buffer};

    REQUIRE(feeder.Write(40));
    ReadFrames(buffer, 1);
    // A stall delivers 100 more frames at once: 139 buffered, more than 60 past the 40 cushion
    REQUIRE(feeder.Write(100));
    const std::vector<int16_t> played = ReadFrames(buffer, 10);
    // Only the newest 40 frames are left, starting at frame 101
    CHECK(played[0] == 101);
    const HeadsetAudioBuffer::Stats stats = buffer.TakeStats();
    CHECK(stats.trimmedFrames == 99);
    CHECK(stats.bufferedFrames == 30);
}

TEST_CASE("Headset audio buffer follows a sender clock that runs fast or slow", "[headset-audio]")
{
    SECTION("fast: one frame is skipped now and then")
    {
        HeadsetAudioBuffer buffer;
        buffer.Reset(kMillisecondRate);
        BufferFeeder feeder{buffer};
        REQUIRE(feeder.Write(40));
        for (int step = 0; step < 600; ++step)
        {
            // The sender delivers 11 frames for every 10 played
            REQUIRE(feeder.Write(step % 10 == 0 ? 11 : 10));
            ReadFrames(buffer, 10);
        }
        const HeadsetAudioBuffer::Stats stats = buffer.TakeStats();
        CHECK(stats.underruns == 0);
        CHECK(stats.trimmedFrames == 0);
        CHECK(stats.nudgedFrames >= 50);
        CHECK(stats.bufferedFrames <= 50);
    }

    SECTION("slow: one frame is played twice now and then")
    {
        HeadsetAudioBuffer buffer;
        buffer.Reset(kMillisecondRate);
        BufferFeeder feeder{buffer};
        REQUIRE(feeder.Write(40));
        for (int step = 0; step < 600; ++step)
        {
            // The sender delivers 9 frames for every 10 played
            REQUIRE(feeder.Write(step % 10 == 0 ? 9 : 10));
            ReadFrames(buffer, 10);
        }
        const HeadsetAudioBuffer::Stats stats = buffer.TakeStats();
        CHECK(stats.underruns == 0);
        CHECK(stats.nudgedFrames >= 50);
        CHECK(stats.bufferedFrames >= 25);
    }
}

TEST_CASE("Headset audio buffer shrinks its cushion again after a long steady stretch", "[headset-audio]")
{
    HeadsetAudioBuffer::Settings settings;
    settings.relaxAfterMs = 200;
    HeadsetAudioBuffer buffer(settings);
    buffer.Reset(kMillisecondRate);
    BufferFeeder feeder{buffer};

    REQUIRE(feeder.Write(40));
    ReadFrames(buffer, 50);
    REQUIRE(buffer.TakeStats().targetFrames == 50);

    REQUIRE(feeder.Write(50));
    for (int step = 0; step < 30; ++step)
    {
        REQUIRE(feeder.Write(10));
        ReadFrames(buffer, 10);
    }
    const HeadsetAudioBuffer::Stats stats = buffer.TakeStats();
    CHECK(stats.targetFrames == 40);
    CHECK(stats.underruns == 1);
}

TEST_CASE("Headset audio buffer plays mono on both sides, drops what doesn't fit and reports its peak",
          "[headset-audio]")
{
    HeadsetAudioBuffer buffer;
    buffer.Reset(kMillisecondRate);

    std::vector<int16_t> mono(40);
    for (int16_t frame = 0; frame < 40; ++frame)
    {
        mono[frame] = static_cast<int16_t>(frame * 100);
    }
    REQUIRE(buffer.Write(reinterpret_cast<const uint8_t*>(mono.data()), 40, 1));
    const std::vector<int16_t> played = ReadFrames(buffer, 5);
    CHECK(played[8] == 400);
    CHECK(played[9] == 400);
    CHECK(buffer.TakeStats().peak == 400);
    CHECK(buffer.TakeStats().peak == 0);

    // 500 ms of room, 35 frames already waiting
    BufferFeeder feeder{buffer};
    REQUIRE(feeder.Write(465));
    CHECK_FALSE(feeder.Write(1));
    CHECK(buffer.TakeStats().droppedFrames == 1);
}
