/*
 * Copyright (C) 2023 The Android Open Source Project
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
#include <limits>

#define LOG_TAG "AHAL_StreamAlsa"
#include <android-base/logging.h>

#include <Utils.h>
#include <audio_utils/clock.h>
#include <error/expected_utils.h>
#include <media/AidlConversionCppNdk.h>

#include "core-impl/StreamAlsa.h"

#include "pulse/Utils.h"

using aidl::android::hardware::audio::common::getChannelCount;

namespace aidl::android::hardware::audio::core {

StreamAlsa::StreamAlsa(StreamContext* context, const Metadata& metadata, int readWriteRetries)
    : StreamCommonImpl(context, metadata),
      mBufferSizeFrames(getContext().getBufferSizeInFrames()),
      mFrameSizeBytes(getContext().getFrameSize()),
      mSampleRate(getContext().getSampleRate()),
      mIsInput(isInput(metadata)),
      mConfig(alsa::getPcmConfig(getContext(), mIsInput)),
      mReadWriteRetries(readWriteRetries),
      mPACtx(pulse::Context::getContext()),
      mPAStream(nullptr, StreamDeleter(mPACtx)) {}

StreamAlsa::~StreamAlsa() {
    cleanupWorker();
}

::android::NBAIO_Format StreamAlsa::getPipeFormat() const {
    const audio_format_t audioFormat = VALUE_OR_FATAL(
            aidl2legacy_AudioFormatDescription_audio_format_t(getContext().getFormat()));
    const int channelCount = getChannelCount(getContext().getChannelLayout());
    return ::android::Format_from_SR_C(getContext().getSampleRate(), channelCount, audioFormat);
}

::android::sp<::android::MonoPipe> StreamAlsa::makeSink(bool writeCanBlock) {
    const ::android::NBAIO_Format format = getPipeFormat();
    auto sink = ::android::sp<::android::MonoPipe>::make(mBufferSizeFrames, format, writeCanBlock);
    const ::android::NBAIO_Format offers[1] = {format};
    size_t numCounterOffers = 0;
    ssize_t index = sink->negotiate(offers, 1, nullptr, numCounterOffers);
    LOG_IF(FATAL, index != 0) << __func__ << ": Negotiation for the sink failed, index = " << index;
    return sink;
}

::android::sp<::android::MonoPipeReader> StreamAlsa::makeSource(::android::MonoPipe* pipe) {
    const ::android::NBAIO_Format format = getPipeFormat();
    const ::android::NBAIO_Format offers[1] = {format};
    auto source = ::android::sp<::android::MonoPipeReader>::make(pipe);
    size_t numCounterOffers = 0;
    ssize_t index = source->negotiate(offers, 1, nullptr, numCounterOffers);
    LOG_IF(FATAL, index != 0) << __func__
                              << ": Negotiation for the source failed, index = " << index;
    return source;
}

::android::status_t StreamAlsa::init(DriverCallbackInterface* /*callback*/) {
    ::android::status_t ret = ::android::OK;

    if ((ret = mPACtx->init()) != ::android::OK) {
        return ret;
    }

    mPACtx->withLock([&]() {
        const pa_sample_spec sampleSpec = {
            .format = pulse::getPASampleFormat(getContext().getFormat()),
            .rate = static_cast<uint32_t>(getContext().getSampleRate()),
            .channels = static_cast<uint8_t>(getChannelCount(getContext().getChannelLayout())),
        };

        const pa_buffer_attr bufAttr = {
            .maxlength = static_cast<uint32_t>(-1),
            .tlength = static_cast<uint32_t>(mBufferSizeFrames * mFrameSizeBytes),
            .prebuf = static_cast<uint32_t>(mFrameSizeBytes),
            .minreq = static_cast<uint32_t>(-1),
            .fragsize = static_cast<uint32_t>(-1)
        };

        pa_stream_flags_t streamFlags = static_cast<pa_stream_flags_t>(
            PA_STREAM_ADJUST_LATENCY |
            PA_STREAM_AUTO_TIMING_UPDATE |
            PA_STREAM_START_CORKED |
            PA_STREAM_INTERPOLATE_TIMING
        );

        mPAStream.reset(pa_stream_new(mPACtx->mCtx.get(), "Waydroid", &sampleSpec, nullptr));
        if (mPAStream == nullptr) {
            ret = ::android::NO_INIT;
            return;
        }

        // Register state callback
        pa_stream_set_state_callback(mPAStream.get(), [](auto, void* userdata) {
            pa_threaded_mainloop_signal(reinterpret_cast<pa_threaded_mainloop*>(userdata), 0);
        }, mPACtx->mMainloop.get());

        // Connect stream
        int connectRet;
        if (mIsInput) {
            connectRet = pa_stream_connect_record(mPAStream.get(), nullptr, &bufAttr, streamFlags);
        } else {
            connectRet = pa_stream_connect_playback(mPAStream.get(), nullptr, &bufAttr, streamFlags, nullptr, nullptr);
        }

        // Wait until context is ready
        if (connectRet == 0) {
            while (pa_stream_get_state(mPAStream.get()) == PA_STREAM_CREATING) {
                pa_threaded_mainloop_wait(mPACtx->mMainloop.get());
            }
        }
    });

    if (mPAStream == nullptr || pa_stream_get_state(mPAStream.get()) != PA_STREAM_READY) {
        LOG(ERROR) << __func__ << ": failed to connect stream to sink: "
                               << pa_strerror(pa_context_errno(mPACtx->mCtx.get()));

        ret = ::android::NO_INIT;
    }

    return ret;
}

