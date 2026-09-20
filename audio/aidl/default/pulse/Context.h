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

#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>

extern "C" {
#include <pulse/pulseaudio.h>
}

#include <utils/Errors.h>

namespace aidl::android::hardware::audio::core::pulse {

// A class for PulseAudio context
class Context {
  protected:
    struct MainloopDeleter {
        void operator()(pa_threaded_mainloop *mainloop) const {
            pa_threaded_mainloop_stop(mainloop);
            pa_threaded_mainloop_free(mainloop);
        }
    };

    struct ContextDeleter {
        Context *mInstance;
        ContextDeleter(Context *instance) : mInstance(instance) {}

        void operator()(pa_context *ctx) const {
            if (ctx == nullptr) return;
            mInstance->withLock([&]() {
                pa_context_disconnect(ctx);
                pa_context_unref(ctx);
            });
        }
    };

    Context();

  public:
    std::unique_ptr<pa_threaded_mainloop, MainloopDeleter> mMainloop;
    std::unique_ptr<pa_context, ContextDeleter> mCtx;

    ::android::status_t init();
    const char* getLastError();
    bool waitForOperation(const std::function<pa_operation*(pa_context_success_cb_t, std::promise<bool>*)>& lambda);
    bool waitForStreamOperation(const std::function<pa_operation*(pa_stream_success_cb_t, std::promise<bool>*)>& lambda);
    void withLock(const std::function<void()>& lambda);
    static std::shared_ptr<Context> getContext();
};

}  // namespace aidl::android::hardware::audio::core::pulse
