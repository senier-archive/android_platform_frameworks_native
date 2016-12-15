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

#include <array>
#include <unordered_set>
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <android-base/unique_fd.h>
#include <hardware/hardware.h>

#define HWC2_INCLUDE_STRINGIFICATION
#define HWC2_USE_CPP11
#include <hardware/hwcomposer2.h>
#undef HWC2_INCLUDE_STRINGIFICATION
#undef HWC2_USE_CPP11

#include "Hwc2TestLayer.h"
#include "Hwc2TestLayers.h"

void hwc2TestHotplugCallback(hwc2_callback_data_t callbackData,
        hwc2_display_t display, int32_t connected);
void hwc2TestVsyncCallback(hwc2_callback_data_t callbackData,
        hwc2_display_t display, int64_t timestamp);

class Hwc2Test : public testing::Test {
public:

    virtual void SetUp()
    {
        hw_module_t const* hwc2Module;

        int err = hw_get_module(HWC_HARDWARE_MODULE_ID, &hwc2Module);
        ASSERT_GE(err, 0) << "failed to get hwc hardware module: "
                << strerror(-err);

        /* The following method will fail if you have not run
         * "adb shell stop" */
        err = hwc2_open(hwc2Module, &mHwc2Device);
        ASSERT_GE(err, 0) << "failed to open hwc hardware module: "
                << strerror(-err);

        populateDisplays();
    }

    virtual void TearDown()
    {

        for (auto itr = mLayers.begin(); itr != mLayers.end();) {
            hwc2_display_t display = itr->first;
            hwc2_layer_t layer = itr->second;
            itr++;
            /* Destroys and removes the layer from mLayers */
            destroyLayer(display, layer);
        }

        for (auto itr = mActiveDisplays.begin(); itr != mActiveDisplays.end();) {
            hwc2_display_t display = *itr;
            itr++;
            /* Sets power mode to off and removes the display from
             * mActiveDisplays */
            setPowerMode(display, HWC2_POWER_MODE_OFF);
        }

        if (mHwc2Device)
            hwc2_close(mHwc2Device);
    }

    void registerCallback(hwc2_callback_descriptor_t descriptor,
            hwc2_callback_data_t callbackData, hwc2_function_pointer_t pointer,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_REGISTER_CALLBACK>(
                getFunction(HWC2_FUNCTION_REGISTER_CALLBACK));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, descriptor,
                callbackData, pointer));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to register callback";
        }
    }

    void getDisplayType(hwc2_display_t display, hwc2_display_type_t* outType,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_GET_DISPLAY_TYPE>(
                getFunction(HWC2_FUNCTION_GET_DISPLAY_TYPE));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                    reinterpret_cast<int32_t*>(outType)));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get display type";
        }
    }

    /* If the populateDisplays function is still receiving displays and the
     * display is connected, the display handle is stored in mDisplays. */
    void hotplugCallback(hwc2_display_t display, int32_t connected)
    {
        std::lock_guard<std::mutex> lock(mHotplugMutex);

        if (mHotplugStatus != Hwc2TestHotplugStatus::Receiving)
            return;

        if (connected == HWC2_CONNECTION_CONNECTED)
            mDisplays.insert(display);

        mHotplugCv.notify_all();
    }

    void createLayer(hwc2_display_t display, hwc2_layer_t* outLayer,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_CREATE_LAYER>(
                getFunction(HWC2_FUNCTION_CREATE_LAYER));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                outLayer));

        if (err == HWC2_ERROR_NONE)
            mLayers.insert(std::make_pair(display, *outLayer));

        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to create layer";
        }
    }

    void destroyLayer(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_DESTROY_LAYER>(
                getFunction(HWC2_FUNCTION_DESTROY_LAYER));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer));

        if (err == HWC2_ERROR_NONE)
            mLayers.erase(std::make_pair(display, layer));

        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to destroy layer "
                    << layer;
        }
    }

    void getDisplayAttribute(hwc2_display_t display, hwc2_config_t config,
            hwc2_attribute_t attribute, int32_t* outValue,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_GET_DISPLAY_ATTRIBUTE>(
                getFunction(HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, config,
                attribute, outValue));

        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get display attribute "
                    << getAttributeName(attribute) << " for config " << config;
        }
    }

    void getDisplayConfigs(hwc2_display_t display,
            std::vector<hwc2_config_t>* outConfigs,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_GET_DISPLAY_CONFIGS>(
                getFunction(HWC2_FUNCTION_GET_DISPLAY_CONFIGS));
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t numConfigs = 0;

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                &numConfigs, nullptr));

        if (err == HWC2_ERROR_NONE) {
            outConfigs->resize(numConfigs);

            err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                    &numConfigs, outConfigs->data()));
        }

        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get configs for"
                    " display " << display;
        }
    }

    void getActiveConfig(hwc2_display_t display, hwc2_config_t* outConfig,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_GET_ACTIVE_CONFIG>(
                getFunction(HWC2_FUNCTION_GET_ACTIVE_CONFIG));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                outConfig));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get active config on"
                    " display " << display;
        }
    }

    void setActiveConfig(hwc2_display_t display, hwc2_config_t config,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_ACTIVE_CONFIG>(
                getFunction(HWC2_FUNCTION_SET_ACTIVE_CONFIG));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, config));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set active config "
                    << config;
        }
    }

    void getDozeSupport(hwc2_display_t display, int32_t* outSupport,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_GET_DOZE_SUPPORT>(
                getFunction(HWC2_FUNCTION_GET_DOZE_SUPPORT));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                outSupport));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get doze support on"
                    " display " << display;
        }
    }

    void setPowerMode(hwc2_display_t display, hwc2_power_mode_t mode,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_POWER_MODE>(
                getFunction(HWC2_FUNCTION_SET_POWER_MODE));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                mode));
        if (outErr) {
            *outErr = err;
            if (err != HWC2_ERROR_NONE)
                return;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set power mode "
                    << getPowerModeName(mode) << " on display " << display;
        }

        if (mode == HWC2_POWER_MODE_OFF) {
            mActiveDisplays.erase(display);
        } else {
            mActiveDisplays.insert(display);
        }
    }

    void setVsyncEnabled(hwc2_display_t display, hwc2_vsync_t enabled,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_VSYNC_ENABLED>(
                getFunction(HWC2_FUNCTION_SET_VSYNC_ENABLED));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                enabled));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set vsync enabled "
                    << getVsyncName(enabled);
        }
    }

    void vsyncCallback(hwc2_display_t display, int64_t timestamp)
    {
        std::lock_guard<std::mutex> lock(mVsyncMutex);
        mVsyncDisplay = display;
        mVsyncTimestamp = timestamp;
        mVsyncCv.notify_all();
    }

    void getDisplayName(hwc2_display_t display, std::string* outName,
                hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_GET_DISPLAY_NAME>(
                getFunction(HWC2_FUNCTION_GET_DISPLAY_NAME));
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t size = 0;

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, &size,
                nullptr));

        if (err == HWC2_ERROR_NONE) {
            std::vector<char> name(size);

            err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, &size,
                    name.data()));

            outName->assign(name.data());
        }

        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get display name for "
                    << display;
        }
    }

    void setLayerCompositionType(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_composition_t composition, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_COMPOSITION_TYPE>(
                getFunction(HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                composition));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer composition"
                    " type " << getCompositionName(composition);
        }
    }

    void setCursorPosition(hwc2_display_t display, hwc2_layer_t layer,
            int32_t x, int32_t y, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_CURSOR_POSITION>(
                getFunction(HWC2_FUNCTION_SET_CURSOR_POSITION));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer, x,
                y));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set cursor position";
        }
    }

    void setLayerBlendMode(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_blend_mode_t mode, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_BLEND_MODE>(
                getFunction(HWC2_FUNCTION_SET_LAYER_BLEND_MODE));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                mode));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer blend mode "
                    << getBlendModeName(mode);
        }
    }

    void setLayerBuffer(hwc2_display_t display, hwc2_layer_t layer,
            buffer_handle_t buffer, int32_t acquireFence,
            hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_BUFFER>(
                getFunction(HWC2_FUNCTION_SET_LAYER_BUFFER));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                buffer, acquireFence));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer buffer";
        }
    }

    void setLayerColor(hwc2_display_t display, hwc2_layer_t layer,
            hwc_color_t color, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_COLOR>(
                getFunction(HWC2_FUNCTION_SET_LAYER_COLOR));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                color));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer color";
        }
    }

    void setLayerDataspace(hwc2_display_t display, hwc2_layer_t layer,
            android_dataspace_t dataspace, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_DATASPACE>(
                getFunction(HWC2_FUNCTION_SET_LAYER_DATASPACE));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display,
                layer, dataspace));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer dataspace";
        }
    }

    void setLayerDisplayFrame(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_rect_t& displayFrame, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_DISPLAY_FRAME>(
                getFunction(HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                displayFrame));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer display"
                    " frame";
        }
    }

    void setLayerPlaneAlpha(hwc2_display_t display, hwc2_layer_t layer,
            float alpha, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_PLANE_ALPHA>(
                getFunction(HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                alpha));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer plane alpha "
                    << alpha;
        }
    }

    void setLayerSourceCrop(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_frect_t& sourceCrop, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_SOURCE_CROP>(
                getFunction(HWC2_FUNCTION_SET_LAYER_SOURCE_CROP));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                sourceCrop));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer source crop";
        }
    }

    void setLayerSurfaceDamage(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_region_t& surfaceDamage, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_SURFACE_DAMAGE>(
                getFunction(HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                surfaceDamage));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer surface"
                    " damage";
        }
    }

    void setLayerTransform(hwc2_display_t display, hwc2_layer_t layer,
            hwc_transform_t transform, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_TRANSFORM>(
                getFunction(HWC2_FUNCTION_SET_LAYER_TRANSFORM));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                transform));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer transform "
                    << getTransformName(transform);
        }
    }

    void setLayerVisibleRegion(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_region_t& visibleRegion, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_VISIBLE_REGION>(
                getFunction(HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                visibleRegion));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer visible"
                    " region";
        }
    }

    void setLayerZOrder(hwc2_display_t display, hwc2_layer_t layer,
            uint32_t zOrder, hwc2_error_t* outErr = nullptr)
    {
        auto pfn = reinterpret_cast<HWC2_PFN_SET_LAYER_Z_ORDER>(
                getFunction(HWC2_FUNCTION_SET_LAYER_Z_ORDER));
        ASSERT_TRUE(pfn) << "failed to get function";

        auto err = static_cast<hwc2_error_t>(pfn(mHwc2Device, display, layer,
                zOrder));
        if (outErr) {
            *outErr = err;
        } else {
            ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer z order "
                    << zOrder;
        }
    }