::android::status_t StreamAlsa::drain(StreamDescriptor::DrainMode) {
    if (!mIsInput) {
        static constexpr float kMicrosPerSecond = MICROS_PER_SECOND;
        const size_t delayUs = static_cast<size_t>(
                std::roundf(mBufferSizeFrames * kMicrosPerSecond / mSampleRate));
        usleep(delayUs);
    }
    return ::android::OK;
}

::android::status_t StreamAlsa::flush() {
    return ::android::OK;
}

::android::status_t StreamAlsa::pause() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    bool success = mPACtx->waitForOperation([&](auto cb, auto p) -> auto {
        LOG(INFO) << __func__ << ": PulseAudio stream paused";
        return pa_stream_cork(mPAStream.get(), 1, cb, p);
    });

    return success ? ::android::OK : ::android::INVALID_OPERATION;
}

::android::status_t StreamAlsa::standby() {
    teardownIo();
    return ::android::OK;
}

::android::status_t StreamAlsa::start() {
    if (mPAStream == nullptr) {
        LOG(ERROR) << __func__ << ": PulseAudio stream not initialized";
        return ::android::NO_INIT;
    }

    mPACtx->waitForOperation([&](auto cb, auto p) -> auto {
        return pa_stream_cork(mPAStream.get(), 0, cb, p);
    });

    LOG(INFO) << __func__ << ": PulseAudio stream started";

    if (!mAlsaDeviceProxies.empty()) {
        // This is a resume after a pause.
        return ::android::OK;
    }
    decltype(mAlsaDeviceProxies) alsaDeviceProxies;
    decltype(mSources) sources;
    decltype(mSinks) sinks;
    for (const auto& device : getDeviceProfiles()) {
        if ((device.direction == PCM_OUT && mIsInput) ||
            (device.direction == PCM_IN && !mIsInput)) {
            continue;
        }
        alsa::DeviceProxy proxy;
        if (device.isExternal) {
            // Always ask alsa configure as required since the configuration should be supported
            // by the connected device. That is guaranteed by `setAudioPortConfig` and
            // `setAudioPatch`.
            proxy = alsa::openProxyForExternalDevice(
                    device, const_cast<struct pcm_config*>(&mConfig.value()),
                    true /*require_exact_match*/);
        } else {
            proxy = alsa::openProxyForAttachedDevice(
                    device, const_cast<struct pcm_config*>(&mConfig.value()), mBufferSizeFrames);
        }
        if (proxy.get() == nullptr) {
            return ::android::NO_INIT;
        }
        alsaDeviceProxies.push_back(std::move(proxy));
        auto sink = makeSink(mIsInput);  // Do not block the writer when it is on our thread.
        if (sink != nullptr) {
            sinks.push_back(sink);
        } else {
            return ::android::NO_INIT;
        }
        if (auto source = makeSource(sink.get()); source != nullptr) {
            sources.push_back(source);
        } else {
            return ::android::NO_INIT;
        }
    }
    if (alsaDeviceProxies.empty()) {
        return ::android::NO_INIT;
    }
    mAlsaDeviceProxies = std::move(alsaDeviceProxies);
    mSources = std::move(sources);
    mSinks = std::move(sinks);
    mIoThreadIsRunning = true;
    for (size_t i = 0; i < mAlsaDeviceProxies.size(); ++i) {
        mIoThreads.emplace_back(mIsInput ? &StreamAlsa::inputIoThread : &StreamAlsa::outputIoThread,
                                this, i);
    }
    return ::android::OK;
}

