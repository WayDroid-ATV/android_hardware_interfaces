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

#include <cmath>
#include <format>
#include <limits>

#define LOG_TAG "AHAL_StreamPulse"
#include <android-base/logging.h>

#include <Utils.h>
#include <audio_utils/clock.h>
#include <error/expected_utils.h>
#include <media/AidlConversionCppNdk.h>

#include "core-impl/StreamPulse.h"

using aidl::android::hardware::audio::common::getChannelCount;

namespace aidl::android::hardware::audio::core {

StreamPulse::StreamPulse(StreamContext* context, const Metadata& metadata)
    : StreamCommonImpl(context, metadata),
      mBufferSizeFrames(getContext().getBufferSizeInFrames()),
      mFrameSizeBytes(getContext().getFrameSize()),
      mIsInput(isInput(metadata)),
      mPACtx(pulse::Context::getContext()),
      mPASampleSpec{
          .format = pulse::getPASampleFormat(getContext().getFormat()),
          .rate = static_cast<uint32_t>(getContext().getSampleRate()),
          .channels = static_cast<uint8_t>(getChannelCount(getContext().getChannelLayout())),
      } {}

StreamPulse::~StreamPulse() {
    cleanupWorker();
}

::android::NBAIO_Format StreamPulse::getPipeFormat() const {
    const audio_format_t audioFormat = VALUE_OR_FATAL(
            aidl2legacy_AudioFormatDescription_audio_format_t(getContext().getFormat()));
    return ::android::Format_from_SR_C(mPASampleSpec.rate, mPASampleSpec.channels, audioFormat);
}

::android::sp<::android::MonoPipe> StreamPulse::makeSink(bool writeCanBlock) {
    const ::android::NBAIO_Format format = getPipeFormat();
    auto sink = ::android::sp<::android::MonoPipe>::make(mBufferSizeFrames, format, writeCanBlock);
    const ::android::NBAIO_Format offers[1] = {format};
    size_t numCounterOffers = 0;
    ssize_t index = sink->negotiate(offers, 1, nullptr, numCounterOffers);
    LOG_IF(FATAL, index != 0) << __func__ << ": Negotiation for the sink failed, index = " << index;
    return sink;
}

::android::sp<::android::MonoPipeReader> StreamPulse::makeSource(::android::MonoPipe* pipe) {
    const ::android::NBAIO_Format format = getPipeFormat();
    const ::android::NBAIO_Format offers[1] = {format};
    auto source = ::android::sp<::android::MonoPipeReader>::make(pipe);
    size_t numCounterOffers = 0;
    ssize_t index = source->negotiate(offers, 1, nullptr, numCounterOffers);
    LOG_IF(FATAL, index != 0) << __func__
                              << ": Negotiation for the source failed, index = " << index;
    return source;
}

::android::status_t StreamPulse::init(DriverCallbackInterface* /*callback*/) {
    ::android::status_t ret = ::android::OK;

    if ((ret = mPACtx->init()) != ::android::OK) {
        return ret;
    }

    mPACtx->withLock([&]() {
        const pa_buffer_attr bufferAttr = {
            .maxlength = static_cast<uint32_t>(mBufferSizeFrames * mFrameSizeBytes * 2),
            .tlength = static_cast<uint32_t>(pa_usec_to_bytes(21000, &mPASampleSpec)),
            .prebuf = static_cast<uint32_t>(-1),
            .minreq = static_cast<uint32_t>(-1),
            .fragsize = static_cast<uint32_t>(mBufferSizeFrames * mFrameSizeBytes),
        };

        constexpr pa_stream_flags_t streamFlags = static_cast<pa_stream_flags_t>(
            PA_STREAM_ADJUST_LATENCY |
            PA_STREAM_AUTO_TIMING_UPDATE |
            PA_STREAM_START_CORKED
        );

        const std::string streamName = std::format("Android {} @ {}Hz",
                                                   mIsInput ? "Record" : "Playback",
                                                   mPASampleSpec.rate);
        mPAStream = pa_stream_new(mPACtx->mCtx.get(), streamName.c_str(), &mPASampleSpec, nullptr);

        if (mPAStream == nullptr) {
            ret = ::android::NO_INIT;
            return;
        }

        if (mIsInput) {
            mRecordSink = makeSink(false);
            mRecordSource = makeSource(mRecordSink.get());
        }

        // Register callbacks
        registerCallbacks();

        // Connect stream
        int connectRet;
        if (mIsInput) {
            connectRet = pa_stream_connect_record(mPAStream, nullptr, &bufferAttr, streamFlags);
        } else {
            connectRet = pa_stream_connect_playback(mPAStream, nullptr, &bufferAttr, streamFlags, nullptr, nullptr);
        }

        // Wait until context is ready
        if (connectRet == 0) {
            while (pa_stream_get_state(mPAStream) == PA_STREAM_CREATING) {
                pa_threaded_mainloop_wait(mPACtx->mMainloop.get());
            }
        }
    });

    if (mPAStream == nullptr || pa_stream_get_state(mPAStream) != PA_STREAM_READY) {
        LOG(ERROR) << __func__ << ": Failed to connect stream to sink: "
                               << pa_strerror(pa_context_errno(mPACtx->mCtx.get()));

        ret = ::android::NO_INIT;
    }

    mPAStreamIndex = pa_stream_get_index(mPAStream);
    LOG(INFO) << __func__ << ": Stream initialized, index = " << mPAStreamIndex << ", mIsInput = " << mIsInput;
    return ret;
}

::android::status_t StreamPulse::drain(StreamDescriptor::DrainMode) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": Stream not initialized";
        return ::android::NO_INIT;
    }

    if (mIsInput) {
        return ::android::OK;
    } else {
        bool success = mPACtx->waitForStreamOperation([&](auto cb, auto p) -> auto {
            return pa_stream_drain(mPAStream, cb, p);
        });

        LOG(INFO) << __func__ << ": Stream drained, success = " << success;
        return success ? ::android::OK : ::android::INVALID_OPERATION;
    }
}