protected:
    hwc2_function_pointer_t getFunction(hwc2_function_descriptor_t descriptor)
    {
        return mHwc2Device->getFunction(mHwc2Device, descriptor);
    }

    void getCapabilities(std::vector<hwc2_capability_t>* outCapabilities)
    {
        uint32_t num = 0;

        mHwc2Device->getCapabilities(mHwc2Device, &num, nullptr);

        outCapabilities->resize(num);

        mHwc2Device->getCapabilities(mHwc2Device, &num,
                reinterpret_cast<int32_t*>(outCapabilities->data()));
    }

    /* Registers a hotplug callback and waits for hotplug callbacks. This
     * function will have no effect if called more than once. */
    void populateDisplays()
    {
        /* Sets the hotplug status to receiving */
        {
            std::lock_guard<std::mutex> lock(mHotplugMutex);

            if (mHotplugStatus != Hwc2TestHotplugStatus::Init)
                return;
            mHotplugStatus = Hwc2TestHotplugStatus::Receiving;
        }

        /* Registers the callback. This function call cannot be locked because
         * a callback could happen on the same thread */
        ASSERT_NO_FATAL_FAILURE(registerCallback(HWC2_CALLBACK_HOTPLUG, this,
                reinterpret_cast<hwc2_function_pointer_t>(
                hwc2TestHotplugCallback)));

        /* Waits for hotplug events. If a hotplug event has not come within 1
         * second, stop waiting. */
        std::unique_lock<std::mutex> lock(mHotplugMutex);

        while (mHotplugCv.wait_for(lock, std::chrono::seconds(1)) !=
                std::cv_status::timeout) { }

        /* Sets the hotplug status to done. Future calls will have no effect */
        mHotplugStatus = Hwc2TestHotplugStatus::Done;
    }

    void getBadDisplay(hwc2_display_t* outDisplay)
    {
        for (hwc2_display_t display = 0; display < UINT64_MAX; display++) {
            if (mDisplays.count(display) == 0) {
                *outDisplay = display;
                return;
            }
        }
        ASSERT_TRUE(false) << "Unable to find bad display. UINT64_MAX displays"
                " are registered. This should never happen.";
    }

    /* NOTE: will create min(newlayerCnt, max supported layers) layers */
    void createLayers(hwc2_display_t display,
            std::vector<hwc2_layer_t>* outLayers, size_t newLayerCnt)
    {
        std::vector<hwc2_layer_t> newLayers;
        hwc2_layer_t layer;
        hwc2_error_t err = HWC2_ERROR_NONE;

        for (size_t i = 0; i < newLayerCnt; i++) {

            EXPECT_NO_FATAL_FAILURE(createLayer(display, &layer, &err));
            if (err == HWC2_ERROR_NO_RESOURCES)
                break;
            if (err != HWC2_ERROR_NONE) {
                newLayers.clear();
                ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to create layer";
            }
            newLayers.push_back(layer);
        }

        *outLayers = std::move(newLayers);
    }

    void destroyLayers(hwc2_display_t display,
            std::vector<hwc2_layer_t>&& layers)
    {
        for (hwc2_layer_t layer : layers) {
            EXPECT_NO_FATAL_FAILURE(destroyLayer(display, layer));
        }
    }

    void getInvalidConfig(hwc2_display_t display, hwc2_config_t* outConfig)
    {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        hwc2_config_t CONFIG_MAX = UINT32_MAX;

        ASSERT_LE(configs.size() - 1, CONFIG_MAX) << "every config value"
                " (2^32 values) has been taken which shouldn't happen";

        hwc2_config_t config;
        for (config = 0; config < CONFIG_MAX; config++) {
            if (std::count(configs.begin(), configs.end(), config) == 0)
                break;
        }

        *outConfig = config;
    }

    void enableVsync(hwc2_display_t display)
    {
        ASSERT_NO_FATAL_FAILURE(registerCallback(HWC2_CALLBACK_VSYNC, this,
                reinterpret_cast<hwc2_function_pointer_t>(
                hwc2TestVsyncCallback)));
        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_ENABLE));
    }

    void disableVsync(hwc2_display_t display)
    {
        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE));
    }

    void waitForVsync(hwc2_display_t* outDisplay = nullptr,
            int64_t* outTimestamp = nullptr)
    {
        std::unique_lock<std::mutex> lock(mVsyncMutex);
        ASSERT_EQ(mVsyncCv.wait_for(lock, std::chrono::seconds(3)),
                std::cv_status::no_timeout) << "timed out attempting to get"
                " vsync callback";
        if (outDisplay)
            *outDisplay = mVsyncDisplay;
        if (outTimestamp)
            *outTimestamp = mVsyncTimestamp;
    }

    /* Calls a set property function from Hwc2Test to set a property value from
     * Hwc2TestLayer to hwc2_layer_t on hwc2_display_t */
    using TestLayerPropertyFunction = void (*)(Hwc2Test* test,
            hwc2_display_t display, hwc2_layer_t layer,
            Hwc2TestLayer* testLayer, hwc2_error_t* outErr);

    /* Calls a set property function from Hwc2Test to set property values from
     * Hwc2TestLayers to hwc2_layer_t on hwc2_display_t */
    using TestLayerPropertiesFunction = void (*)(Hwc2Test* test,
            hwc2_display_t display, hwc2_layer_t layer,
            Hwc2TestLayers* testLayers);

    /* Calls a set property function from Hwc2Test to set a bad property value
     * on hwc2_layer_t on hwc2_display_t */
    using TestLayerPropertyBadLayerFunction = void (*)(Hwc2Test* test,
            hwc2_display_t display, hwc2_layer_t layer,
            Hwc2TestLayer* testLayer, hwc2_error_t* outErr);

    /* Calls a set property function from Hwc2Test to set a bad property value
     * on hwc2_layer_t on hwc2_display_t */
    using TestLayerPropertyBadParameterFunction = void (*)(Hwc2Test* test,
            hwc2_display_t display, hwc2_layer_t layer, hwc2_error_t* outErr);

    /* Advances a property of Hwc2TestLayer */
    using AdvanceProperty = bool (*)(Hwc2TestLayer* testLayer);

    /* Advances properties of Hwc2TestLayers */
    using AdvanceProperties = bool (*)(Hwc2TestLayers* testLayer);

    /* For each active display it cycles through each display config and tests
     * each property value. It creates a layer, sets the property and then
     * destroys the layer */
    void setLayerProperty(Hwc2TestCoverage coverage,
            TestLayerPropertyFunction function, AdvanceProperty advance)
    {
        for (auto display : mDisplays) {
            std::vector<hwc2_config_t> configs;

            ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

            for (auto config : configs) {
                hwc2_layer_t layer;
                Area displayArea;

                ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config));
                ASSERT_NO_FATAL_FAILURE(getActiveDisplayArea(display,
                        &displayArea));
                Hwc2TestLayer testLayer(coverage, displayArea);

                do {
                    ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

                    ASSERT_NO_FATAL_FAILURE(function(this, display, layer,
                            &testLayer, nullptr));

                    ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));
                } while (advance(&testLayer));
            }
        }
    }

    /* For each active display it cycles through each display config and tests
     * each property value. It creates a layer, cycles through each property
     * value and updates the layer property value and then destroys the layer */
    void setLayerPropertyUpdate(Hwc2TestCoverage coverage,
            TestLayerPropertyFunction function, AdvanceProperty advance)
    {
        for (auto display : mDisplays) {
            std::vector<hwc2_config_t> configs;

            ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

            for (auto config : configs) {
                hwc2_layer_t layer;
                Area displayArea;

                ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config));
                ASSERT_NO_FATAL_FAILURE(getActiveDisplayArea(display,
                        &displayArea));
                Hwc2TestLayer testLayer(coverage, displayArea);

                ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

                do {
                    ASSERT_NO_FATAL_FAILURE(function(this, display, layer,
                            &testLayer, nullptr));
                } while (advance(&testLayer));

                ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));
            }
        }
    }

    /* For each active display it cycles through each display config and tests
     * each property value. It creates multiple layers, calls the
     * TestLayerPropertiesFunction to set property values and then
     * destroys the layers */
    void setLayerProperties(Hwc2TestCoverage coverage, size_t layerCnt,
            TestLayerPropertiesFunction function, AdvanceProperties advance)
    {
        for (auto display : mDisplays) {
            std::vector<hwc2_config_t> configs;

            ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

            for (auto config : configs) {
                std::vector<hwc2_layer_t> layers;
                Area displayArea;

                ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config));
                ASSERT_NO_FATAL_FAILURE(getActiveDisplayArea(display,
                        &displayArea));

                ASSERT_NO_FATAL_FAILURE(createLayers(display, &layers, layerCnt));
                Hwc2TestLayers testLayers(layers, coverage, displayArea);

                do {
                    for (auto layer : layers) {
                        EXPECT_NO_FATAL_FAILURE(function(this, display, layer,
                                &testLayers));
                    }
                } while (advance(&testLayers));

                ASSERT_NO_FATAL_FAILURE(destroyLayers(display, std::move(layers)));
            }
        }
    }

    /* For each active display it cycles through each display config.
     * 1) It attempts to set a valid property value to bad layer handle.
     * 2) It creates a layer x and attempts to set a valid property value to
     *    layer x + 1
     * 3) It destroys the layer x and attempts to set a valid property value to
     *    the destroyed layer x.
     */
    void setLayerPropertyBadLayer(Hwc2TestCoverage coverage,
            TestLayerPropertyBadLayerFunction function)
    {
        for (auto display : mDisplays) {
            std::vector<hwc2_config_t> configs;

            ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

            for (auto config : configs) {
                hwc2_layer_t layer = 0;
                Area displayArea;
                hwc2_error_t err = HWC2_ERROR_NONE;

                ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config));
                ASSERT_NO_FATAL_FAILURE(getActiveDisplayArea(display,
                        &displayArea));
                Hwc2TestLayer testLayer(coverage, displayArea);

                ASSERT_NO_FATAL_FAILURE(function(this, display, layer,
                        &testLayer, &err));
                EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

                ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

                ASSERT_NO_FATAL_FAILURE(function(this, display, layer + 1,
                        &testLayer, &err));
                EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

                ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));

                ASSERT_NO_FATAL_FAILURE(function(this, display, layer,
                        &testLayer, &err));
                EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
            }
        }
    }

    /* For each active display it cycles through each display config and tests
     * each property value. It creates a layer, sets a bad property value and
     * then destroys the layer */
    void setLayerPropertyBadParameter(TestLayerPropertyBadParameterFunction function)
    {
        for (auto display : mDisplays) {
            std::vector<hwc2_config_t> configs;

            ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

            for (auto config : configs) {
                hwc2_layer_t layer;
                hwc2_error_t err = HWC2_ERROR_NONE;

                ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config));

                ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

                ASSERT_NO_FATAL_FAILURE(function(this, display, layer, &err));
                EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong"
                        " error code";

                ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));
            }
        }
    }

    void getActiveConfigAttribute(hwc2_display_t display,
            hwc2_attribute_t attribute, int32_t* outValue)
    {
        hwc2_config_t config;
        ASSERT_NO_FATAL_FAILURE(getActiveConfig(display, &config));
        ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config,
                attribute, outValue));
        ASSERT_GE(*outValue, 0) << "failed to get valid "
                << getAttributeName(attribute);
    }

    void getActiveDisplayArea(hwc2_display_t display, Area* displayArea)
    {
        ASSERT_NO_FATAL_FAILURE(getActiveConfigAttribute(display,
                HWC2_ATTRIBUTE_WIDTH, &displayArea->width));
        ASSERT_NO_FATAL_FAILURE(getActiveConfigAttribute(display,
                HWC2_ATTRIBUTE_HEIGHT, &displayArea->height));
    }

    hwc2_device_t* mHwc2Device = nullptr;

    enum class Hwc2TestHotplugStatus {
        Init = 1,
        Receiving,
        Done,
    };

    std::mutex mHotplugMutex;
    std::condition_variable mHotplugCv;
    Hwc2TestHotplugStatus mHotplugStatus = Hwc2TestHotplugStatus::Init;
    std::unordered_set<hwc2_display_t> mDisplays;

    /* Store all created layers that have not been destroyed. If an ASSERT_*
     * fails, then destroy the layers on exit */
    std::set<std::pair<hwc2_display_t, hwc2_layer_t>> mLayers;

    /* Store the power mode state. If it is not HWC2_POWER_MODE_OFF when
     * tearing down the test cases, change it to HWC2_POWER_MODE_OFF */
    std::set<hwc2_display_t> mActiveDisplays;

    std::mutex mVsyncMutex;
    std::condition_variable mVsyncCv;
    hwc2_display_t mVsyncDisplay;
    int64_t mVsyncTimestamp = -1;
};

