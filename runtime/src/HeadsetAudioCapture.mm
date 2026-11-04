// SPDX-License-Identifier: MPL-2.0

#include "HeadsetAudioCapture.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>

#include <unistd.h>

namespace
{

int64_t SteadyClockNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

AudioObjectPropertyAddress GlobalAddress(AudioObjectPropertySelector selector)
{
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}

// Core Audio only knows a process once it has opened audio
AudioObjectID ProcessObjectForPid(pid_t pid)
{
    const AudioObjectPropertyAddress address = GlobalAddress(kAudioHardwarePropertyTranslatePIDToProcessObject);
    AudioObjectID processObject = kAudioObjectUnknown;
    UInt32 size = sizeof(processObject);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(pid), &pid, &size,
                                   &processObject) != noErr)
    {
        return kAudioObjectUnknown;
    }
    return processObject;
}

} // namespace

struct HeadsetAudioCapture::Impl
{
    AudioObjectID tap = kAudioObjectUnknown;
    AudioObjectID device = kAudioObjectUnknown;
    AudioDeviceIOProcID ioProc = nullptr;
    AudioObjectID tappedProcess = kAudioObjectUnknown;
    BlockHandler onBlock;
    uint32_t sampleRateHz = 0;
    bool mutedOnMac = false;
    std::atomic<uint64_t> callbackCount{0};

    void OnInput(const AudioBufferList* input)
    {
        callbackCount.fetch_add(1, std::memory_order_relaxed);
        if (input == nullptr || input->mNumberBuffers == 0 || input->mBuffers[0].mData == nullptr)
        {
            return;
        }

        const AudioBuffer& first = input->mBuffers[0];
        const auto* left = static_cast<const float*>(first.mData);
        const float* right = left;
        uint32_t stride = 1;
        uint32_t frameCount = 0;
        if (input->mNumberBuffers > 1)
        {
            // One buffer per channel
            if (input->mBuffers[1].mData != nullptr)
            {
                right = static_cast<const float*>(input->mBuffers[1].mData);
            }
            frameCount = first.mDataByteSize / sizeof(float);
        }
        else
        {
            stride = std::max<uint32_t>(first.mNumberChannels, 1);
            if (stride > 1)
            {
                right = left + 1;
            }
            frameCount = first.mDataByteSize / (sizeof(float) * stride);
        }
        if (frameCount > 0)
        {
            onBlock(left, right, stride, frameCount, SteadyClockNowNs());
        }
    }

    void Destroy()
    {
        if (ioProc != nullptr)
        {
            AudioDeviceStop(device, ioProc);
            AudioDeviceDestroyIOProcID(device, ioProc);
            ioProc = nullptr;
        }
        if (device != kAudioObjectUnknown)
        {
            AudioHardwareDestroyAggregateDevice(device);
            device = kAudioObjectUnknown;
        }
        if (tap != kAudioObjectUnknown)
        {
            if (@available(macOS 14.2, *))
            {
                AudioHardwareDestroyProcessTap(tap);
            }
            tap = kAudioObjectUnknown;
        }
        tappedProcess = kAudioObjectUnknown;
        sampleRateHz = 0;
        mutedOnMac = false;
    }
};

HeadsetAudioCapture::HeadsetAudioCapture()
    : impl_(std::make_unique<Impl>())
{
}

HeadsetAudioCapture::~HeadsetAudioCapture()
{
    Stop();
}

bool HeadsetAudioCapture::IsSupported()
{
    if (@available(macOS 14.2, *))
    {
        return true;
    }
    return false;
}

