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

#include <algorithm>
#include <future>
#include <limits>

#define LOG_TAG "AHAL_StreamPulse"
#include <android-base/logging.h>

#include <Utils.h>
#include <audio_utils/clock.h>
#include <error/expected_utils.h>
#include <media/AidlConversionCppNdk.h>

#include "Utils.h"
#include "core-impl/StreamPulse.h"

using aidl::android::hardware::audio::common::getChannelCount;

namespace aidl::android::hardware::audio::core {

StreamPulse::StreamPulse(StreamContext* context, const Metadata& metadata)
    : StreamCommonImpl(context, metadata),
      mChannelCount(getChannelCount(getContext().getChannelLayout())),
      mFrameSizeBytes(getContext().getFrameSize()),
      mIsInput(isInput(metadata)),
      mPAContext(pulse::Context::getContext()),
      mLatency(0),
      mPAStream(nullptr, StreamDeleter(mPAContext)) {}

StreamPulse::~StreamPulse() {
    cleanupWorker();
}

::android::status_t StreamPulse::init(DriverCallbackInterface* /*callback*/) {
    std::promise<pa_stream_state_t> promise;

    mPAContext->init();
    if (mPAContext->mCtx == nullptr) return ::android::NO_INIT;

    mPAContext->withLock([&]() {
        int ret;

        pa_sample_spec sampleSpec = {
            .channels = static_cast<uint8_t>(mChannelCount),
            .format = pulse::getSampleFormat(getContext().getFormat()),
            .rate = static_cast<uint32_t>(getContext().getSampleRate()),
        };

        mPAStream.reset(pa_stream_new(mPAContext->mCtx.get(), "Waydroid", &sampleSpec, nullptr));

        if (mPAStream == nullptr) {
            promise.set_value(PA_STREAM_FAILED);
            return;
        }

        pa_stream_set_state_callback(mPAStream.get(), [](pa_stream* s, void* promise) {
            pa_stream_state_t state = pa_stream_get_state(s);

            // Only handle ready/fail status
            if (state == PA_STREAM_READY || !PA_STREAM_IS_GOOD(state)) {
                pa_stream_set_state_callback(s, nullptr, nullptr);
                reinterpret_cast<std::promise<pa_stream_state_t>*>(promise)->set_value(state);
            }
        }, &promise);

        if (mIsInput) {
            ret = pa_stream_connect_record(
                mPAStream.get(),
                nullptr,
                nullptr,
                PA_STREAM_AUTO_TIMING_UPDATE
            );
        } else {
            ret = pa_stream_connect_playback(
                mPAStream.get(),
                nullptr,
                nullptr,
                PA_STREAM_AUTO_TIMING_UPDATE,
                nullptr,
                nullptr
            );
        }

        if (ret < 0) {
            pa_stream_set_state_callback(mPAStream.get(), nullptr, nullptr);
            promise.set_value(PA_STREAM_FAILED);
            return;
        }
    });

    if (!PA_STREAM_IS_GOOD(promise.get_future().get())) {
        LOG(ERROR) << __func__ << ": failed to connect stream to sink: "
                               << pa_strerror(pa_context_errno(mPAContext->mCtx.get()));

        return ::android::NO_INIT;
    }

    mPAContext->withLock([&]() {
        void (*callbackFn)(pa_stream*, void*) = [](pa_stream* s, void* userdata) {
            pa_usec_t unsignedLatency;
            int isNegative;

            if (pa_stream_get_latency(s, &unsignedLatency, &isNegative) == 0) {
                unsignedLatency = std::min(static_cast<int32_t>(unsignedLatency / PA_USEC_PER_MSEC), INT32_MAX);
                *reinterpret_cast<std::atomic<int32_t>*>(userdata) = isNegative ? -static_cast<int>(unsignedLatency)
                                                                                : +static_cast<int>(unsignedLatency);
            }
        };

        pa_stream_set_latency_update_callback(mPAStream.get(), callbackFn, &mLatency);
        callbackFn(mPAStream.get(), &mLatency);
    });

    return ::android::OK;
}

::android::status_t StreamPulse::drain(StreamDescriptor::DrainMode) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    if (!mIsInput) {
        bool success = mPAContext->waitForOperation([&](std::promise<bool>& promise) -> pa_operation* {
            return pa_stream_drain(mPAStream.get(), [](pa_stream*, int success, void* userdata) {
                reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
            }, &promise);
        });

        return success ? ::android::OK : ::android::INVALID_OPERATION;
    }
    return ::android::OK;
}

