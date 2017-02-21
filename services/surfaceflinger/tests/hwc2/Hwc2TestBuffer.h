/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef _HWC2_TEST_BUFFER_H
#define _HWC2_TEST_BUFFER_H

#include <android-base/unique_fd.h>
#include <set>

#include <hardware/hwcomposer2.h>

#include <gui/GraphicBufferAlloc.h>
#include <ui/GraphicBuffer.h>

class Hwc2TestFenceGenerator;
class Hwc2TestLayers;

class Hwc2TestBuffer {
public:
    Hwc2TestBuffer();
    ~Hwc2TestBuffer();

    void updateBufferArea(int32_t bufferWidth, int32_t bufferHeight);

    int  get(buffer_handle_t* outHandle, int32_t* outFence);

protected:
    int generateBuffer();

    android::GraphicBufferAlloc mGraphicBufferAlloc;
    android::sp<android::GraphicBuffer> mGraphicBuffer;

    std::unique_ptr<Hwc2TestFenceGenerator> mFenceGenerator;

    int32_t mBufferWidth = -1;
    int32_t mBufferHeight = -1;
    const android_pixel_format_t mFormat = HAL_PIXEL_FORMAT_RGBA_8888;

    bool mValidBuffer = false;
    buffer_handle_t mHandle = nullptr;
};


class Hwc2TestClientTargetBuffer {
public:
    Hwc2TestClientTargetBuffer();
    ~Hwc2TestClientTargetBuffer();

    int  get(buffer_handle_t* outHandle, int32_t* outFence,
            int32_t bufferWidth, int32_t bufferHeight,
            const Hwc2TestLayers* testLayers,
            const std::set<hwc2_layer_t>* clientLayers,
            const std::set<hwc2_layer_t>* clearLayers);

protected:
    android::GraphicBufferAlloc mGraphicBufferAlloc;
    android::sp<android::GraphicBuffer> mGraphicBuffer;

    std::unique_ptr<Hwc2TestFenceGenerator> mFenceGenerator;

    const android_pixel_format_t mFormat = HAL_PIXEL_FORMAT_RGBA_8888;
};

#endif /* ifndef _HWC2_TEST_BUFFER_H */
