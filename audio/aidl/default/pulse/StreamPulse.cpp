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
#include <cmath>
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
using aidl::android::media::audio::common::AudioFormatDescription;

namespace aidl::android::hardware::audio::core {

StreamPulse::StreamPulse(StreamContext* context, const Metadata& metadata)
    : StreamCommonImpl(context, metadata),
      mBufferSizeFrames(getContext().getBufferSizeInFrames()),
      mChannelCount(getChannelCount(getContext().getChannelLayout())),
      mFrameSizeBytes(getContext().getFrameSize()),
      mSampleRate(getContext().getSampleRate()),
      mIsInput(isInput(metadata)),
      mPALatency(0),
      mPAContext(pulse::Context::getContext()),
      mPAStream(nullptr, StreamDeleter(mPAContext)) {}

StreamPulse::~StreamPulse() {
    cleanupWorker();
}

static inline ::android::NBAIO_Format getPipeFormat(const AudioFormatDescription& format, int channelCount, int sampleRate) {
    const audio_format_t audioFormat = VALUE_OR_FATAL(aidl2legacy_AudioFormatDescription_audio_format_t(format));
    return ::android::Format_from_SR_C(sampleRate, channelCount, audioFormat);
}

void StreamPulse::initMonoPipe(bool writeCanBlock) {
    const auto format = getPipeFormat(getContext().getFormat(), mChannelCount, mSampleRate);

    mSinkIo = ::android::sp<::android::MonoPipe>::make(mBufferSizeFrames, format, writeCanBlock);
    mSourceIo = ::android::sp<::android::MonoPipeReader>::make(mSinkIo.get());

    {
        size_t numCounterOffers = 0;
        ssize_t index = mSinkIo->negotiate(&format, 1, nullptr, numCounterOffers);
        LOG_IF(FATAL, index != 0) << __func__ << ": Negotiation for sink failed, index = " << index;
    }
    {
        size_t numCounterOffers = 0;
        ssize_t index = mSourceIo->negotiate(&format, 1, nullptr, numCounterOffers);
        LOG_IF(FATAL, index != 0) << __func__ << ": Negotiation for source failed, index = " << index;
    }
}

::android::status_t StreamPulse::init(DriverCallbackInterface* /*callback*/) {
    std::promise<pa_stream_state_t> promise;

    mPAContext->init();
    if (mPAContext->mCtx == nullptr) return ::android::NO_INIT;

    mPAContext->withLock([&]() {
        int ret;

        pa_sample_spec sampleSpec = {
            .format = pulse::getSampleFormat(getContext().getFormat()),
            .rate = static_cast<uint32_t>(getContext().getSampleRate()),
            .channels = static_cast<uint8_t>(mChannelCount),
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
            pa_buffer_attr bufAttr = {
                .maxlength = UINT32_MAX,
                .tlength = static_cast<uint32_t>(mBufferSizeFrames * 1.3 * mFrameSizeBytes),
                .prebuf = UINT32_MAX,
                .minreq = static_cast<uint32_t>(mBufferSizeFrames * 0.5 * mFrameSizeBytes),
            };

            pa_stream_flags_t flags = static_cast<pa_stream_flags_t>(

                PA_STREAM_AUTO_TIMING_UPDATE
            );

            ret = pa_stream_connect_playback(mPAStream.get(), nullptr, nullptr, flags, nullptr, nullptr);
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

        pa_stream_set_latency_update_callback(mPAStream.get(), callbackFn, &mPALatency);
        callbackFn(mPAStream.get(), &mPALatency);
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

    teardownIo();
    return pause();
}

::android::status_t StreamPulse::start() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    if (flush() != ::android::OK) {
        LOG(WARNING) << __func__ << ": failed to flush stream";
    }

    mPAContext->withLock([&]() {
        mPAWritableSize = pa_stream_writable_size(mPAStream.get());
    });

    LOG(INFO) << "PA Size: " << mPAWritableSize << " HAL Size: " << mFrameSizeBytes * mBufferSizeFrames;

    mPAContext->withLock([&]() {
        struct CallbackArgs {
            std::atomic<bool>& readyForWrite;
            std::atomic<size_t>& writableSize;
            size_t bufferSizeFrames;
            size_t frameSizeBytes;
        } *cbArgs = new CallbackArgs{mPAReadyForWrite, mPAWritableSize, mBufferSizeFrames, mFrameSizeBytes};

        pa_stream_set_write_callback(mPAStream.get(), [](pa_stream*, size_t writable, void* userdata) {
            CallbackArgs* cbArgs = reinterpret_cast<CallbackArgs*>(userdata);
            //LOG(WARNING) << __func__ << ": writable size update: " << writable;
            cbArgs->writableSize = writable;

            cbArgs->readyForWrite = true;
            cbArgs->readyForWrite.notify_all();
        }, cbArgs);
    });

    if (mSinkIo == nullptr || mSourceIo == nullptr) {
        initMonoPipe(mIsInput);
        mIoThreadIsRunning = true;
        mIoThreads.emplace_back(mIsInput ? &StreamPulse::inputIoThread : &StreamPulse::outputIoThread, this);
    }

    bool success = mPAContext->waitForOperation([&](std::promise<bool>& promise) -> pa_operation* {
        return pa_stream_cork(mPAStream.get(), 0, [](pa_stream*, int success, void* userdata) {
            reinterpret_cast<std::promise<bool>*>(userdata)->set_value(static_cast<bool>(success));
        }, &promise);
    });

    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

/*::android::status_t StreamPulse::transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                         int32_t* latencyMs) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    if (mIsInput) {
        LOG(VERBOSE) << __func__ << ": reading from sink";
        ssize_t framesRead = mSourceIo->read(buffer, frameCount);
        LOG_IF(FATAL, framesRead < 0) << "Error reading from the pipe: " << framesRead;
        if (ssize_t framesMissing = static_cast<ssize_t>(frameCount) - framesRead; framesMissing > 0) {
            LOG(WARNING) << __func__ << ": incomplete data received, inserting " << framesMissing
                         << " frames of silence";
            memset(static_cast<char*>(buffer) + framesRead * mFrameSizeBytes, 0,
                   framesMissing * mFrameSizeBytes);
        }
        //maxLatency = proxy_get_latency(mAlsaDeviceProxies[i].get());
    } else {
        LOG(VERBOSE) << __func__ << ": writing into sink";
        ssize_t framesWritten = mSinkIo->write(buffer, frameCount);
        LOG_IF(FATAL, framesWritten < 0) << "Error writing into the pipe: " << framesWritten;
        if (ssize_t framesLost = static_cast<ssize_t>(frameCount) - framesWritten; framesLost > 0) {
            LOG(WARNING) << __func__ << ": sink has incomplete data sent, dropping "
                         << framesLost << " frames";
        }
        //maxLatency = std::max(maxLatency, proxy_get_latency(mAlsaDeviceProxies[i].get()));
    }
    *actualFrameCount = frameCount;
    //maxLatency = std::min(0, static_cast<unsigned>(std::numeric_limits<int32_t>::max()));
    *latencyMs = 0;
    return ::android::OK;
}
    */

::android::status_t StreamPulse::transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                          int32_t* latencyMs) {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    /*mPAContext->withLock([&]() {
        const pa_timing_info* info = pa_stream_get_timing_info(mPAStream.get());
        pa_usec_t latencyUs = 0;
        int negative = 0;
        const int latencyResult =
                pa_stream_get_latency(mPAStream.get(), &latencyUs, &negative);

        LOG(INFO) << "state=" << pa_stream_get_state(mPAStream.get())
                  << " corked=" << pa_stream_is_corked(mPAStream.get())
                  << " writable=" << pa_stream_writable_size(mPAStream.get())
                  << " latencyUs=" << latencyUs
                  << " negative=" << negative
                << " writeIndex=" << (info ? info->write_index : -1)
              << " readIndex=" << (info ? info->read_index : -1)
              << " sinkUsec=" << (info ? info->sink_usec : -1)
              << " transportUsec=" << (info ? info->transport_usec : -1)
              << " latencyResult=" << latencyResult;
    });*/

    const size_t bytesToTransfer = frameCount * mFrameSizeBytes;
    int ret;

    std::promise<void> promise;

    if (mIsInput) {
        // wip
        ret = 1;
    } else {
        size_t pendingBytes = bytesToTransfer;

        while (pendingBytes > 0) {
            mPAReadyForWrite.wait(false);

            size_t bytes = std::min(mPAWritableSize.load(), pendingBytes);
            //LOG(WARNING) << __func__ << ": writable size: " << mPAWritableSize << " remaining: " << pendingBytes;

            mPAContext->withLock([&]() {
                ret = pa_stream_write(mPAStream.get(), buffer, bytes, nullptr, 0, PA_SEEK_RELATIVE);
                pendingBytes -= bytes;
                mPAReadyForWrite = false;
            });
        }
    }

    if (ret == 0) {
        *actualFrameCount = frameCount;
        *latencyMs = 20;//mLatency.load();
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

    if (const pa_timing_info* timingInfo; (timingInfo = pa_stream_get_timing_info(mPAStream.get()))) {
        mPAContext->withLock([&]() {
            // Timestamp provided by PulseAudio is not monotonic, so not to use it here
            position->timeNs = ::android::uptimeNanos();
            position->frames = (mIsInput ? timingInfo->write_index : timingInfo->read_index) / mFrameSizeBytes;
        });
        return ::android::OK;
    } else {
        return ::android::INVALID_OPERATION;
    }
}

void StreamPulse::shutdown() {
    teardownIo();
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

void StreamPulse::inputIoThread() {
#if defined(__ANDROID__)
    setWorkerThreadPriority(pthread_gettid_np(pthread_self()));
    const std::string threadName = "in";
    pthread_setname_np(pthread_self(), threadName.c_str());
#endif
    mPAContext->withLock([&]() {
        struct CallbackArgs {
            StreamPulse* instance;
            decltype(mSinkIo) io;
        } cbArgs{this, mSinkIo};

        pa_stream_set_read_callback(mPAStream.get(), [](pa_stream*, size_t bytesCanRead, void* userdata) {
            CallbackArgs* cbArgs = reinterpret_cast<CallbackArgs*>(userdata);
            const char *buffer;

            int ret = pa_stream_peek(
                cbArgs->instance->mPAStream.get(),
                reinterpret_cast<const void**>(&buffer),
                &bytesCanRead
            );

            if (ret != 0) {
                LOG(WARNING) << __func__ << ": Error writing into PulseAudio: " << ret;
                return;
            }

            ssize_t framesWrittenOrError = cbArgs->io->write(buffer, bytesCanRead);
            LOG_IF(WARNING, framesWrittenOrError < 0) << __func__
                                                      << ": Error while writing into the pipe: "
                                                      << framesWrittenOrError;
        }, &cbArgs);
    });
}

void StreamPulse::outputIoThread() {
#if defined(__ANDROID__)
    setWorkerThreadPriority(pthread_gettid_np(pthread_self()));
    const std::string threadName = "out";
    pthread_setname_np(pthread_self(), threadName.c_str());
#endif

    std::vector<char> buffer(mBufferSizeFrames * mFrameSizeBytes);

    while (false) {
        ssize_t framesReadOrError = mSourceIo->read(buffer.data(), mPAWritableSize / mFrameSizeBytes);

        if (
            framesReadOrError == 0 || mPAWritableSize == 0 ||
            (mPAWritableSize / mFrameSizeBytes) < static_cast<size_t>(framesReadOrError)
        ) {
            // MonoPipeReader does not have a blocking read, while use of std::condition_variable
            // requires use of a mutex. For now, just do a 1ms sleep. Consider using a different
            // pipe / ring buffer mechanism.
            if (mIoThreadIsRunning) usleep(1000);
        } else if (framesReadOrError > 0) {
            mPAContext->withLock([&]() {
                int ret = pa_stream_write(
                    mPAStream.get(),
                    buffer.data(),
                    framesReadOrError * mFrameSizeBytes,
                    nullptr,
                    0,
                    PA_SEEK_RELATIVE
                );

                LOG_IF(WARNING, ret != 0) << __func__ << ": Error writing into PulseAudio: " << ret;
            });
        } else {
            LOG(WARNING) << __func__ << ": Error while reading from the pipe: " << framesReadOrError;
        }
    }
}

void StreamPulse::teardownIo() {
    mIoThreadIsRunning = false;
    mPAContext->withLock([&]() {
        if (mIsInput) {
            LOG(DEBUG) << __func__ << ": shutting down pipes";
            mSinkIo->shutdown(true);
            pa_stream_set_read_callback(mPAStream.get(), nullptr, nullptr);
        } else {
            pa_stream_set_write_callback(mPAStream.get(), nullptr, nullptr);
        }
    });

    LOG(DEBUG) << __func__ << ": joining threads";
    for (auto& thread : mIoThreads) {
        if (thread.joinable()) thread.join();
    }
    mSinkIo.clear();
    mSourceIo.clear();
    mIoThreads.clear();
}

}  // namespace aidl::android::hardware::audio::core