::android::status_t StreamPulse::flush() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": Stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPACtx->waitForStreamOperation([&](auto cb, auto p) -> auto {
        return pa_stream_flush(mPAStream, cb, p);
    });

    LOG(INFO) << __func__ << ": Stream flushed, success = " << success;
    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

::android::status_t StreamPulse::pause() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": Stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPACtx->waitForStreamOperation([&](auto cb, auto p) -> auto {
        return pa_stream_cork(mPAStream, 1, cb, p);
    });

    LOG(INFO) << __func__ << ": Stream paused, success = " << success;
    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

::android::status_t StreamPulse::standby() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": Stream not initialized";
        return ::android::NO_INIT;
    }

    return pause();
}

::android::status_t StreamPulse::start() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": Stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPACtx->waitForStreamOperation([&](auto cb, auto p) -> auto {
        return pa_stream_cork(mPAStream, 0, cb, p);
    });

    LOG(INFO) << __func__ << ": Stream started, success = " << success;
    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

::android::status_t StreamPulse::transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                         int32_t* latencyMs) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": Stream not initialized";
        return ::android::NO_INIT;
    }

    const size_t bytesToTransfer = frameCount * mFrameSizeBytes;
    char* bufferOffset = reinterpret_cast<char*>(buffer);

    if (mIsInput) {
        size_t remainingFrames = frameCount;
        while (remainingFrames > 0) {
            ssize_t framesReadOrError = mRecordSource->read(bufferOffset, remainingFrames);
            if (framesReadOrError < 0) {
                LOG(FATAL) << __func__ << ": Error reading from the pipe: " << framesReadOrError;
                return ::android::INVALID_OPERATION;
            }

            bufferOffset += framesReadOrError * mFrameSizeBytes;
            remainingFrames -= framesReadOrError;

            // MonoPipeReader does not have a blocking read, while use of std::condition_variable
            // requires use of a mutex. For now, just do a 1ms sleep. Consider using a different
            // pipe / ring buffer mechanism.
            if (remainingFrames != 0) usleep(1000);
        }
    } else {
        mPACtx->withLock([&, fname = __func__]() {
            size_t remainingBytes = bytesToTransfer;
            while (remainingBytes > 0) {
                size_t writable;
                while ((writable = pa_stream_writable_size(mPAStream)) == 0) {
                    pa_threaded_mainloop_wait(mPACtx->mMainloop.get());
                }

                size_t bytesToWrite = std::min(writable, remainingBytes);
                if (pa_stream_write(mPAStream, bufferOffset, bytesToWrite, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
                    LOG(ERROR) << fname << ": Failed to write into PulseAudio: " << mPACtx->getLastError();
                    return;
                }

                bufferOffset += bytesToWrite;
                remainingBytes -= bytesToWrite;
            }
        });
    }

    *actualFrameCount = frameCount;
    *latencyMs = std::min(mLatency / PA_USEC_PER_MSEC, static_cast<pa_usec_t>(INT32_MAX));
    return ::android::OK;
}