void hwc2TestHotplugCallback(hwc2_callback_data_t callbackData,
        hwc2_display_t display, int32_t connection)
{
    if (callbackData)
        static_cast<Hwc2Test*>(callbackData)->hotplugCallback(display,
                connection);
}

void hwc2TestVsyncCallback(hwc2_callback_data_t callbackData,
        hwc2_display_t display, int64_t timestamp)
{
    if (callbackData)
        static_cast<Hwc2Test*>(callbackData)->vsyncCallback(display,
                timestamp);
}

void setBlendMode(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerBlendMode(display, layer,
            testLayer->getBlendMode(), outErr));
}

void setBuffer(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    buffer_handle_t handle;
    android::base::unique_fd acquireFence;
    hwc2_composition_t composition = testLayer->getComposition();

    if (composition == HWC2_COMPOSITION_CLIENT
            || composition == HWC2_COMPOSITION_SOLID_COLOR
            || composition == HWC2_COMPOSITION_SIDEBAND)
        return;

    if (testLayer->getBuffer(&handle, &acquireFence) < 0)
        return;

    ASSERT_NO_FATAL_FAILURE(test->setLayerCompositionType(display, layer,
            composition));
    EXPECT_NO_FATAL_FAILURE(test->setLayerBuffer(display, layer,
            handle, acquireFence, outErr));
}

