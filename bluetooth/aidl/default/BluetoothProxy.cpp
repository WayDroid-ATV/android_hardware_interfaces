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

#define LOG_TAG "android.hardware.bluetooth.service.default"

#include "BluetoothProxy.h"

#include "log/log.h"

namespace aidl::vendor::waydroid::bluetooth::impl {

ScopedAStatus BluetoothProxy::setCallback(const std::shared_ptr<IBluetoothProxyCallback>& cb) {
    mCallback = cb;
    mCallbackSet.store(true);
    mCallbackSet.notify_all();

    ALOGI("BluetoothProxy callback registered");
    return ScopedAStatus::ok();
}

int BluetoothProxy::openHciConnection() {
    // Wait for connection from host
    while (!mCallbackSet) {
        mCallbackSet.wait(mCallbackSet);
    }

    ScopedFileDescriptor hciFd;
    ScopedAStatus status = mCallback->requestHciConnection(0, &hciFd);

    if (!status.isOk()) {
        ALOGE("Failed to request HCI connection");
        return -1;
    }

    return hciFd.release();
}

}