bool HeadsetAudioCapture::Start(Source source, bool muteOnMac, BlockHandler onBlock)
{
    Stop();
    if (@available(macOS 14.2, *))
    {
        @autoreleasepool
        {
            Impl& impl = *impl_;
            CATapDescription* description = nil;
            if (source == Source::App)
            {
                impl.tappedProcess = ProcessObjectForPid(getpid());
                if (impl.tappedProcess == kAudioObjectUnknown)
                {
                    return false;
                }
                description = [[CATapDescription alloc] initStereoMixdownOfProcesses:@[ @(impl.tappedProcess) ]];
            }
            else
            {
                description = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
            }
            description.name = @"OXRSys headset audio";
            description.privateTap = YES;
            // A muting tap silences the Mac only while it is read, so the Mac plays again as
            // soon as streaming stops
            description.muteBehavior = muteOnMac ? CATapMutedWhenTapped : CATapUnmuted;
            NSString* tapUid = description.UUID.UUIDString;

            OSStatus status = AudioHardwareCreateProcessTap(description, &impl.tap);
            [description release];
            if (status != noErr)
            {
                impl.tap = kAudioObjectUnknown;
                spdlog::warn("HeadsetAudio: couldn't create the audio tap (Core Audio error {})",
                             static_cast<int>(status));
                Stop();
                return false;
            }

            const AudioObjectPropertyAddress formatAddress = GlobalAddress(kAudioTapPropertyFormat);
            AudioStreamBasicDescription format = {};
            UInt32 formatSize = sizeof(format);
            status = AudioObjectGetPropertyData(impl.tap, &formatAddress, 0, nullptr, &formatSize, &format);
            const bool floatSamples = format.mFormatID == kAudioFormatLinearPCM &&
                                      (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
                                      format.mBitsPerChannel == 32;
            if (status != noErr || !floatSamples || format.mSampleRate < 8000.0 || format.mSampleRate > 192000.0)
            {
                spdlog::warn("HeadsetAudio: the audio tap has an unusable format ({} Hz, {} bit, flags 0x{:x})",
                             format.mSampleRate, format.mBitsPerChannel, format.mFormatFlags);
                Stop();
                return false;
            }
            impl.sampleRateHz = static_cast<uint32_t>(format.mSampleRate + 0.5);

            // A private device that only this process sees, reading nothing but the tap
            NSDictionary* deviceDescription = @{
                @kAudioAggregateDeviceNameKey : @"OXRSys headset audio",
                @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
                @kAudioAggregateDeviceIsPrivateKey : @YES,
                @kAudioAggregateDeviceIsStackedKey : @NO,
                @kAudioAggregateDeviceTapAutoStartKey : @YES,
                @kAudioAggregateDeviceTapListKey : @[ @{
                    @kAudioSubTapUIDKey : tapUid,
                    @kAudioSubTapDriftCompensationKey : @YES,
                } ],
            };
            status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)deviceDescription, &impl.device);
            if (status != noErr)
            {
                impl.device = kAudioObjectUnknown;
                spdlog::warn("HeadsetAudio: couldn't create the device that reads the audio tap (Core Audio error {})",
                             static_cast<int>(status));
                Stop();
                return false;
            }

            impl.onBlock = std::move(onBlock);
            impl.callbackCount.store(0, std::memory_order_relaxed);
            // The block only holds the plain pointer; Stop destroys the IO proc before Impl goes away
            Impl* implPointer = impl_.get();
            status = AudioDeviceCreateIOProcIDWithBlock(
                &impl.ioProc, impl.device, nullptr,
                ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*, AudioBufferList*,
                  const AudioTimeStamp*) {
                    implPointer->OnInput(input);
                });
            if (status == noErr)
            {
                status = AudioDeviceStart(impl.device, impl.ioProc);
            }
            if (status != noErr)
            {
                spdlog::warn("HeadsetAudio: couldn't start reading the audio tap (Core Audio error {})",
                             static_cast<int>(status));
                Stop();
                return false;
            }
            impl.mutedOnMac = muteOnMac;
            return true;
        }
    }
    return false;
}

void HeadsetAudioCapture::Stop()
{
    impl_->Destroy();
    impl_->onBlock = nullptr;
}

bool HeadsetAudioCapture::IsRunning() const
{
    return impl_->ioProc != nullptr;
}

bool HeadsetAudioCapture::IsMutedOnMac() const
{
    return impl_->mutedOnMac;
}

uint32_t HeadsetAudioCapture::GetSampleRateHz() const
{
    return impl_->sampleRateHz;
}

uint64_t HeadsetAudioCapture::GetCallbackCount() const
{
    return impl_->callbackCount.load(std::memory_order_relaxed);
}

bool HeadsetAudioCapture::IsSourcePlaying() const
{
    if (impl_->tappedProcess == kAudioObjectUnknown)
    {
        return true;
    }
    const AudioObjectPropertyAddress address = GlobalAddress(kAudioProcessPropertyIsRunningOutput);
    UInt32 running = 0;
    UInt32 size = sizeof(running);
    if (AudioObjectGetPropertyData(impl_->tappedProcess, &address, 0, nullptr, &size, &running) != noErr)
    {
        return true;
    }
    return running != 0;
}