void setColor(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    ASSERT_NO_FATAL_FAILURE(test->setLayerCompositionType(display,
            layer, HWC2_COMPOSITION_SOLID_COLOR));
    ASSERT_NO_FATAL_FAILURE(test->setLayerBlendMode(display,
            layer, testLayer->getBlendMode()));
    EXPECT_NO_FATAL_FAILURE(test->setLayerColor(display, layer,
            testLayer->getColor(), outErr));
}

void setComposition(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    hwc2_composition_t composition = testLayer->getComposition();
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(test->setLayerCompositionType(display, layer,
            composition, &err));
    if (outErr) {
        *outErr = err;
        return;
    }

    if (composition != HWC2_COMPOSITION_SIDEBAND) {
        EXPECT_EQ(err, HWC2_ERROR_NONE) << "returned wrong error code";
    } else {
        EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_UNSUPPORTED)
                 << "returned wrong error code";
    }
}

void setCursorPosition(Hwc2Test* test, hwc2_display_t display,
        hwc2_layer_t layer, Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    ASSERT_NO_FATAL_FAILURE(test->setLayerCompositionType(display,
            layer, HWC2_COMPOSITION_CURSOR));

    const hwc_rect_t cursorPosition = testLayer->getCursorPosition();
    EXPECT_NO_FATAL_FAILURE(test->setCursorPosition(display, layer,
            cursorPosition.left, cursorPosition.top, outErr));
}

void setDataspace(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerDataspace(display, layer,
            testLayer->getDataspace(), outErr));
}

void setDisplayFrame(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerDisplayFrame(display, layer,
            testLayer->getDisplayFrame(), outErr));
}

void setPlaneAlpha(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t *outErr)
{
    ASSERT_NO_FATAL_FAILURE(test->setLayerBlendMode(display, layer,
            testLayer->getBlendMode()));
    EXPECT_NO_FATAL_FAILURE(test->setLayerPlaneAlpha(display, layer,
            testLayer->getPlaneAlpha(), outErr));
}

void setSourceCrop(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerSourceCrop(display, layer,
            testLayer->getSourceCrop(), outErr));
}

void setSurfaceDamage(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerSurfaceDamage(display, layer,
            testLayer->getSurfaceDamage(), outErr));
}

void setTransform(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerTransform(display, layer,
            testLayer->getTransform(), outErr));
}

void setVisibleRegion(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerVisibleRegion(display, layer,
            testLayer->getVisibleRegion(), outErr));
}

void setZOrder(Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
        Hwc2TestLayer* testLayer, hwc2_error_t* outErr)
{
    EXPECT_NO_FATAL_FAILURE(test->setLayerZOrder(display, layer,
            testLayer->getZOrder(), outErr));
}

bool advanceBlendMode(Hwc2TestLayer* testLayer)
{
    return testLayer->advanceBlendMode();
}

bool advanceBuffer(Hwc2TestLayer* testLayer)
{
    if (testLayer->advanceComposition())
        return true;
    return testLayer->advanceBufferArea();
}

bool advanceColor(Hwc2TestLayer* testLayer)
{
    if (testLayer->advanceBlendMode())
        return true;
    return testLayer->advanceColor();
}

bool advanceComposition(Hwc2TestLayer* testLayer)
{
    return testLayer->advanceComposition();
}

bool advanceCursorPosition(Hwc2TestLayer* testLayer)
{
    return testLayer->advanceCursorPosition();
}

bool advanceDataspace(Hwc2TestLayer* testLayer)
{
    return testLayer->advanceDataspace();
}

bool advanceDisplayFrame(Hwc2TestLayer* testLayer)
{
    return testLayer->advanceDisplayFrame();
}

bool advancePlaneAlpha(Hwc2TestLayer* testLayer)
{
    return testLayer->advancePlaneAlpha();
}

bool advanceSourceCrop(Hwc2TestLayer* testLayer)
{
    if (testLayer->advanceSourceCrop())
        return true;
    return testLayer->advanceBufferArea();
}

bool advanceSurfaceDamage(Hwc2TestLayer* testLayer)
{
    if (testLayer->advanceSurfaceDamage())
        return true;
    return testLayer->advanceBufferArea();
}

bool advanceTransform(Hwc2TestLayer* testLayer)
{
    return testLayer->advanceTransform();
}

bool advanceVisibleRegions(Hwc2TestLayers* testLayers)
{
    return testLayers->advanceVisibleRegions();
}


static const std::array<hwc2_function_descriptor_t, 42> requiredFunctions = {{
    HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES,
    HWC2_FUNCTION_CREATE_LAYER,
    HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY,
    HWC2_FUNCTION_DESTROY_LAYER,
    HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY,
    HWC2_FUNCTION_DUMP,
    HWC2_FUNCTION_GET_ACTIVE_CONFIG,
    HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES,
    HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT,
    HWC2_FUNCTION_GET_COLOR_MODES,
    HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE,
    HWC2_FUNCTION_GET_DISPLAY_CONFIGS,
    HWC2_FUNCTION_GET_DISPLAY_NAME,
    HWC2_FUNCTION_GET_DISPLAY_REQUESTS,
    HWC2_FUNCTION_GET_DISPLAY_TYPE,
    HWC2_FUNCTION_GET_DOZE_SUPPORT,
    HWC2_FUNCTION_GET_HDR_CAPABILITIES,
    HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT,
    HWC2_FUNCTION_GET_RELEASE_FENCES,
    HWC2_FUNCTION_PRESENT_DISPLAY,
    HWC2_FUNCTION_REGISTER_CALLBACK,
    HWC2_FUNCTION_SET_ACTIVE_CONFIG,
    HWC2_FUNCTION_SET_CLIENT_TARGET,
    HWC2_FUNCTION_SET_COLOR_MODE,
    HWC2_FUNCTION_SET_COLOR_TRANSFORM,
    HWC2_FUNCTION_SET_CURSOR_POSITION,
    HWC2_FUNCTION_SET_LAYER_BLEND_MODE,
    HWC2_FUNCTION_SET_LAYER_BUFFER,
    HWC2_FUNCTION_SET_LAYER_COLOR,
    HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE,
    HWC2_FUNCTION_SET_LAYER_DATASPACE,
    HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME,
    HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA,
    HWC2_FUNCTION_SET_LAYER_SOURCE_CROP,
    HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE,
    HWC2_FUNCTION_SET_LAYER_TRANSFORM,
    HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION,
    HWC2_FUNCTION_SET_LAYER_Z_ORDER,
    HWC2_FUNCTION_SET_OUTPUT_BUFFER,
    HWC2_FUNCTION_SET_POWER_MODE,
    HWC2_FUNCTION_SET_VSYNC_ENABLED,
    HWC2_FUNCTION_VALIDATE_DISPLAY,
}};