::android::status_t StreamAlsa::transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                         int32_t* latencyMs) {
    if (mAlsaDeviceProxies.empty()) {
        LOG(FATAL) << __func__ << ": no opened devices";
        return ::android::NO_INIT;
    }
    const size_t bytesToTransfer = frameCount * mFrameSizeBytes;
    unsigned maxLatency = 0;
    if (mIsInput) {
        const size_t i = 0;  // For the input case, only support a single device.
        LOG(VERBOSE) << __func__ << ": reading from sink " << i;
        ssize_t framesRead = mSources[i]->read(buffer, frameCount);
        LOG_IF(FATAL, framesRead < 0) << "Error reading from the pipe: " << framesRead;
        if (ssize_t framesMissing = static_cast<ssize_t>(frameCount) - framesRead;
            framesMissing > 0) {
            LOG(WARNING) << __func__ << ": incomplete data received, inserting " << framesMissing
                         << " frames of silence";
            memset(static_cast<char*>(buffer) + framesRead * mFrameSizeBytes, 0,
                   framesMissing * mFrameSizeBytes);
        }
        maxLatency = proxy_get_latency(mAlsaDeviceProxies[i].get());
    } else {
        alsa::applyGain(buffer, mGain, bytesToTransfer, mConfig.value().format, mConfig->channels);
        for (size_t i = 0; i < mAlsaDeviceProxies.size(); ++i) {
            LOG(VERBOSE) << __func__ << ": writing into sink " << i;
            ssize_t framesWritten = mSinks[i]->write(buffer, frameCount);
            LOG_IF(FATAL, framesWritten < 0) << "Error writing into the pipe: " << framesWritten;
            if (ssize_t framesLost = static_cast<ssize_t>(frameCount) - framesWritten;
                framesLost > 0) {
                LOG(WARNING) << __func__ << ": sink " << i << " incomplete data sent, dropping "
                             << framesLost << " frames";
            }
        }

        maxLatency = std::max(maxLatency, getPALatency());
    }
    *actualFrameCount = frameCount;
    maxLatency = std::min(maxLatency, static_cast<unsigned>(std::numeric_limits<int32_t>::max()));
    *latencyMs = maxLatency;

    LOG(INFO) << "latency: " << maxLatency << " " << proxy_get_latency(mAlsaDeviceProxies[0].get());
    return ::android::OK;
}

::android::status_t StreamAlsa::refinePosition(StreamDescriptor::Position* /* position */) {
    return ::android::OK;
}

void StreamAlsa::shutdown() {
    teardownIo();
}

ndk::ScopedAStatus StreamAlsa::setGain(float gain) {
    mGain = gain;
    return ndk::ScopedAStatus::ok();
}

unsigned StreamAlsa::getPALatency() {
    pa_usec_t latency;
    int negative;

    mPACtx->withLock([&]() {
        if (pa_stream_get_latency(mPAStream.get(), &latency, &negative) < 0) {
            LOG(WARNING) << __func__ << ": Failed to fetch latency info from PulseAudio";
            latency = UINT32_MAX;
        } else if (negative == 1) {
            LOG(WARNING) << __func__ << ": Negative latency value reported by PulseAudio";
            latency = 0;
        }
    });

    return static_cast<int>(latency / PA_USEC_PER_MSEC);
}

