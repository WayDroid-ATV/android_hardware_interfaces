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

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#include <media/nbaio/MonoPipe.h>
#include <media/nbaio/MonoPipeReader.h>

#include "Stream.h"
#include "pulse/Context.h"

namespace aidl::android::hardware::audio::core {

// This class is intended to be used as a base class for implementations
// that use PulseAudio.
// This class does not define a complete stream implementation,
// and should never be used on its own. Derived classes are expected to
// provide necessary overrides for all interface methods omitted here.
class StreamPulse : public StreamCommonImpl {
  public:
    StreamPulse(StreamContext* context, const Metadata& metadata);
    ~StreamPulse();

    // Methods of 'DriverInterface'.
    ::android::status_t init(DriverCallbackInterface* callback) override;
    ::android::status_t drain(StreamDescriptor::DrainMode) override;
    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;
    ::android::status_t refinePosition(StreamDescriptor::Position* position) override;
    void shutdown() override;
    ndk::ScopedAStatus setGain(float gain) override;

  protected:
    struct StreamDeleter {
        std::shared_ptr<pulse::Context> mCtx;
        StreamDeleter(std::shared_ptr<pulse::Context> ctx) : mCtx(ctx) {}

        void operator()(pa_stream *s) const {
            if (s == nullptr) return;

            mCtx->withLock([&]() {
                pa_stream_set_state_callback(s, nullptr, nullptr);
                pa_stream_set_latency_update_callback(s, nullptr, nullptr);
                pa_stream_disconnect(s);
                pa_stream_unref(s);
            });
        }
    };

    const size_t mBufferSizeFrames;
    const int mChannelCount;
    const size_t mFrameSizeBytes;
    const int mSampleRate;
    const bool mIsInput;

    std::atomic<int32_t> mPALatency;
    std::shared_ptr<pulse::Context> mPAContext;
    std::unique_ptr<pa_stream, StreamDeleter> mPAStream;

  private:
    void initMonoPipe(bool writeCanBlock);
    void inputIoThread();
    void outputIoThread();
    void teardownIo();

    // All fields below are only used on the worker thread.
    std::atomic<bool> mPAReadyForWrite = true;
    std::atomic<size_t> mPAWritableSize = 0;

    // Only 'libnbaio_mono' is vendor-accessible, thus no access to the multi-reader Pipe.
    ::android::sp<::android::MonoPipe> mSinkIo;
    ::android::sp<::android::MonoPipeReader> mSourceIo;
    std::vector<std::thread> mIoThreads;
    std::atomic<bool> mIoThreadIsRunning = false;  // used by all threads
};

}  // namespace aidl::android::hardware::audio::core