/* TESTCASE: Tests that the HWC2 supports all required functions. */
TEST_F(Hwc2Test, GET_FUNCTION)
{
    for (hwc2_function_descriptor_t descriptor : requiredFunctions) {
        hwc2_function_pointer_t pfn = getFunction(descriptor);
        EXPECT_TRUE(pfn) << "failed to get function "
                << getFunctionDescriptorName(descriptor);
    }
}

/* TESTCASE: Tests that the HWC2 fails to retrieve and invalid function. */
TEST_F(Hwc2Test, GET_FUNCTION_invalid_function)
{
    hwc2_function_pointer_t pfn = getFunction(HWC2_FUNCTION_INVALID);
    EXPECT_FALSE(pfn) << "failed to get invalid function";
}

/* TESTCASE: Tests that the HWC2 does not return an invalid capability. */
TEST_F(Hwc2Test, GET_CAPABILITIES)
{
    std::vector<hwc2_capability_t> capabilities;

    getCapabilities(&capabilities);

    EXPECT_EQ(std::count(capabilities.begin(), capabilities.end(),
            HWC2_CAPABILITY_INVALID), 0);
}

static const std::array<hwc2_callback_descriptor_t, 3> callbackDescriptors = {{
    HWC2_CALLBACK_HOTPLUG,
    HWC2_CALLBACK_REFRESH,
    HWC2_CALLBACK_VSYNC,
}};

/* TESTCASE: Tests that the HWC2 can successfully register all required
 * callback functions. */
TEST_F(Hwc2Test, REGISTER_CALLBACK)
{
    hwc2_callback_data_t data = reinterpret_cast<hwc2_callback_data_t>(
            const_cast<char*>("data"));

    for (auto descriptor : callbackDescriptors) {
        ASSERT_NO_FATAL_FAILURE(registerCallback(descriptor, data,
                []() { return; }));
    }
}

/* TESTCASE: Test that the HWC2 fails to register invalid callbacks. */
TEST_F(Hwc2Test, REGISTER_CALLBACK_bad_parameter)
{
    hwc2_callback_data_t data = reinterpret_cast<hwc2_callback_data_t>(
            const_cast<char*>("data"));
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(registerCallback(HWC2_CALLBACK_INVALID, data,
            []() { return; }, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 can register a callback with null data. */
TEST_F(Hwc2Test, REGISTER_CALLBACK_null_data)
{
    hwc2_callback_data_t data = nullptr;

    for (auto descriptor : callbackDescriptors) {
        ASSERT_NO_FATAL_FAILURE(registerCallback(descriptor, data,
                []() { return; }));
    }
}

/* TESTCASE: Tests that the HWC2 returns the correct display type for each
 * physical display. */
TEST_F(Hwc2Test, GET_DISPLAY_TYPE)
{
    for (auto display : mDisplays) {
        hwc2_display_type_t type;

        ASSERT_NO_FATAL_FAILURE(getDisplayType(display, &type));
        EXPECT_EQ(type, HWC2_DISPLAY_TYPE_PHYSICAL) << "failed to return"
                " correct display type";
    }
}

/* TESTCASE: Tests that the HWC2 returns an error when the display type of a bad
 * display is requested. */
TEST_F(Hwc2Test, GET_DISPLAY_TYPE_bad_display)
{
    hwc2_display_t display;
    hwc2_display_type_t type;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(getDisplayType(display, &type, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 can create and destroy layers. */
TEST_F(Hwc2Test, CREATE_DESTROY_LAYER)
{
    for (auto display : mDisplays) {
        hwc2_layer_t layer;

        ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));
    }
}

/* TESTCASE: Tests that the HWC2 cannot create a layer for a bad display */
TEST_F(Hwc2Test, CREATE_LAYER_bad_display)
{
    hwc2_display_t display;
    hwc2_layer_t layer;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 will either support a large number of resources
 * or will return no resources. */
TEST_F(Hwc2Test, CREATE_LAYER_no_resources)
{
    const size_t layerCnt = 1000;

    for (auto display : mDisplays) {
        std::vector<hwc2_layer_t> layers;

        ASSERT_NO_FATAL_FAILURE(createLayers(display, &layers, layerCnt));

        ASSERT_NO_FATAL_FAILURE(destroyLayers(display, std::move(layers)));
    }
}

/* TESTCASE: Tests that the HWC2 cannot destroy a layer for a bad display */
TEST_F(Hwc2Test, DESTROY_LAYER_bad_display)
{
    hwc2_display_t badDisplay;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&badDisplay));

    for (auto display : mDisplays) {
        hwc2_layer_t layer = 0;
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(destroyLayer(badDisplay, layer, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(destroyLayer(badDisplay, layer, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));
    }
}

/* TESTCASE: Tests that the HWC2 cannot destory a bad layer */
TEST_F(Hwc2Test, DESTROY_LAYER_bad_layer)
{
    for (auto display : mDisplays) {
        hwc2_layer_t layer;
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, UINT64_MAX / 2, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, 0, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, UINT64_MAX - 1, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, 1, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, UINT64_MAX, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer + 1, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));

        ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

static const std::array<hwc2_attribute_t, 2> requiredAttributes = {{
    HWC2_ATTRIBUTE_WIDTH,
    HWC2_ATTRIBUTE_HEIGHT,
}};

static const std::array<hwc2_attribute_t, 3> optionalAttributes = {{
    HWC2_ATTRIBUTE_VSYNC_PERIOD,
    HWC2_ATTRIBUTE_DPI_X,
    HWC2_ATTRIBUTE_DPI_Y,
}};

/* TESTCASE: Tests that the HWC2 can return display attributes for a valid
 * config. */
TEST_F(Hwc2Test, GET_DISPLAY_ATTRIBUTE)
{
    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        for (auto config : configs) {
            int32_t value;

            for (auto attribute : requiredAttributes) {
                ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config,
                        attribute, &value));
                EXPECT_GE(value, 0) << "missing required attribute "
                        << getAttributeName(attribute) << " for config "
                        << config;
            }
            for (auto attribute : optionalAttributes) {
                ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config,
                        attribute, &value));
            }
        }
    }
}

/* TESTCASE: Tests that the HWC2 will return a value of -1 for an invalid
 * attribute */
TEST_F(Hwc2Test, GET_DISPLAY_ATTRIBUTE_invalid_attribute)
{
    const hwc2_attribute_t attribute = HWC2_ATTRIBUTE_INVALID;

    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        for (auto config : configs) {
            int32_t value;
            hwc2_error_t err = HWC2_ERROR_NONE;

            ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config,
                    attribute, &value, &err));
            EXPECT_EQ(value, -1) << "failed to return -1 for an invalid"
                    " attribute for config " << config;
        }
    }
}

/* TESTCASE: Tests that the HWC2 will fail to get attributes for a bad display */
TEST_F(Hwc2Test, GET_DISPLAY_ATTRIBUTE_bad_display)
{
    hwc2_display_t display;
    const hwc2_config_t config = 0;
    int32_t value;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    for (auto attribute : requiredAttributes) {
        ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config, attribute,
                &value, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
    }

    for (auto attribute : optionalAttributes) {
        ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config, attribute,
                &value, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
    }
}

/* TESTCASE: Tests that the HWC2 will fail to get attributes for a bad config */
TEST_F(Hwc2Test, GET_DISPLAY_ATTRIBUTE_bad_config)
{
    for (auto display : mDisplays) {
        hwc2_config_t config;
        int32_t value;
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(getInvalidConfig(display, &config));

        for (auto attribute : requiredAttributes) {
            ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config,
                    attribute, &value, &err));
            EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
        }

        for (auto attribute : optionalAttributes) {
            ASSERT_NO_FATAL_FAILURE(getDisplayAttribute(display, config,
                    attribute, &value, &err));
            EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
        }
    }
}

/* TESTCASE: Tests that the HWC2 will get display configs for active displays */
TEST_F(Hwc2Test, GET_DISPLAY_CONFIGS)
{
    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));
    }
}