void StreamPulse::shutdown() {
    if (mPAStream == nullptr) return;

    pa_stream_disconnect(mPAStream);
    pa_stream_unref(mPAStream);
    LOG(INFO) << __func__ << ": Stream shutted down, mIsInput = " << mIsInput;
}

ndk::ScopedAStatus StreamPulse::setGain(float gain) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": Stream not initialized";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    bool success = mPACtx->waitForOperation([&](auto cb, auto p) -> auto {
        pa_cvolume volume = { .channels = static_cast<uint8_t>(mPASampleSpec.channels) };
        pa_cvolume_set(&volume, mPASampleSpec.channels, pa_sw_volume_from_linear(gain));

        return mIsInput ? pa_context_set_source_output_volume(mPACtx->mCtx.get(), mPAStreamIndex, &volume, cb, p)
                        : pa_context_set_sink_input_volume(mPACtx->mCtx.get(), mPAStreamIndex, &volume, cb, p);
    });

    LOG(INFO) << __func__ << ": Set gain = " << gain << ", success = " << success;
    return success ? ndk::ScopedAStatus::ok() : ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
}

static void streamLatencyCallback(pa_stream* s, void* userdata) {
    pa_usec_t latency;
    int negative;

    if (pa_stream_get_latency(s, &latency, &negative) < 0) {
        LOG(ERROR) << __func__ << ": Failed to fetch latency info";
        latency = static_cast<pa_usec_t>(-1);
    } else if (negative == 1 || latency == 0) {
        LOG(VERBOSE) << __func__ << ": Zero or negative latency value reported";
        return;
    }

    reinterpret_cast<std::atomic<pa_usec_t>*>(userdata)->store(latency);
}

static void streamReadCallback(pa_stream* s, size_t nbytes, void* userdata) {
    ::android::MonoPipe* sink = reinterpret_cast<::android::MonoPipe*>(userdata);
    const char* buffer = nullptr;

    if (int ret = pa_stream_peek(s, reinterpret_cast<const void**>(&buffer), &nbytes); ret < 0) {
        LOG(ERROR) << __func__ << ": Error reading from PulseAudio: " << ret;
        return;
    }

    const size_t totalFrames = nbytes / pa_frame_size(pa_stream_get_sample_spec(s));
    ssize_t framesWritten = sink->write(buffer, totalFrames);

    if (framesWritten < 0) {
        LOG(ERROR) << __func__ << ": Error while writing into the pipe: "
                   << framesWritten;
        return;
    } else if (ssize_t framesLost = static_cast<ssize_t>(totalFrames) - framesWritten;
               framesLost > 0) {
        LOG(WARNING) << __func__ << ": Sink has incomplete data sent, dropping "
                     << framesLost << " frames";
    }

    if (int ret = pa_stream_drop(s); ret < 0) {
        LOG(ERROR) << __func__ << ": Error while dropping fragment: " << ret;
        return;
    }
}

void StreamPulse::registerCallbacks() {
    pa_stream_set_latency_update_callback(mPAStream, streamLatencyCallback, &mLatency);
    pa_stream_set_state_callback(mPAStream, [](auto, void* userdata) {
        pa_threaded_mainloop_signal(reinterpret_cast<pa_threaded_mainloop*>(userdata), 0);
    }, mPACtx->mMainloop.get());

    if (mIsInput) {
        pa_stream_set_read_callback(mPAStream, streamReadCallback, mRecordSink.get());
    } else {
        pa_stream_set_write_callback(mPAStream, [](auto, auto, void* userdata) {
            pa_threaded_mainloop_signal(reinterpret_cast<pa_threaded_mainloop*>(userdata), 0);
        }, mPACtx->mMainloop.get());
    }
}

}  // namespace aidl::android::hardware::audio::core
