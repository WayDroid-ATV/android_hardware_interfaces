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

#define LOG_TAG "AHAL_StreamPulsePrimary"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <audio_utils/clock.h>
#include <error/Result.h>
#include <error/expected_utils.h>

#include "core-impl/StreamPrimaryPulse.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioDeviceAddress;
using aidl::android::media::audio::common::AudioDeviceDescription;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::MicrophoneInfo;
using android::base::GetBoolProperty;

namespace aidl::android::hardware::audio::core {

StreamPrimaryPulse::StreamPrimaryPulse(StreamContext* context, const Metadata& metadata)
    : StreamPulse(context, metadata) {
    context->startStreamDataProcessor();
}

::android::status_t StreamPrimaryPulse::flush() {
    RETURN_STATUS_IF_ERROR(StreamPulse::flush());
    return mIsInput ? standby() : ::android::OK;
}

StreamInPrimaryPulse::StreamInPrimaryPulse(StreamContext&& context, const SinkMetadata& sinkMetadata,
                                           const std::vector<MicrophoneInfo>& microphones)
    : StreamIn(std::move(context), microphones),
      StreamPrimaryPulse(&mContextInstance, sinkMetadata) {}

StreamOutPrimaryPulse::StreamOutPrimaryPulse(StreamContext&& context, const SourceMetadata& sourceMetadata,
                                             const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamPrimaryPulse(&mContextInstance, sourceMetadata) {}

}  // namespace aidl::android::hardware::audio::core