/* TESTCASE: Tests that the HWC2 will not get display configs for bad displays */
TEST_F(Hwc2Test, GET_DISPLAY_CONFIGS_bad_display)
{
    hwc2_display_t display;
    std::vector<hwc2_config_t> configs;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs, &err));

    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
    EXPECT_TRUE(configs.empty()) << "returned configs for bad display";
}

/* TESTCASE: Tests that the HWC2 will return the same config list multiple
 * times in a row. */
TEST_F(Hwc2Test, GET_DISPLAY_CONFIGS_same)
{
    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs1, configs2;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs1));
        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs2));

        EXPECT_TRUE(std::is_permutation(configs1.begin(), configs1.end(),
                configs2.begin())) << "returned two different config sets";
    }
}

/* TESTCASE: Tests that the HWC2 does not return duplicate display configs */
TEST_F(Hwc2Test, GET_DISPLAY_CONFIGS_duplicate)
{
    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        std::unordered_set<hwc2_config_t> configsSet(configs.begin(),
                configs.end());
        EXPECT_EQ(configs.size(), configsSet.size()) << "returned duplicate"
                " configs";
    }
}

/* TESTCASE: Tests that the HWC2 returns the active config for a display */
TEST_F(Hwc2Test, GET_ACTIVE_CONFIG)
{
    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        for (auto config : configs) {
            hwc2_config_t activeConfig;

            ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config));
            ASSERT_NO_FATAL_FAILURE(getActiveConfig(display, &activeConfig));

            EXPECT_EQ(activeConfig, config) << "failed to get active config";
        }
    }
}

/* TESTCASE: Tests that the HWC2 does not return an active config for a bad
 * display. */
TEST_F(Hwc2Test, GET_ACTIVE_CONFIG_bad_display)
{
    hwc2_display_t display;
    hwc2_config_t activeConfig;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(getActiveConfig(display, &activeConfig, &err));

    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 either begins with a valid active config
 * or returns an error when getActiveConfig is called. */
TEST_F(Hwc2Test, GET_ACTIVE_CONFIG_bad_config)
{
    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;
        hwc2_config_t activeConfig;
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        if (configs.empty())
            return;

        ASSERT_NO_FATAL_FAILURE(getActiveConfig(display, &activeConfig, &err));
        if (err == HWC2_ERROR_NONE) {
            EXPECT_NE(std::count(configs.begin(), configs.end(),
                    activeConfig), 0) << "active config is not found in "
                    " configs for display";
        } else {
            EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
        }
    }
}

/* TESTCASE: Tests that the HWC2 can set every display config as an active
 * config */
TEST_F(Hwc2Test, SET_ACTIVE_CONFIG)
{
    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        for (auto config : configs) {
            EXPECT_NO_FATAL_FAILURE(setActiveConfig(display, config));
        }
    }
}

/* TESTCASE: Tests that the HWC2 cannot set an active config for a bad display */
TEST_F(Hwc2Test, SET_ACTIVE_CONFIG_bad_display)
{
    hwc2_display_t display;
    const hwc2_config_t config = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 cannot set an invalid active config */
TEST_F(Hwc2Test, SET_ACTIVE_CONFIG_bad_config)
{
    for (auto display : mDisplays) {
        hwc2_config_t config;
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(getInvalidConfig(display, &config));

        ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
    }
}

/* TESTCASE: Tests that the HWC2 returns a valid value for getDozeSupport. */
TEST_F(Hwc2Test, GET_DOZE_SUPPORT)
{
    for (auto display : mDisplays) {
        int32_t support = -1;

        ASSERT_NO_FATAL_FAILURE(getDozeSupport(display, &support));

        EXPECT_TRUE(support == 0 || support == 1) << "invalid doze support value";
    }
}

/* TESTCASE: Tests that the HWC2 cannot get doze support for a bad display. */
TEST_F(Hwc2Test, GET_DOZE_SUPPORT_bad_display)
{
    hwc2_display_t display;
    int32_t support = -1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(getDozeSupport(display, &support, &err));

    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 can set all supported power modes */
TEST_F(Hwc2Test, SET_POWER_MODE)
{
    for (auto display : mDisplays) {
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));

        int32_t support = -1;
        ASSERT_NO_FATAL_FAILURE(getDozeSupport(display, &support));
        if (support != 1)
            return;

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_DOZE));
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display,
                HWC2_POWER_MODE_DOZE_SUSPEND));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
    }
}

/* TESTCASE: Tests that the HWC2 cannot set a power mode for a bad display. */
TEST_F(Hwc2Test, SET_POWER_MODE_bad_display)
{
    hwc2_display_t display;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    int32_t support = -1;
    ASSERT_NO_FATAL_FAILURE(getDozeSupport(display, &support, &err));
    if (support != 1)
        return;

    ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_DOZE, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_DOZE_SUSPEND,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 cannot set an invalid power mode value. */
TEST_F(Hwc2Test, SET_POWER_MODE_bad_parameter)
{
    for (auto display : mDisplays) {
        hwc2_power_mode_t mode = static_cast<hwc2_power_mode_t>(
                HWC2_POWER_MODE_DOZE_SUSPEND + 1);
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, mode, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code "
                << mode;
    }
}

/* TESTCASE: Tests that the HWC2 will return unsupported if it does not support
 * an optional power mode. */
TEST_F(Hwc2Test, SET_POWER_MODE_unsupported)
{
    for (auto display : mDisplays) {
        int32_t support = -1;
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(getDozeSupport(display, &support, &err));
        if (support == 1)
            return;

        ASSERT_EQ(support, 0) << "invalid doze support value";

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_DOZE,
                &err));
        EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display,
                HWC2_POWER_MODE_DOZE_SUSPEND, &err));
        EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED) <<  "returned wrong error code";
    }
}

/* TESTCASE: Tests that the HWC2 can set the same power mode multiple times. */
TEST_F(Hwc2Test, SET_POWER_MODE_stress)
{
    for (auto display : mDisplays) {
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));

        int32_t support = -1;
        ASSERT_NO_FATAL_FAILURE(getDozeSupport(display, &support));
        if (support != 1)
            return;

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_DOZE));
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_DOZE));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display,
                HWC2_POWER_MODE_DOZE_SUSPEND));
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display,
                HWC2_POWER_MODE_DOZE_SUSPEND));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
    }
}

/* TESTCASE: Tests that the HWC2 can enable and disable vsync on active
 * displays */
TEST_F(Hwc2Test, SET_VSYNC_ENABLED)
{
    for (auto display : mDisplays) {
        hwc2_callback_data_t data = reinterpret_cast<hwc2_callback_data_t>(
                const_cast<char*>("data"));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));

        ASSERT_NO_FATAL_FAILURE(registerCallback(HWC2_CALLBACK_VSYNC, data,
                []() { return; }));

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_ENABLE));

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
    }
}

/* TESTCASE: Tests that the HWC2 issues a valid vsync callback. */
TEST_F(Hwc2Test, SET_VSYNC_ENABLED_callback)
{
    for (auto display : mDisplays) {
        hwc2_display_t receivedDisplay;
        int64_t receivedTimestamp;

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));

        ASSERT_NO_FATAL_FAILURE(enableVsync(display));

        ASSERT_NO_FATAL_FAILURE(waitForVsync(&receivedDisplay,
                &receivedTimestamp));

        EXPECT_EQ(receivedDisplay, display) << "failed to get correct display";
        EXPECT_GE(receivedTimestamp, 0) << "failed to get valid timestamp";

        ASSERT_NO_FATAL_FAILURE(disableVsync(display));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
    }
}