::android::status_t StreamPulse::flush() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPAContext->waitForOperation([&](std::promise<bool>& promise) -> pa_operation* {
        return pa_stream_flush(mPAStream.get(), [](pa_stream*, int success, void* userdata) {
            reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
        }, &promise);
    });

    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

::android::status_t StreamPulse::pause() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPAContext->waitForOperation([&](std::promise<bool>& promise) -> pa_operation* {
        return pa_stream_cork(mPAStream.get(), 1, [](pa_stream*, int success, void* userdata) {
            reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
        }, &promise);
    });

    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

::android::status_t StreamPulse::standby() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    ::android::status_t status = flush();
    return (status == ::android::OK) ? pause() : status;
}

::android::status_t StreamPulse::start() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPAContext->waitForOperation([&](std::promise<bool>& promise) -> pa_operation* {
        return pa_stream_cork(mPAStream.get(), 0, [](pa_stream*, int success, void* userdata) {
            reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
        }, &promise);
    });

    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

::android::status_t StreamPulse::transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                          int32_t* latencyMs) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    const size_t bytesToTransfer = frameCount * mFrameSizeBytes;
    int ret;

    mPAContext->withLock([&]() {
        if (mIsInput) {
            // wip
            ret = 1;
        } else {
            ret = pa_stream_write(mPAStream.get(), buffer, bytesToTransfer, nullptr, 0, PA_SEEK_RELATIVE);
        }
    });

    if (ret == 0) {
        *actualFrameCount = frameCount;
        *latencyMs = mLatency.load();
        return ::android::OK;
    } else {
        *actualFrameCount = 0;
        *latencyMs = 0;
        return ::android::INVALID_OPERATION;
    }
}

::android::status_t StreamPulse::refinePosition(StreamDescriptor::Position* position) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPAContext->waitForOperation([&](std::promise<bool>& promise) -> pa_operation* {
        return pa_stream_update_timing_info(mPAStream.get(), [](pa_stream*, int success, void* userdata) {
            reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
        }, &promise);
    });

    const pa_timing_info* timingInfo = pa_stream_get_timing_info(mPAStream.get());
    if (success && timingInfo) {
        mPAContext->withLock([&]() {
            // Timestamp provided by PulseAudio is not monotonic, so not to use it here
            position->timeNs = pa_rtclock_now() * PA_NSEC_PER_USEC;
            position->frames = mIsInput ? timingInfo->write_index : timingInfo->read_index;
            position->frames /= mFrameSizeBytes;
        });
        return ::android::OK;
    } else {
        return ::android::INVALID_OPERATION;
    }
}

void StreamPulse::shutdown() {
    mPAStream.reset(nullptr);
}

ndk::ScopedAStatus StreamPulse::setGain(float gain) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    bool success = mPAContext->waitForOperation([&](std::promise<bool>& promise) -> pa_operation* {
        pa_cvolume volume;
        pa_cvolume_set(&volume, mChannelCount, static_cast<pa_volume_t>(PA_VOLUME_NORM * gain));

        if (mIsInput) {
            return pa_context_set_source_output_volume(
                mPAContext->mCtx.get(),
                pa_stream_get_index(mPAStream.get()),
                &volume,
                [](pa_context*, int success, void* userdata) {
                    reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
                },
                &promise
            );
        } else {
            return pa_context_set_sink_input_volume(
                mPAContext->mCtx.get(),
                pa_stream_get_index(mPAStream.get()),
                &volume,
                [](pa_context*, int success, void* userdata) {
                    reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
                },
                &promise
            );
        }
    });

    return success ? ndk::ScopedAStatus::ok() : ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

}  // namespace aidl::android::hardware::audio::core
