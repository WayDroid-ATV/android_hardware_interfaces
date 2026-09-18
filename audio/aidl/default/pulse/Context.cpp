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

#include <chrono>
#include <format>
#include <future>
#include <string>

#define LOG_TAG "AHAL_PulseAudio"
#include <android-base/logging.h>

#include <android-base/properties.h>

#include "Context.h"

namespace aidl::android::hardware::audio::core::pulse {

using ::android::base::GetProperty;

Context::Context()
    : mMainloop(nullptr, MainloopDeleter()),
      mCtx(nullptr, ContextDeleter(this)) {}

::android::status_t Context::init() {
    static std::once_flag initialized;

    std::call_once(initialized, [&]() {
        const std::string pulseServerPath = std::format(
            "unix:{}/native",
            GetProperty("waydroid.pulse_runtime_path", "/run/user/1000/pulse")
        );

        LOG(DEBUG) << __func__ << ": libpulse is initializing...";

        mMainloop.reset(pa_threaded_mainloop_new());
        if (mMainloop == nullptr) {
            LOG(ERROR) << __func__ << ": mainloop creation failed";
            return;
        }

        int ret = pa_threaded_mainloop_start(mMainloop.get());
        if (ret < 0) {
            LOG(ERROR) << __func__ << ": libpulse failed to start: " << pa_strerror(ret);
            return;
        }

        LOG(DEBUG) << __func__ << ": libpulse mainloop started";

        // Create context and wait for creation
        withLock([&]() {
            mCtx.reset(pa_context_new(pa_threaded_mainloop_get_api(mMainloop.get()), "Waydroid"));

            if (mCtx == nullptr) {
                LOG(ERROR) << __func__ << ": failed to create PulseAudio context";
                return;
            }

            pa_context_set_state_callback(mCtx.get(), [](auto, void* userdata) {
                pa_threaded_mainloop_signal(reinterpret_cast<pa_threaded_mainloop*>(userdata), 0);
            }, mMainloop.get());

            if (pa_context_connect(mCtx.get(), pulseServerPath.c_str(), PA_CONTEXT_NOFLAGS, nullptr) < 0) {
                LOG(ERROR) << __func__ << ": failed to create PulseAudio context";
                return;
            }

            // Wait until context is ready
            while (pa_context_state_t s = pa_context_get_state(mCtx.get())) {
                if (!PA_CONTEXT_IS_GOOD(s) || s == PA_CONTEXT_READY) {
                    break;
                }
                pa_threaded_mainloop_wait(mMainloop.get());
            }
        });
    });

    // Verify context status, reset to nullptr if failed
    if (mMainloop && mCtx) {
        withLock([&]() {
            if (pa_context_get_state(mCtx.get()) != PA_CONTEXT_READY) {
                mCtx.reset();
            }
        });
    }

    if (mMainloop && mCtx) {
        LOG(INFO) << __func__ << ": PulseAudio context created successfully";
        return ::android::OK;
    } else {
        LOG(ERROR) << __func__ << ": Failed to initialize PulseAudio context";
        return ::android::NO_INIT;
    }
}

void Context::withLock(const std::function<void()>& lambda) {
    pa_threaded_mainloop_lock(mMainloop.get());
    if (lambda) lambda();
    pa_threaded_mainloop_unlock(mMainloop.get());
}

bool Context::waitForOperation(const std::function<pa_operation*(pa_stream_success_cb_t, std::promise<bool>*)>& lambda) {
    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();
    pa_operation *op;

    withLock([&]() {
        op = lambda([](pa_stream*, int success, void* userdata) {
            reinterpret_cast<std::promise<bool>*>(userdata)->set_value(success == 1);
        }, &promise);
    });

    if (op == nullptr) return false;

    // Timeout for all operations
    std::future_status status = future.wait_for(std::chrono::seconds(5));
    withLock([&]() {
        if (status == std::future_status::timeout) pa_operation_cancel(op);
        pa_operation_unref(op);
    });
    return (status == std::future_status::timeout) ? false : future.get();
}

std::shared_ptr<Context> Context::getContext() {
    static std::shared_ptr<Context> ctx(new Context());
    return ctx;
}

}  // namespace aidl::android::hardware::audio::core::pulse