/* TESTCASE: Tests that the HWC2 cannot enable a vsync for a bad display */
TEST_F(Hwc2Test, SET_VSYNC_ENABLED_bad_display)
{
    hwc2_display_t display;
    hwc2_callback_data_t data = reinterpret_cast<hwc2_callback_data_t>(
            const_cast<char*>("data"));
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(registerCallback(HWC2_CALLBACK_VSYNC, data,
            []() { return; }));

    ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_ENABLE, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 cannot enable an invalid vsync value */
TEST_F(Hwc2Test, SET_VSYNC_ENABLED_bad_parameter)
{
    for (auto display : mDisplays) {
        hwc2_callback_data_t data = reinterpret_cast<hwc2_callback_data_t>(
                const_cast<char*>("data"));
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));

        ASSERT_NO_FATAL_FAILURE(registerCallback(HWC2_CALLBACK_VSYNC, data,
                []() { return; }));

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_INVALID,
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
    }
}

/* TESTCASE: Tests that the HWC2 can enable and disable a vsync value multiple
 * times. */
TEST_F(Hwc2Test, SET_VSYNC_ENABLED_stress)
{
    for (auto display : mDisplays) {
        hwc2_callback_data_t data = reinterpret_cast<hwc2_callback_data_t>(
                const_cast<char*>("data"));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));

        ASSERT_NO_FATAL_FAILURE(registerCallback(HWC2_CALLBACK_VSYNC, data,
                []() { return; }));

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE));

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_ENABLE));
        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_ENABLE));

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE));
        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
    }
}

/* TESTCASE: Tests that the HWC2 can set a vsync enable value when the display
 * is off and no callback is registered. */
TEST_F(Hwc2Test, SET_VSYNC_ENABLED_no_callback_no_power)
{
    const uint secs = 1;

    for (auto display : mDisplays) {
        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_ENABLE));

        sleep(secs);

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE));
    }
}

/* TESTCASE: Tests that the HWC2 can set a vsync enable value when no callback
 * is registered. */
TEST_F(Hwc2Test, SET_VSYNC_ENABLED_no_callback)
{
    const uint secs = 1;

    for (auto display : mDisplays) {
        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_ON));

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_ENABLE));

        sleep(secs);

        ASSERT_NO_FATAL_FAILURE(setVsyncEnabled(display, HWC2_VSYNC_DISABLE));

        ASSERT_NO_FATAL_FAILURE(setPowerMode(display, HWC2_POWER_MODE_OFF));
    }
}

/* TESTCASE: Tests that the HWC2 returns a display name for each display */
TEST_F(Hwc2Test, GET_DISPLAY_NAME)
{
    for (auto display : mDisplays) {
        std::string name;

        ASSERT_NO_FATAL_FAILURE(getDisplayName(display, &name));
    }
}

/* TESTCASE: Tests that the HWC2 does not return a display name for a bad
 * display */
TEST_F(Hwc2Test, GET_DISPLAY_NAME_bad_display)
{
    hwc2_display_t display;
    std::string name;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(getDisplayName(display, &name, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 can set basic composition types. */
TEST_F(Hwc2Test, SET_LAYER_COMPOSITION_TYPE)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setComposition, advanceComposition));
}

/* TESTCASE: Tests that the HWC2 can update a basic composition type on a
 * layer. */
TEST_F(Hwc2Test, SET_LAYER_COMPOSITION_TYPE_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setComposition, advanceComposition));
}

/* TESTCASE: Tests that the HWC2 cannot set a composition type for a bad layer */
TEST_F(Hwc2Test, SET_LAYER_COMPOSITION_TYPE_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setComposition));
}

/* TESTCASE: Tests that the HWC2 cannot set a bad composition type */
TEST_F(Hwc2Test, SET_LAYER_COMPOSITION_TYPE_bad_parameter)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadParameter(
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
                    hwc2_error_t* outErr) {

                ASSERT_NO_FATAL_FAILURE(test->setLayerCompositionType(display,
                        layer, HWC2_COMPOSITION_INVALID, outErr));
            }
    ));
}

/* TESTCASE: Tests that the HWC2 can set the cursor position of a layer. */
TEST_F(Hwc2Test, SET_CURSOR_POSITION)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            ::setCursorPosition, advanceCursorPosition));
}

/* TESTCASE: Tests that the HWC2 can update the cursor position of a layer. */
TEST_F(Hwc2Test, SET_CURSOR_POSITION_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            ::setCursorPosition, advanceCursorPosition));
}

/* TESTCASE: Tests that the HWC2 can set the cursor position of a layer when the
 * composition type has not been set to HWC2_COMPOSITION_CURSOR. */
TEST_F(Hwc2Test, SET_CURSOR_POSITION_composition_type_unset)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
                    Hwc2TestLayer* testLayer, hwc2_error_t* outErr) {

                const hwc_rect_t cursorPosition = testLayer->getCursorPosition();
                EXPECT_NO_FATAL_FAILURE(test->setCursorPosition(display, layer,
                        cursorPosition.left, cursorPosition.top, outErr));
            },

            advanceCursorPosition));
}

/* TESTCASE: Tests that the HWC2 cannot set the cursor position of a bad
 * display. */
TEST_F(Hwc2Test, SET_CURSOR_POSITION_bad_display)
{
    hwc2_display_t display;
    hwc2_layer_t layer = 0;
    int32_t x = 0, y = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(getBadDisplay(&display));

    ASSERT_NO_FATAL_FAILURE(setCursorPosition(display, layer, x, y, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

/* TESTCASE: Tests that the HWC2 cannot set the cursor position of a bad layer. */
TEST_F(Hwc2Test, SET_CURSOR_POSITION_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t badLayer,
                    Hwc2TestLayer* testLayer, hwc2_error_t* outErr) {

                const hwc_rect_t cursorPosition = testLayer->getCursorPosition();
                EXPECT_NO_FATAL_FAILURE(test->setCursorPosition(display,
                        badLayer, cursorPosition.left, cursorPosition.top,
                        outErr));
            }
   ));
}

/* TESTCASE: Tests that the HWC2 can set a blend mode value of a layer. */
TEST_F(Hwc2Test, SET_LAYER_BLEND_MODE)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setBlendMode, advanceBlendMode));
}

/* TESTCASE: Tests that the HWC2 can update a blend mode value of a layer. */
TEST_F(Hwc2Test, SET_LAYER_BLEND_MODE_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setBlendMode, advanceBlendMode));
}

/* TESTCASE: Tests that the HWC2 cannot set a blend mode for a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_BLEND_MODE_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setBlendMode));
}

/* TESTCASE: Tests that the HWC2 cannot set an invalid blend mode. */
TEST_F(Hwc2Test, SET_LAYER_BLEND_MODE_bad_parameter)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadParameter(
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
                    hwc2_error_t* outErr) {

                ASSERT_NO_FATAL_FAILURE(test->setLayerBlendMode(display,
                        layer, HWC2_BLEND_MODE_INVALID, outErr));
            }
    ));
}

/* TESTCASE: Tests that the HWC2 can set the buffer of a layer. */
TEST_F(Hwc2Test, SET_LAYER_BUFFER)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setBuffer, advanceBuffer));
}

/* TESTCASE: Tests that the HWC2 can update the buffer of a layer. */
TEST_F(Hwc2Test, SET_LAYER_BUFFER_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setBuffer, advanceBuffer));
}

/* TESTCASE: Tests that the HWC2 cannot set the buffer of a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_BUFFER_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t badLayer,
                    Hwc2TestLayer* testLayer, hwc2_error_t* outErr) {

                buffer_handle_t handle = nullptr;
                android::base::unique_fd acquireFence;

                /* If there is not available buffer for the given buffer
                 * properties, it should not fail this test case */
                if (testLayer->getBuffer(&handle, &acquireFence) == 0) {
                    *outErr = HWC2_ERROR_BAD_LAYER;
                    return;
                }

                ASSERT_NO_FATAL_FAILURE(test->setLayerBuffer(display, badLayer,
                        handle, acquireFence, outErr));
            }
    ));
}

/* TESTCASE: Tests that the HWC2 can set an invalid buffer for a layer. */
TEST_F(Hwc2Test, SET_LAYER_BUFFER_bad_parameter)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadParameter(
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
                    hwc2_error_t* outErr) {

                buffer_handle_t handle = nullptr;
                int32_t acquireFence = -1;

                ASSERT_NO_FATAL_FAILURE(test->setLayerBuffer(display, layer,
                        handle, acquireFence, outErr));
            }
    ));
}

