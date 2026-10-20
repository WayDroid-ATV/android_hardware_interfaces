/*
 * Copyright 2026 The Waydroid Project
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
#include <memory>

#include <aidl/vendor/waydroid/bluetooth/BnBluetoothProxy.h>

namespace aidl::vendor::waydroid::bluetooth::impl {

using ndk::ScopedAStatus;
using ndk::ScopedFileDescriptor;

class BluetoothProxy : public BnBluetoothProxy {
  public:
    // AIDL methods
    ScopedAStatus setCallback(const std::shared_ptr<IBluetoothProxyCallback>& cb) override;

    // Helpers
    static int openHciConnection();

  private:
    static std::atomic<bool> mCallbackSet = false;
    static std::shared_ptr<IBluetoothProxyCallback> mCallback = nullptr;
};

}