void StreamAlsa::inputIoThread(size_t idx) {
#if defined(__ANDROID__)
    setWorkerThreadPriority(pthread_gettid_np(pthread_self()));
    const std::string threadName = (std::string("in_") + std::to_string(idx)).substr(0, 15);
    pthread_setname_np(pthread_self(), threadName.c_str());
#endif
    const size_t bufferSize = mBufferSizeFrames * mFrameSizeBytes;
    std::vector<char> buffer(bufferSize);
    while (mIoThreadIsRunning) {
        if (int ret = proxy_read_with_retries(mAlsaDeviceProxies[idx].get(), &buffer[0], bufferSize,
                                              mReadWriteRetries);
            ret == 0) {
            size_t bufferFramesWritten = 0;
            while (bufferFramesWritten < mBufferSizeFrames) {
                if (!mIoThreadIsRunning) return;
                ssize_t framesWrittenOrError =
                        mSinks[idx]->write(&buffer[0], mBufferSizeFrames - bufferFramesWritten);
                if (framesWrittenOrError >= 0) {
                    bufferFramesWritten += framesWrittenOrError;
                } else {
                    LOG(WARNING) << __func__ << "[" << idx
                                 << "]: Error while writing into the pipe: "
                                 << framesWrittenOrError;
                }
            }
        } else {
            // Errors when the stream is being stopped are expected.
            LOG_IF(WARNING, mIoThreadIsRunning)
                    << __func__ << "[" << idx << "]: Error reading from ALSA: " << ret;
        }
    }
}

void StreamAlsa::outputIoThread(size_t idx) {
#if defined(__ANDROID__)
    setWorkerThreadPriority(pthread_gettid_np(pthread_self()));
    const std::string threadName = (std::string("out_") + std::to_string(idx)).substr(0, 15);
    pthread_setname_np(pthread_self(), threadName.c_str());
#endif
    const size_t bufferSize = mBufferSizeFrames * mFrameSizeBytes;
    std::vector<char> buffer(bufferSize);
    while (mIoThreadIsRunning) {
        ssize_t framesReadOrError = mSources[idx]->read(&buffer[0], mBufferSizeFrames);
        if (framesReadOrError > 0) {
            int ret = proxy_write_with_retries(mAlsaDeviceProxies[idx].get(), &buffer[0],
                                               framesReadOrError * mFrameSizeBytes,
                                               mReadWriteRetries);

            ret = pa_stream_write(mPAStream.get(), &buffer[0], framesReadOrError * mFrameSizeBytes, nullptr, 0, PA_SEEK_RELATIVE);
            LOG_IF(WARNING, ret < 0) << __func__ << ": Error writing into PulseAudio";

            // Errors when the stream is being stopped are expected.
            LOG_IF(WARNING, ret != 0 && mIoThreadIsRunning)
                    << __func__ << "[" << idx << "]: Error writing into ALSA: " << ret;
        } else if (framesReadOrError == 0) {
            // MonoPipeReader does not have a blocking read, while use of std::condition_variable
            // requires use of a mutex. For now, just do a 1ms sleep. Consider using a different
            // pipe / ring buffer mechanism.
            if (mIoThreadIsRunning) usleep(1000);
        } else {
            LOG(WARNING) << __func__ << "[" << idx
                         << "]: Error while reading from the pipe: " << framesReadOrError;
        }
    }
}

void StreamAlsa::teardownIo() {
    mIoThreadIsRunning = false;
    if (mIsInput) {
        LOG(DEBUG) << __func__ << ": shutting down pipes";
        for (auto& sink : mSinks) {
            sink->shutdown(true);
        }
    }
    LOG(DEBUG) << __func__ << ": stopping PCM streams";
    for (const auto& proxy : mAlsaDeviceProxies) {
        proxy_stop(proxy.get());
    }
    LOG(DEBUG) << __func__ << ": joining threads";
    for (auto& thread : mIoThreads) {
        if (thread.joinable()) thread.join();
    }
    mIoThreads.clear();
    LOG(DEBUG) << __func__ << ": closing PCM devices";
    mAlsaDeviceProxies.clear();
    mSources.clear();
    mSinks.clear();
}

}  // namespace aidl::android::hardware::audio::core
