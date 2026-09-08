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

void Context::init() {
    std::call_once(mInitialized, [&]() {
        std::promise<pa_context_state_t> promise;
        std::string pulseServerPath;

        LOG(DEBUG) << __func__ << ": libpulse is initializing...";

        if (mMainloop.reset(pa_threaded_mainloop_new()); mMainloop == nullptr) {
            LOG(ERROR) << __func__ << ": mainloop creation failed";
            return;
        }

        if (int ret = pa_threaded_mainloop_start(mMainloop.get()); ret < 0) {
            LOG(ERROR) << __func__ << ": libpulse failed to start: " << pa_strerror(ret);
            mMainloop.reset();
            return;
        }

        LOG(DEBUG) << __func__ << ": libpulse mainloop started";

        // Create context and wait for creation
        withLock([&]() {
            mCtx.reset(pa_context_new(
                pa_threaded_mainloop_get_api(mMainloop.get()),
                "Waydroid"
            ));

            if (mCtx == nullptr) {
                LOG(ERROR) << __func__ << ": failed to create PulseAudio context";
                mMainloop.reset();
                return;
            }

            pa_context_set_state_callback(mCtx.get(), [](pa_context *c, void *promise) {
                pa_context_state_t state = pa_context_get_state(c);

                // Only handle ready/fail status
                if (state == PA_CONTEXT_READY || !PA_CONTEXT_IS_GOOD(state)) {
                    pa_context_set_state_callback(c, nullptr, nullptr);
                    reinterpret_cast<std::promise<pa_context_state_t>*>(promise)->set_value(state);
                }
            }, &promise);

            pulseServerPath = std::format(
                "unix:{}/native",
                GetProperty("waydroid.pulse_runtime_path", "/run/user/1000/pulse")
            );

            if (pa_context_connect(mCtx.get(), pulseServerPath.c_str(), PA_CONTEXT_NOFLAGS, nullptr) < 0) {
                pa_context_set_state_callback(mCtx.get(), nullptr, nullptr);
                promise.set_value(PA_CONTEXT_FAILED);
            }
        });

        if (!PA_CONTEXT_IS_GOOD(promise.get_future().get())) {
            LOG(ERROR) << __func__ << ": context creation failed: " << pa_strerror(pa_context_errno(mCtx.get()));
            return;
        }
    });

    LOG(INFO) << __func__ << ": PulseAudio context created successfully";
    return;
}

void Context::withLock(const std::function<void()>& lambda) {
    pa_threaded_mainloop_lock(mMainloop.get());
    if (lambda) lambda();
    pa_threaded_mainloop_unlock(mMainloop.get());
}

bool Context::waitForOperation(const std::function<pa_operation*(std::promise<bool>&)>& lambda) {
    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();
    pa_operation *op;

    withLock([&]() { op = lambda(promise); });
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
