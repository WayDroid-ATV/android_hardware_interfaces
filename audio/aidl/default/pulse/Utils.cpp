/*
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

#include <memory>
#include <unordered_map>

#include "Utils.h"

namespace aidl::android::hardware::audio::core::pulse {

using android::media::audio::common::AudioFormatType;
using android::media::audio::common::PcmType;
using OperationPtr = std::unique_ptr<pa_operation, decltype([](pa_operation *op) { if (op) pa_operation_unref(op); })>;

pa_sample_format_t getSampleFormat(const AudioFormatDescription& desc) {
    static const std::unordered_map<PcmType, pa_sample_format_t> sampleFormatMap = {
        { PcmType::UINT_8_BIT, PA_SAMPLE_U8 },
        { PcmType::INT_16_BIT, PA_SAMPLE_S16LE },
        { PcmType::INT_32_BIT, PA_SAMPLE_S32LE },
        { PcmType::FLOAT_32_BIT, PA_SAMPLE_FLOAT32 },
        { PcmType::INT_24_BIT, PA_SAMPLE_S24LE },
    };

    if (desc.type != AudioFormatType::PCM) return PA_SAMPLE_INVALID;

    auto it = sampleFormatMap.find(desc.pcm);
    return (it == sampleFormatMap.end()) ? PA_SAMPLE_INVALID : it->second;
};

}
