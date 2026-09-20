/*
 * Copyright (C) 2023 The Android Open Source Project
 * Copyright (C) 2026 The WayDroid-ATV Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <vector>

#include <android-base/thread_annotations.h>

#include "StreamPulse.h"

namespace aidl::android::hardware::audio::core {

class StreamPrimaryPulse : public StreamPulse {
  public:
    StreamPrimaryPulse(StreamContext* context, const Metadata& metadata);

    // Methods of 'DriverInterface'.
    ::android::status_t flush() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;
    ::android::status_t refinePosition(StreamDescriptor::Position* position) override;

    // Overridden methods of 'StreamCommonImpl', called on a Binder thread.
    ndk::ScopedAStatus setConnectedDevices(const ConnectedDevices& devices) override;

  protected:
    const bool mIsAsynchronous;
    int64_t mStartTimeNs = 0;
    long mFramesSinceStart = 0;
};

class StreamInPrimaryPulse final : public StreamIn,
                                   public StreamPrimaryPulse,
                                   public StreamInHwGainHelper {
  public:
    friend class ndk::SharedRefBase;
    StreamInPrimaryPulse(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

class StreamOutPrimaryPulse final : public StreamOut,
                                    public StreamPrimaryPulse,
                                    public StreamOutHwVolumeHelper {
  public:
    friend class ndk::SharedRefBase;
    StreamOutPrimaryPulse(StreamContext&& context,
                          const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
                          const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>& offloadInfo);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }

    ndk::ScopedAStatus setHwVolume(const std::vector<float>& in_channelVolumes) override;
};

}  // namespace aidl::android::hardware::audio::core