/* TESTCASE: Tests that the HWC2 can set the color of a layer. */
TEST_F(Hwc2Test, SET_LAYER_COLOR)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setColor, advanceColor));
}

/* TESTCASE: Tests that the HWC2 can update the color of a layer. */
TEST_F(Hwc2Test, SET_LAYER_COLOR_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setColor, advanceColor));
}

/* TESTCASE: Tests that the HWC2 can set the color of a layer when the
 * composition type has not been set to HWC2_COMPOSITION_SOLID_COLOR. */
TEST_F(Hwc2Test, SET_LAYER_COLOR_composition_type_unset)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Basic,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
                    Hwc2TestLayer* testLayer, hwc2_error_t* outErr) {

                EXPECT_NO_FATAL_FAILURE(test->setLayerColor(display, layer,
                        testLayer->getColor(), outErr));
            },

            advanceColor));
}

/* TESTCASE: Tests that the HWC2 cannot set the color of a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_COLOR_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t badLayer,
                    Hwc2TestLayer* testLayer, hwc2_error_t* outErr) {

                EXPECT_NO_FATAL_FAILURE(test->setLayerColor(display, badLayer,
                        testLayer->getColor(), outErr));
            }
    ));
}

/* TESTCASE: Tests that the HWC2 can set the dataspace of a layer. */
TEST_F(Hwc2Test, SET_LAYER_DATASPACE)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setDataspace, advanceDataspace));
}

/* TESTCASE: Tests that the HWC2 can update the dataspace of a layer. */
TEST_F(Hwc2Test, SET_LAYER_DATASPACE_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setDataspace, advanceDataspace));
}

/* TESTCASE: Tests that the HWC2 cannot set a dataspace for a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_DATASPACE_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setDataspace));
}

/* TESTCASE: Tests that the HWC2 can set the display frame of a layer. */
TEST_F(Hwc2Test, SET_LAYER_DISPLAY_FRAME)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setDisplayFrame, advanceDisplayFrame));
}

/* TESTCASE: Tests that the HWC2 can update the display frame of a layer. */
TEST_F(Hwc2Test, SET_LAYER_DISPLAY_FRAME_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setDisplayFrame, advanceDisplayFrame));
}

/* TESTCASE: Tests that the HWC2 cannot set the display frame of a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_DISPLAY_FRAME_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setDisplayFrame));
}

/* TESTCASE: Tests that the HWC2 can set the plane alpha of a layer. */
TEST_F(Hwc2Test, SET_LAYER_PLANE_ALPHA)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setPlaneAlpha, advancePlaneAlpha));
}

/* TESTCASE: Tests that the HWC2 can update the plane alpha of a layer. */
TEST_F(Hwc2Test, SET_LAYER_PLANE_ALPHA_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setPlaneAlpha, advancePlaneAlpha));
}

/* TESTCASE: Tests that the HWC2 cannot set a plane alpha for a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_PLANE_ALPHA_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t badLayer,
                    Hwc2TestLayer* testLayer, hwc2_error_t *outErr) {

                    EXPECT_NO_FATAL_FAILURE(test->setLayerPlaneAlpha(display,
                            badLayer, testLayer->getPlaneAlpha(), outErr));
            }
    ));
}

/* TESTCASE: Tests that the HWC2 can set the source crop of a layer. */
TEST_F(Hwc2Test, SET_LAYER_SOURCE_CROP)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setSourceCrop, advanceSourceCrop));
}

/* TESTCASE: Tests that the HWC2 can update the source crop of a layer. */
TEST_F(Hwc2Test, SET_LAYER_SOURCE_CROP_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setSourceCrop, advanceSourceCrop));
}

/* TESTCASE: Tests that the HWC2 cannot set the source crop of a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_SOURCE_CROP_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setSourceCrop));
}

/* TESTCASE: Tests that the HWC2 can set the surface damage of a layer. */
TEST_F(Hwc2Test, SET_LAYER_SURFACE_DAMAGE)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setSurfaceDamage, advanceSurfaceDamage));
}

/* TESTCASE: Tests that the HWC2 can update the surface damage of a layer. */
TEST_F(Hwc2Test, SET_LAYER_SURFACE_DAMAGE_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setSurfaceDamage, advanceSurfaceDamage));
}

/* TESTCASE: Tests that the HWC2 cannot set the surface damage of a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_SURFACE_DAMAGE_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setSurfaceDamage));
}

/* TESTCASE: Tests that the HWC2 can set the transform value of a layer. */
TEST_F(Hwc2Test, SET_LAYER_TRANSFORM)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperty(Hwc2TestCoverage::Complete,
            setTransform, advanceTransform));
}

/* TESTCASE: Tests that the HWC2 can update the transform value of a layer. */
TEST_F(Hwc2Test, SET_LAYER_TRANSFORM_update)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyUpdate(Hwc2TestCoverage::Complete,
            setTransform, advanceTransform));
}

/* TESTCASE: Tests that the HWC2 cannot set the transform for a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_TRANSFORM_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setTransform));
}

/* TESTCASE: Tests that the HWC2 can set the visible region of a layer. */
TEST_F(Hwc2Test, SET_LAYER_VISIBLE_REGION)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperties(Hwc2TestCoverage::Basic, 5,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
                    Hwc2TestLayers* testLayers) {

                EXPECT_NO_FATAL_FAILURE(test->setLayerVisibleRegion(display,
                        layer, testLayers->getVisibleRegion(layer)));
            },

            advanceVisibleRegions));
}

/* TESTCASE: Tests that the HWC2 cannot set the visible region of a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_VISIBLE_REGION_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setVisibleRegion));
}

/* TESTCASE: Tests that the HWC2 can set the z order of a layer. */
TEST_F(Hwc2Test, SET_LAYER_Z_ORDER)
{
    ASSERT_NO_FATAL_FAILURE(setLayerProperties(Hwc2TestCoverage::Complete, 10,
            [] (Hwc2Test* test, hwc2_display_t display, hwc2_layer_t layer,
                    Hwc2TestLayers* testLayers) {

                EXPECT_NO_FATAL_FAILURE(test->setLayerZOrder(display, layer,
                        testLayers->getZOrder(layer)));
            },

            /* TestLayer z orders are set during the construction of TestLayers
             * and cannot be updated. There is no need (or ability) to cycle
             * through additional z order configurations. */
            [] (Hwc2TestLayers* /*testLayers*/) {
                return false;
            }
    ));
}

/* TESTCASE: Tests that the HWC2 can update the z order of a layer. */
TEST_F(Hwc2Test, SET_LAYER_Z_ORDER_update)
{
    const std::vector<uint32_t> zOrders = { static_cast<uint32_t>(0),
            static_cast<uint32_t>(1), static_cast<uint32_t>(UINT32_MAX / 4),
            static_cast<uint32_t>(UINT32_MAX / 2),
            static_cast<uint32_t>(UINT32_MAX) };

    for (auto display : mDisplays) {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(getDisplayConfigs(display, &configs));

        for (auto config : configs) {
            hwc2_layer_t layer;

            ASSERT_NO_FATAL_FAILURE(setActiveConfig(display, config));

            ASSERT_NO_FATAL_FAILURE(createLayer(display, &layer));

            for (uint32_t zOrder : zOrders) {
                EXPECT_NO_FATAL_FAILURE(setLayerZOrder(display, layer, zOrder));
            }

            ASSERT_NO_FATAL_FAILURE(destroyLayer(display, layer));
        }
    }
}

/* TESTCASE: Tests that the HWC2 cannot set the z order of a bad layer. */
TEST_F(Hwc2Test, SET_LAYER_Z_ORDER_bad_layer)
{
    ASSERT_NO_FATAL_FAILURE(setLayerPropertyBadLayer(Hwc2TestCoverage::Default,
            setZOrder));
}
