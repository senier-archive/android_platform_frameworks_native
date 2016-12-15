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
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <android-base/unique_fd.h>
#include <hardware/hardware.h>
#include <sync/sync.h>

#define HWC2_INCLUDE_STRINGIFICATION
#define HWC2_USE_CPP11
#include <hardware/hwcomposer2.h>
#undef HWC2_INCLUDE_STRINGIFICATION
#undef HWC2_USE_CPP11

#include "hwc2_test_layer.h"
#include "hwc2_test_layers.h"
#include "hwc2_test_client_target.h"
#include "hwc2_test_virtual_display.h"

void hwc2_test_vsync_callback(hwc2_callback_data_t callback_data,
        hwc2_display_t display, int64_t timestamp);

class hwc2_test : public testing::Test {
public:
    hwc2_test()
        : hwc2_device(nullptr),
          layers(),
          active_displays(),
          virtual_displays(),
          vsync_mutex(),
          vsync_cv(),
          vsync_display(),
          vsync_timestamp(-1) { }

    virtual void SetUp()
    {
        hw_module_t const *hwc2_module;

        int err = hw_get_module(HWC_HARDWARE_MODULE_ID, &hwc2_module);
        ASSERT_GE(err, 0) << "failed to get hwc hardware module: "
                << strerror(-err);

        /* The following method will fail if you have not run "adb shell stop" */
        err = hwc2_open(hwc2_module, &hwc2_device);
        ASSERT_GE(err, 0) << "failed to open hwc hardware module: "
                << strerror(-err);
    }

    virtual void TearDown()
    {
        hwc2_display_t display;
        hwc2_layer_t layer;

        for (auto itr = layers.begin(); itr != layers.end();) {
            display = itr->first;
            layer = itr->second;
            itr++;
            destroy_layer(display, layer);
        }

        for (auto itr = active_displays.begin(); itr != active_displays.end();) {
            display = *itr;
            itr++;
            set_power_mode(display, HWC2_POWER_MODE_OFF);
        }

        if (hwc2_device)
            hwc2_close(hwc2_device);
    }

    void register_callback(hwc2_callback_descriptor_t descriptor,
            hwc2_callback_data_t callback_data, hwc2_function_pointer_t pointer,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_REGISTER_CALLBACK pfn = (HWC2_PFN_REGISTER_CALLBACK)
                get_function(HWC2_FUNCTION_REGISTER_CALLBACK);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, descriptor, callback_data,
                pointer);
    }

    void register_callback(hwc2_callback_descriptor_t descriptor,
            hwc2_callback_data_t callback_data, hwc2_function_pointer_t pointer)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(register_callback(descriptor, callback_data,
                pointer, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to register callback";
    }

    void get_display_type(hwc2_display_t display, hwc2_display_type_t *out_type,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_DISPLAY_TYPE pfn = (HWC2_PFN_GET_DISPLAY_TYPE)
                get_function(HWC2_FUNCTION_GET_DISPLAY_TYPE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, (int32_t *) out_type);
    }

    void get_display_type(hwc2_display_t display, hwc2_display_type_t *out_type)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_display_type(display, out_type, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get display type";
    }

    void create_layer(hwc2_display_t display, hwc2_layer_t *out_layer,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_CREATE_LAYER pfn = (HWC2_PFN_CREATE_LAYER)
                get_function(HWC2_FUNCTION_CREATE_LAYER);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, out_layer);

        if (*out_err == HWC2_ERROR_NONE)
            layers.insert(std::make_pair(display, *out_layer));
    }

    void create_layer(hwc2_display_t display, hwc2_layer_t *out_layer)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(create_layer(display, out_layer, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to create layer";
    }

    void destroy_layer(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_DESTROY_LAYER pfn = (HWC2_PFN_DESTROY_LAYER)
                get_function(HWC2_FUNCTION_DESTROY_LAYER);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer);

        if (*out_err == HWC2_ERROR_NONE)
            layers.erase(std::make_pair(display, layer));
    }

    void destroy_layer(hwc2_display_t display, hwc2_layer_t layer)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to destroy layer " << layer;
    }

    void get_display_attribute(hwc2_display_t display, hwc2_config_t config,
            hwc2_attribute_t attribute, int32_t *out_value,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_DISPLAY_ATTRIBUTE pfn = (HWC2_PFN_GET_DISPLAY_ATTRIBUTE)
                get_function(HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, config, attribute,
                out_value);
    }

    void get_display_attribute(hwc2_display_t display, hwc2_config_t config,
            hwc2_attribute_t attribute, int32_t *out_value)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                attribute, out_value, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get display attribute "
                << getAttributeName(attribute) << " for config " << config;
    }

    void get_display_configs(hwc2_display_t display,
            std::vector<hwc2_config_t> *out_configs, hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_DISPLAY_CONFIGS pfn = (HWC2_PFN_GET_DISPLAY_CONFIGS)
                get_function(HWC2_FUNCTION_GET_DISPLAY_CONFIGS);
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t num_configs = 0;

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_configs,
                nullptr);
        if (*out_err != HWC2_ERROR_NONE)
            return;

        out_configs->resize(num_configs);

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_configs,
                out_configs->data());
    }

    void get_display_configs(hwc2_display_t display,
            std::vector<hwc2_config_t> *out_configs)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_display_configs(display, out_configs, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get configs for display "
                << display;
    }

    void get_active_config(hwc2_display_t display, hwc2_config_t *out_config,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_ACTIVE_CONFIG pfn = (HWC2_PFN_GET_ACTIVE_CONFIG)
                get_function(HWC2_FUNCTION_GET_ACTIVE_CONFIG);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, out_config);
    }

    void get_active_config(hwc2_display_t display, hwc2_config_t *out_config)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_active_config(display, out_config, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get active config on"
                " display " << display;
    }

    void set_active_config(hwc2_display_t display, hwc2_config_t config,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_ACTIVE_CONFIG pfn = (HWC2_PFN_SET_ACTIVE_CONFIG)
                get_function(HWC2_FUNCTION_SET_ACTIVE_CONFIG);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, config);
    }

    void set_active_config(hwc2_display_t display, hwc2_config_t config)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set active config "
                << config;
    }

    void get_doze_support(hwc2_display_t display, int32_t *out_support,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_DOZE_SUPPORT pfn = (HWC2_PFN_GET_DOZE_SUPPORT)
                get_function(HWC2_FUNCTION_GET_DOZE_SUPPORT);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, out_support);
    }

    void get_doze_support(hwc2_display_t display, int32_t *out_support)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_doze_support(display, out_support, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get doze support on"
                " display " << display;
    }

    void set_power_mode(hwc2_display_t display, hwc2_power_mode_t mode,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_POWER_MODE pfn = (HWC2_PFN_SET_POWER_MODE)
                get_function(HWC2_FUNCTION_SET_POWER_MODE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, mode);

        if (*out_err != HWC2_ERROR_NONE)
            return;

        if (mode == HWC2_POWER_MODE_OFF)
            active_displays.erase(display);
        else
            active_displays.insert(display);
    }

    void set_power_mode(hwc2_display_t display, hwc2_power_mode_t mode)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_power_mode(display, mode, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set power mode "
                << getPowerModeName(mode) << " on display " << display;
    }

    void set_vsync_enabled(hwc2_display_t display, hwc2_vsync_t enabled,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_VSYNC_ENABLED pfn = (HWC2_PFN_SET_VSYNC_ENABLED)
                get_function(HWC2_FUNCTION_SET_VSYNC_ENABLED);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, enabled);
    }

    void set_vsync_enabled(hwc2_display_t display, hwc2_vsync_t enabled)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, enabled, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set vsync enabled "
                << getVsyncName(enabled);
    }

    void vsync_callback(hwc2_display_t display, int64_t timestamp)
    {
        std::lock_guard<std::mutex::mutex> lock(vsync_mutex);
        vsync_display = display;
        vsync_timestamp = timestamp;
        vsync_cv.notify_all();
    }

    void get_display_name(hwc2_display_t display, std::string *out_name,
                hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_DISPLAY_NAME pfn = (HWC2_PFN_GET_DISPLAY_NAME)
                get_function(HWC2_FUNCTION_GET_DISPLAY_NAME);
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t size = 0;

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &size, nullptr);
        if (*out_err != HWC2_ERROR_NONE)
            return;

        ASSERT_GE(size, (uint32_t) 0) << "failed to get valid display name";

        char *name = new char[size];

        ASSERT_NE(name, nullptr) << "failed to allocate the memory for name";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &size, name);

        out_name->assign(name);
        delete[] name;
    }

    void get_display_name(hwc2_display_t display, std::string *out_name)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_display_name(display, out_name, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get display name for "
                << display;
    }

    void set_layer_composition_type(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_composition_t composition, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_COMPOSITION_TYPE pfn =
                (HWC2_PFN_SET_LAYER_COMPOSITION_TYPE)
                get_function(HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, composition);
    }

    void set_layer_composition_type(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_composition_t composition)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                composition, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer composition"
                " type " << getCompositionName(composition);
    }

    void set_cursor_position(hwc2_display_t display, hwc2_layer_t layer,
            int32_t x, int32_t y, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_CURSOR_POSITION pfn = (HWC2_PFN_SET_CURSOR_POSITION)
                get_function(HWC2_FUNCTION_SET_CURSOR_POSITION);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, x, y);
    }

    void set_cursor_position(hwc2_display_t display, hwc2_layer_t layer,
            int32_t x, int32_t y)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_cursor_position(display, layer, x, y,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set cursor position";
    }

    void set_layer_blend_mode(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_blend_mode_t mode, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_BLEND_MODE pfn = (HWC2_PFN_SET_LAYER_BLEND_MODE)
                get_function(HWC2_FUNCTION_SET_LAYER_BLEND_MODE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, mode);
    }

    void set_layer_blend_mode(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_blend_mode_t mode)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer, mode,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer blend mode "
                << getBlendModeName(mode);
    }

    void set_layer_buffer(hwc2_display_t display, hwc2_layer_t layer,
            buffer_handle_t buffer, int32_t acquire_fence,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_BUFFER pfn = (HWC2_PFN_SET_LAYER_BUFFER)
                get_function(HWC2_FUNCTION_SET_LAYER_BUFFER);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, buffer,
                acquire_fence);
    }

    void set_layer_buffer(hwc2_display_t display, hwc2_layer_t layer,
            buffer_handle_t buffer, int32_t acquire_fence)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_buffer(display, layer, buffer,
                acquire_fence, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer buffer";
    }

    void set_layer_color(hwc2_display_t display, hwc2_layer_t layer,
            hwc_color_t color, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_COLOR pfn = (HWC2_PFN_SET_LAYER_COLOR)
                get_function(HWC2_FUNCTION_SET_LAYER_COLOR);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, color);
    }

    void set_layer_color(hwc2_display_t display, hwc2_layer_t layer,
            hwc_color_t color)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_color(display, layer, color, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer color";
    }

    void set_layer_dataspace(hwc2_display_t display, hwc2_layer_t layer,
            android_dataspace_t dataspace, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_DATASPACE pfn = (HWC2_PFN_SET_LAYER_DATASPACE)
                get_function(HWC2_FUNCTION_SET_LAYER_DATASPACE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, dataspace);
    }

    void set_layer_dataspace(hwc2_display_t display, hwc2_layer_t layer,
            android_dataspace_t dataspace)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_dataspace(display, layer, dataspace,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer dataspace";
    }

    void set_layer_display_frame(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_rect_t &display_frame, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_DISPLAY_FRAME pfn =
                (HWC2_PFN_SET_LAYER_DISPLAY_FRAME)
                get_function(HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer,
                display_frame);
    }

    void set_layer_display_frame(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_rect_t &display_frame)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_display_frame(display, layer,
                display_frame, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer display frame";
    }

    void set_layer_plane_alpha(hwc2_display_t display, hwc2_layer_t layer,
            float alpha, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_PLANE_ALPHA pfn = (HWC2_PFN_SET_LAYER_PLANE_ALPHA)
                get_function(HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, alpha);
    }

    void set_layer_plane_alpha(hwc2_display_t display, hwc2_layer_t layer,
            float alpha)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_plane_alpha(display, layer,
                alpha, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer plane alpha "
                << alpha;
    }

    void set_layer_source_crop(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_frect_t &source_crop, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_SOURCE_CROP pfn =
                (HWC2_PFN_SET_LAYER_SOURCE_CROP)
                get_function(HWC2_FUNCTION_SET_LAYER_SOURCE_CROP);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, source_crop);
    }

    void set_layer_source_crop(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_frect_t &source_crop)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_source_crop(display, layer,
                source_crop, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer source crop";
    }

    void set_layer_surface_damage(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_region_t &surface_damage, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_SURFACE_DAMAGE pfn =
                (HWC2_PFN_SET_LAYER_SURFACE_DAMAGE)
                get_function(HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer,
                surface_damage);
    }

    void set_layer_surface_damage(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_region_t &surface_damage)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_surface_damage(display, layer,
                surface_damage, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer surface damage";
    }

    void set_layer_transform(hwc2_display_t display, hwc2_layer_t layer,
            hwc_transform_t transform, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_TRANSFORM pfn = (HWC2_PFN_SET_LAYER_TRANSFORM)
                get_function(HWC2_FUNCTION_SET_LAYER_TRANSFORM);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, transform);
    }

    void set_layer_transform(hwc2_display_t display, hwc2_layer_t layer,
            hwc_transform_t transform)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_transform(display, layer, transform,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer transform "
                << getTransformName(transform);
    }

    void set_layer_visible_region(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_region_t &visible_region, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_VISIBLE_REGION pfn =
                (HWC2_PFN_SET_LAYER_VISIBLE_REGION)
                get_function(HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer,
                visible_region);
    }

    void set_layer_visible_region(hwc2_display_t display, hwc2_layer_t layer,
            const hwc_region_t &visible_region)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_visible_region(display, layer,
                visible_region, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer visible region";
    }

    void set_layer_z_order(hwc2_display_t display, hwc2_layer_t layer,
            uint32_t z_order, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_LAYER_Z_ORDER pfn = (HWC2_PFN_SET_LAYER_Z_ORDER)
                get_function(HWC2_FUNCTION_SET_LAYER_Z_ORDER);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, layer, z_order);
    }

    void set_layer_z_order(hwc2_display_t display, hwc2_layer_t layer,
            uint32_t z_order)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_layer_z_order(display, layer, z_order,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set layer z order "
                << z_order;
    }

    void validate_display(hwc2_display_t display, uint32_t *out_num_types,
            uint32_t *out_num_requests, hwc2_error_t *out_err)
    {
        HWC2_PFN_VALIDATE_DISPLAY pfn = (HWC2_PFN_VALIDATE_DISPLAY)
                get_function(HWC2_FUNCTION_VALIDATE_DISPLAY);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, out_num_types,
                out_num_requests);
    }

    void validate_display(hwc2_display_t display, uint32_t *out_num_types,
            uint32_t *out_num_requests, bool *out_has_changes)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;

        ASSERT_NO_FATAL_FAILURE(validate_display(display, out_num_types,
                out_num_requests, &err));

        if (err != HWC2_ERROR_HAS_CHANGES) {
            EXPECT_EQ(err, HWC2_ERROR_NONE) << "failed to validate display";
            *out_has_changes = false;
        } else
            *out_has_changes = true;
    }

    void get_display_requests(hwc2_display_t display,
            hwc2_display_request_t *out_display_requests,
            std::vector<hwc2_layer_t> *out_layers,
            std::vector<hwc2_layer_request_t> *out_layer_requests,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_DISPLAY_REQUESTS pfn = (HWC2_PFN_GET_DISPLAY_REQUESTS)
                get_function(HWC2_FUNCTION_GET_DISPLAY_REQUESTS);
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t num_elements = 0;

        *out_err = (hwc2_error_t) pfn(hwc2_device, display,
                (int32_t *) out_display_requests, &num_elements, nullptr,
                nullptr);
        if (*out_err != HWC2_ERROR_NONE || num_elements == 0)
            return;

        out_layers->resize(num_elements);
        out_layer_requests->resize(num_elements);

        *out_err = (hwc2_error_t) pfn(hwc2_device, display,
                (int32_t *) out_display_requests, &num_elements,
                (uint64_t *) out_layers->data(),
                (int32_t *) out_layer_requests->data());
    }

    void get_display_requests(hwc2_display_t display,
            hwc2_display_request_t *out_display_requests,
            std::vector<hwc2_layer_t> *out_layers,
            std::vector<hwc2_layer_request_t> *out_layer_requests)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_display_requests(display,
                out_display_requests, out_layers, out_layer_requests, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get display requests";
    }

    void get_changed_composition_types(hwc2_display_t display,
            std::vector<hwc2_layer_t> *out_layers,
            std::vector<hwc2_composition_t> *out_types, hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_CHANGED_COMPOSITION_TYPES pfn =
                (HWC2_PFN_GET_CHANGED_COMPOSITION_TYPES) get_function(
                HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES);
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t num_elements = 0;

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_elements,
                nullptr, nullptr);
        if (*out_err != HWC2_ERROR_NONE || num_elements == 0)
            return;

        out_layers->resize(num_elements);
        out_types->resize(num_elements);

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_elements,
                (uint64_t *) out_layers->data(), (int32_t *) out_types->data());
    }

    void get_changed_composition_types(hwc2_display_t display,
            std::vector<hwc2_layer_t> *out_layers,
            std::vector<hwc2_composition_t> *out_types)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_changed_composition_types(display,
                out_layers, out_types, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get changed composition"
                " types";
    }

    void accept_display_changes(hwc2_display_t display, hwc2_error_t *out_err)
    {
        HWC2_PFN_ACCEPT_DISPLAY_CHANGES pfn = (HWC2_PFN_ACCEPT_DISPLAY_CHANGES)
                get_function(HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display);
    }

    void accept_display_changes(hwc2_display_t display)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(accept_display_changes(display, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to accept display changes";
    }

    void get_client_target_support(hwc2_display_t display, int32_t width,
            int32_t height, android_pixel_format_t format,
            android_dataspace_t dataspace, hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_CLIENT_TARGET_SUPPORT pfn =
                (HWC2_PFN_GET_CLIENT_TARGET_SUPPORT) get_function(
                HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, width, height,
                format, dataspace);
    }

    void get_client_target_support(hwc2_display_t display, int32_t width,
            int32_t height, android_pixel_format_t format,
            android_dataspace_t dataspace)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_client_target_support(display, width,
                height, format, dataspace, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get client target support";
    }

    void set_client_target(hwc2_display_t display, buffer_handle_t handle,
            int32_t acquire_fence, android_dataspace_t dataspace,
            hwc_region_t damage, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_CLIENT_TARGET pfn = (HWC2_PFN_SET_CLIENT_TARGET)
                get_function(HWC2_FUNCTION_SET_CLIENT_TARGET);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, handle,
                acquire_fence, dataspace, damage);
    }

    void set_client_target(hwc2_display_t display, buffer_handle_t handle,
            int32_t acquire_fence, android_dataspace_t dataspace,
            hwc_region_t damage)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_client_target(display, handle,
                acquire_fence, dataspace, damage, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set client target";
    }

    void present_display(hwc2_display_t display, int32_t *out_present_fence,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_PRESENT_DISPLAY pfn = (HWC2_PFN_PRESENT_DISPLAY)
                get_function(HWC2_FUNCTION_PRESENT_DISPLAY);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, out_present_fence);
    }

    void present_display(hwc2_display_t display, int32_t *out_present_fence)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(present_display(display, out_present_fence,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to present display";
    }

    void get_release_fences(hwc2_display_t display,
            std::vector<hwc2_layer_t> *out_layers,
            std::vector<int32_t> *out_fences, hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_RELEASE_FENCES pfn = (HWC2_PFN_GET_RELEASE_FENCES)
                get_function(HWC2_FUNCTION_GET_RELEASE_FENCES);
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t num_elements = 0;

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_elements,
                nullptr, nullptr);
        if (*out_err != HWC2_ERROR_NONE)
            return;

        out_layers->resize(num_elements);
        out_fences->resize(num_elements);

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_elements,
                out_layers->data(), out_fences->data());
    }

    void get_release_fences(hwc2_display_t display,
            std::vector<hwc2_layer_t> *out_layers,
            std::vector<int32_t> *out_fences)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_release_fences(display, out_layers,
                out_fences, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to present display";
    }

    void get_color_modes(hwc2_display_t display,
            std::vector<android_color_mode_t> *out_color_modes,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_COLOR_MODES pfn = (HWC2_PFN_GET_COLOR_MODES)
                get_function(HWC2_FUNCTION_GET_COLOR_MODES);
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t num_color_modes = 0;

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_color_modes,
                nullptr);
        if (*out_err != HWC2_ERROR_NONE)
            return;

        out_color_modes->resize(num_color_modes);

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_color_modes,
                (int32_t *) out_color_modes->data());
    }

    void get_color_modes(hwc2_display_t display,
            std::vector<android_color_mode_t> *out_color_modes)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_color_modes(display, out_color_modes,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get color modes for"
                " display " << display;
    }

    void set_color_mode(hwc2_display_t display, android_color_mode_t color_mode,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_COLOR_MODE pfn = (HWC2_PFN_SET_COLOR_MODE)
                get_function(HWC2_FUNCTION_SET_COLOR_MODE);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display,
                (int32_t) color_mode);
    }

    void set_color_mode(hwc2_display_t display, android_color_mode_t color_mode)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_color_mode(display, color_mode, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set color mode "
                << color_mode;
    }

    void get_hdr_capabilities(hwc2_display_t display,
            std::vector<android_hdr_t> *out_types, float *out_max_luminance,
            float *out_max_average_luminance, float *out_min_luminance,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_GET_HDR_CAPABILITIES pfn = (HWC2_PFN_GET_HDR_CAPABILITIES)
                get_function(HWC2_FUNCTION_GET_HDR_CAPABILITIES);
        ASSERT_TRUE(pfn) << "failed to get function";

        uint32_t num_types = 0;

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_types,
                nullptr, out_max_luminance, out_max_average_luminance,
                out_min_luminance);
        if (*out_err != HWC2_ERROR_NONE)
            return;

        out_types->resize(num_types);

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, &num_types,
                (int32_t *) out_types->data(), out_max_luminance,
                out_max_average_luminance, out_min_luminance);
    }

    void get_hdr_capabilities(hwc2_display_t display,
            std::vector<android_hdr_t> *out_types, float *out_max_luminance,
            float *out_max_average_luminance, float *out_min_luminance)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(get_hdr_capabilities(display, out_types,
                out_max_luminance, out_max_average_luminance,
                out_min_luminance, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to get hdr capabilities for"
                " display " << display;
    }

    void set_color_transform(hwc2_display_t display,
            const std::array<float, 16> &matrix, android_color_transform_t hint,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_COLOR_TRANSFORM pfn = (HWC2_PFN_SET_COLOR_TRANSFORM)
                get_function(HWC2_FUNCTION_SET_COLOR_TRANSFORM);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, matrix.data(),
                hint);
    }

    void set_color_transform(hwc2_display_t display,
            const std::array<float, 16> &matrix, android_color_transform_t hint)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_color_transform(display, matrix, hint,
                &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set color transform "
                << hint;
    }

    void create_virtual_display(uint32_t width, uint32_t height,
            android_pixel_format_t *out_format, hwc2_display_t *out_display,
            hwc2_error_t *out_err)
    {
        HWC2_PFN_CREATE_VIRTUAL_DISPLAY pfn = (HWC2_PFN_CREATE_VIRTUAL_DISPLAY)
                get_function(HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, width, height,
                (int32_t *) out_format, out_display);

        if (*out_err == HWC2_ERROR_NONE)
            virtual_displays.insert(*out_display);
}

    void create_virtual_display(uint32_t width, uint32_t height,
            android_pixel_format_t *out_format, hwc2_display_t *out_display)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(create_virtual_display(width, height, out_format,
                out_display, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to create virtual display";
    }

    void destroy_virtual_display(hwc2_display_t display, hwc2_error_t *out_err)
    {
        HWC2_PFN_DESTROY_VIRTUAL_DISPLAY pfn = (HWC2_PFN_DESTROY_VIRTUAL_DISPLAY)
                get_function(HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display);

        if (*out_err == HWC2_ERROR_NONE)
            virtual_displays.erase(display);
    }

    void destroy_virtual_display(hwc2_display_t display)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to destroy virtual display";
    }

    void get_max_virtual_display_count(uint32_t *out_max_cnt)
    {
        HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT pfn =
                (HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT)
                get_function(HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_max_cnt = (hwc2_error_t) pfn(hwc2_device);
    }

    void set_output_buffer(hwc2_display_t display, buffer_handle_t buffer,
            int32_t release_fence, hwc2_error_t *out_err)
    {
        HWC2_PFN_SET_OUTPUT_BUFFER pfn = (HWC2_PFN_SET_OUTPUT_BUFFER)
                get_function(HWC2_FUNCTION_SET_OUTPUT_BUFFER);
        ASSERT_TRUE(pfn) << "failed to get function";

        *out_err = (hwc2_error_t) pfn(hwc2_device, display, buffer,
                release_fence);
    }

    void set_output_buffer(hwc2_display_t display, buffer_handle_t buffer,
            int32_t release_fence)
    {
        hwc2_error_t err = HWC2_ERROR_NONE;
        ASSERT_NO_FATAL_FAILURE(set_output_buffer(display, buffer,
                release_fence, &err));
        ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to set output buffer";
    }

protected:
    hwc2_function_pointer_t get_function(hwc2_function_descriptor_t descriptor)
    {
        return hwc2_device->getFunction(hwc2_device, descriptor);
    }

    void get_capabilities(std::vector<hwc2_capability_t> *out_capabilities)
    {
        uint32_t num_capabilities = 0;

        hwc2_device->getCapabilities(hwc2_device, &num_capabilities, nullptr);

        out_capabilities->resize(num_capabilities);

        hwc2_device->getCapabilities(hwc2_device, &num_capabilities,
                (int32_t *) out_capabilities->data());
    }

    /* NOTE: will create min(new_layer_cnt, max supported layers) layers */
    void create_layers(hwc2_display_t display,
            std::vector<hwc2_layer_t> &layers, size_t new_layer_cnt)
    {
        std::vector<hwc2_layer_t> new_layers;
        hwc2_layer_t layer;
        hwc2_error_t err = HWC2_ERROR_NONE;

        for (size_t i = 0; i < new_layer_cnt; i++) {

            EXPECT_NO_FATAL_FAILURE(create_layer(display, &layer, &err));
            if (err == HWC2_ERROR_NO_RESOURCES)
                break;
            if (err != HWC2_ERROR_NONE) {
                new_layers.clear();
                ASSERT_EQ(err, HWC2_ERROR_NONE) << "failed to create layer";
            }
            new_layers.push_back(layer);
        }

        layers.insert(layers.end(), new_layers.begin(), new_layers.end());
    }

    void destroy_layers(hwc2_display_t display,
            std::vector<hwc2_layer_t> &layers)
    {
        for (hwc2_layer_t layer: layers)
            EXPECT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        layers.clear();
    }

    void get_invalid_config(hwc2_display_t display, hwc2_config_t *out_config)
    {
        std::vector<hwc2_config_t> configs;

        ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

        std::set<hwc2_config_t> configs_set(configs.begin(), configs.end());
        hwc2_config_t CONFIG_MAX = UINT32_MAX;

        ASSERT_LE(configs_set.size() - 1, CONFIG_MAX) << "every config value"
                " (2^32 values) has been taken which shouldn't happen";

        hwc2_config_t config;
        for (config = 0; config < CONFIG_MAX; config++)
            if (configs_set.find(config) == configs_set.end())
                break;

        *out_config = config;
    }

    void enable_vsync(hwc2_display_t display)
    {
        ASSERT_NO_FATAL_FAILURE(register_callback(HWC2_CALLBACK_VSYNC,
                this, (hwc2_function_pointer_t) hwc2_test_vsync_callback));
        ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_ENABLE));
    }

    void disable_vsync(hwc2_display_t display)
    {
        ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE));
    }

    void wait_for_vsync(hwc2_display_t *out_display = nullptr,
            int64_t *out_timestamp = nullptr)
    {
        std::unique_lock<std::mutex> lock(vsync_mutex);
        ASSERT_EQ(vsync_cv.wait_for(lock, std::chrono::seconds(3)),
                std::cv_status::no_timeout) << "timed out attempting to get"
                " vsync callback";
        if (out_display)
            *out_display = vsync_display;
        if (out_timestamp)
            *out_timestamp = vsync_timestamp;
    }

    void get_active_config_attribute(hwc2_display_t display,
            hwc2_attribute_t attribute, int32_t *out_value)
    {
        hwc2_config_t config;
        ASSERT_NO_FATAL_FAILURE(get_active_config(display, &config));
        ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                attribute, out_value));
        ASSERT_GE(*out_value, 0) << "failed to get valid "
                << getAttributeName(attribute);
    }

    void get_active_dimensions(hwc2_display_t display, int32_t *out_width,
            int32_t *out_height)
    {
        ASSERT_NO_FATAL_FAILURE(get_active_config_attribute(display,
                HWC2_ATTRIBUTE_WIDTH, out_width));
        ASSERT_NO_FATAL_FAILURE(get_active_config_attribute(display,
                HWC2_ATTRIBUTE_HEIGHT, out_height));
    }

    void close_fences(hwc2_display_t display, int32_t present_fence)
    {
        std::vector<hwc2_layer_t> layers;
        std::vector<int32_t> fences;
        int ms_wait = 3000;

        if (present_fence >= 0) {
            ASSERT_GE(sync_wait(present_fence, ms_wait), 0);
            close(present_fence);
        }

        ASSERT_NO_FATAL_FAILURE(get_release_fences(display, &layers, &fences));
        ASSERT_EQ(layers.size(), fences.size());

        for (int32_t fence: fences) {
            ASSERT_GE(sync_wait(fence, ms_wait), 0);
            if (fence >= 0)
                close(fence);
        }
    }

    void set_layer_properties(hwc2_display_t display, hwc2_layer_t layer,
            hwc2_test_layers &test_layers, bool *out_skip)
    {
        hwc2_composition_t composition;
        buffer_handle_t handle = nullptr;
        int32_t acquire_fence;
        hwc2_error_t err = HWC2_ERROR_NONE;
        *out_skip = true;

        if (!test_layers.contains(layer))
            return;

        composition = test_layers.get_composition(layer);

        /* If the device cannot support a buffer format, then do not continue */
        if ((composition == HWC2_COMPOSITION_DEVICE
                || composition == HWC2_COMPOSITION_CURSOR)
                && test_layers.get_buffer(layer, &handle, &acquire_fence) < 0)
            return;

        EXPECT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                composition, &err));
        if (err == HWC2_ERROR_UNSUPPORTED)
            EXPECT_TRUE(composition != HWC2_COMPOSITION_CLIENT
                    && composition != HWC2_COMPOSITION_DEVICE);

        std::pair<int32_t, int32_t> cursor = test_layers.get_cursor(layer);

        EXPECT_NO_FATAL_FAILURE(set_layer_buffer(display, layer, handle,
                acquire_fence));
        EXPECT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer,
                test_layers.get_blend_mode(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_color(display, layer,
                test_layers.get_color(layer)));
        EXPECT_NO_FATAL_FAILURE(set_cursor_position(display, layer,
                cursor.first, cursor.second));
        EXPECT_NO_FATAL_FAILURE(set_layer_dataspace(display, layer,
                test_layers.get_dataspace(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_display_frame(display, layer,
                test_layers.get_display_frame(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_plane_alpha(display, layer,
                test_layers.get_plane_alpha(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_source_crop(display, layer,
                test_layers.get_source_crop(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_surface_damage(display, layer,
                test_layers.get_surface_damage(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_transform(display, layer,
                test_layers.get_transform(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_visible_region(display, layer,
                test_layers.get_visible_region(layer)));
        EXPECT_NO_FATAL_FAILURE(set_layer_z_order(display, layer,
                test_layers.get_z_order(layer)));

        *out_skip = false;
    }

    void set_layer_properties(hwc2_display_t display,
            std::vector<hwc2_layer_t> &layers,
            hwc2_test_layers &test_layers, bool *out_skip)
    {
        for (auto itr = layers.begin(); itr != layers.end(); itr++) {
            EXPECT_NO_FATAL_FAILURE(set_layer_properties(display, *itr,
                    test_layers, out_skip));
            if (*out_skip)
                return;
        }
    }

    void manage_requests(hwc2_display_t display,
            const std::vector<hwc2_layer_t> &layers, uint32_t num_requests,
            std::set<hwc2_layer_t> *out_clear_layers = nullptr,
            bool *out_flip_client_target = nullptr)
    {
        hwc2_display_request_t display_request =
                static_cast<hwc2_display_request_t>(0);
        std::vector<hwc2_layer_t> requested_layers;
        std::vector<hwc2_layer_request_t> requests;

        ASSERT_NO_FATAL_FAILURE(get_display_requests(display, &display_request,
                &requested_layers, &requests));

        EXPECT_EQ(num_requests, requests.size()) << "validate returned "
                << num_requests << " requests and get display requests returned "
                << requests.size() << " requests";

        hwc2_layer_t requested_layer;
        hwc2_layer_request_t request;

        for (size_t i = 0; i < requests.size(); i++) {
            requested_layer = requested_layers.at(i);
            request = requests.at(i);

            EXPECT_TRUE(std::find(layers.begin(), layers.end(), requested_layer)
                    != layers.end())
                    << "get display requests returned an unknown layer";
            EXPECT_NE(request, 0) << "returned empty request for layer "
                    << requested_layer;

            if (out_clear_layers && request
                    == HWC2_LAYER_REQUEST_CLEAR_CLIENT_TARGET)
                out_clear_layers->insert(requested_layer);
        }

        if (out_flip_client_target)
            *out_flip_client_target = display_request
                    & HWC2_DISPLAY_REQUEST_FLIP_CLIENT_TARGET;
    }

    void manage_composition_changes(hwc2_display_t display,
            const hwc2_test_layers &test_layers,
            const std::vector<hwc2_layer_t> &layers,
            uint32_t num_types,
            std::set<hwc2_layer_t> *out_client_layers = nullptr)
    {
        std::vector<hwc2_layer_t> changed_layers;
        std::vector<hwc2_composition_t> types;

        ASSERT_NO_FATAL_FAILURE(get_changed_composition_types(display,
                &changed_layers, &types));

        EXPECT_EQ(num_types, types.size()) << "validate returned "
                << num_types << " types and get changed composition types"
                " returned " << types.size() << " types";

        for (size_t i = 0; i < types.size(); i++) {

            auto layer = std::find(layers.begin(), layers.end(),
                    changed_layers.at(i));

            EXPECT_TRUE(layer != layers.end() || !test_layers.contains(*layer))
                    << "get changed composition types returned an unknown layer";

            hwc2_composition_t requested_type = test_layers.get_composition(
                    *layer);
            hwc2_composition_t returned_type = types.at(i);

            EXPECT_NE(returned_type, HWC2_COMPOSITION_INVALID) << "get changed"
                    " composition types returned invalid composition";

            switch (requested_type) {
            case HWC2_COMPOSITION_CLIENT:
                EXPECT_TRUE(false) << getCompositionName(returned_type)
                        << " cannot be changed";
                break;
            case HWC2_COMPOSITION_DEVICE:
            case HWC2_COMPOSITION_SOLID_COLOR:
                EXPECT_EQ(returned_type, HWC2_COMPOSITION_CLIENT)
                        << "composition of type "
                        << getCompositionName(requested_type)
                        << " can only be changed to "
                        << getCompositionName(HWC2_COMPOSITION_CLIENT);
                break;
            case HWC2_COMPOSITION_CURSOR:
            case HWC2_COMPOSITION_SIDEBAND:
                EXPECT_TRUE(returned_type == HWC2_COMPOSITION_CLIENT
                        || returned_type == HWC2_COMPOSITION_DEVICE)
                        << "composition of type "
                        << getCompositionName(requested_type)
                        << " can only be changed to "
                        << getCompositionName(HWC2_COMPOSITION_CLIENT) << " or "
                        << getCompositionName(HWC2_COMPOSITION_DEVICE);
                break;
            default:
                EXPECT_TRUE(false) << "unknown type "
                        << getCompositionName(requested_type);
                break;
            }

            if (out_client_layers)
                if (returned_type == HWC2_COMPOSITION_CLIENT)
                    out_client_layers->insert(*layer);
        }

        if (out_client_layers)
            for (auto layer: layers)
                if (test_layers.get_composition(layer)
                        == HWC2_COMPOSITION_CLIENT)
                    out_client_layers->insert(layer);
    }

    void set_client_target(hwc2_display_t display,
            hwc2_test_client_target &test_client_target,
            const hwc2_test_layers &test_layers,
            const std::set<hwc2_layer_t> client_layers,
            const std::set<hwc2_layer_t> clear_layers,
            bool flip_client_target, int32_t display_width,
            int32_t display_height)
    {
        android_pixel_format_t format = HAL_PIXEL_FORMAT_RGBA_8888;
        android_dataspace_t dataspace = HAL_DATASPACE_UNKNOWN;
        hwc_region_t damage = { };
        buffer_handle_t handle;
        int32_t acquire_fence;

        ASSERT_EQ(test_client_target.get_buffer(test_layers, client_layers,
                clear_layers, flip_client_target, display_width, display_height,
                format, &handle, &acquire_fence), 0);
        EXPECT_NO_FATAL_FAILURE(set_client_target(display, handle,
                acquire_fence, dataspace, damage));
    }

    void present_display(hwc2_display_t display, size_t layer_cnt,
            hwc2_test_coverage_t coverage, const std::map<hwc2_test_property_t,
            hwc2_test_coverage_t> &coverage_exceptions,
            bool full_display_required)
    {
        std::vector<hwc2_config_t> configs;
        int32_t width, height;
        std::vector<hwc2_layer_t> layers;
        uint32_t num_types, num_requests;
        bool has_changes, skip;
        bool flip_client_target;
        int32_t present_fence;

        ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));
        ASSERT_NO_FATAL_FAILURE(enable_vsync(display));

        ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

        for (hwc2_config_t config: configs) {
            ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
            ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

            ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
            hwc2_test_layers test_layers(layers, coverage, width, height,
                    coverage_exceptions);

            if (full_display_required)
                test_layers.require_full_display();

            std::set<hwc2_layer_t> client_layers;
            std::set<hwc2_layer_t> clear_layers;
            hwc2_test_client_target test_client_target;

            do {
                ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                        test_layers, &skip));
                if (skip)
                    continue;

                ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                        &num_requests, &has_changes));
                if (has_changes)
                    EXPECT_LE(num_types, (uint32_t) layers.size())
                            << "wrong number of requests";

                ASSERT_NO_FATAL_FAILURE(manage_composition_changes(display,
                        test_layers, layers, num_types, &client_layers));
                ASSERT_NO_FATAL_FAILURE(manage_requests(display, layers,
                        num_requests, &clear_layers, &flip_client_target));
                ASSERT_NO_FATAL_FAILURE(set_client_target(display,
                        test_client_target, test_layers, client_layers,
                        clear_layers, flip_client_target, width, height));
                ASSERT_NO_FATAL_FAILURE(accept_display_changes(display));

                ASSERT_NO_FATAL_FAILURE(wait_for_vsync());

                EXPECT_NO_FATAL_FAILURE(present_display(display, &present_fence));

                ASSERT_NO_FATAL_FAILURE(close_fences(display, present_fence));

                client_layers.clear();
                clear_layers.clear();

            } while (test_layers.advance());

            ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
        }

        ASSERT_NO_FATAL_FAILURE(disable_vsync(display));
        ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
    }

    hwc2_device_t *hwc2_device;

    /* Store all created layers that have not been destroyed. If an ASSERT_*
     * fails, then destroy the layers on exit */
    std::set<std::pair<hwc2_display_t, hwc2_layer_t>> layers;

    /* Store the power mode state. If it is not HWC2_POWER_MODE_OFF when
     * tearing down the test cases, change it to HWC2_POWER_MODE_OFF */
    std::set<hwc2_display_t> active_displays;

    /* Store all created virtual displays that have not been destroyed. If an
     * ASSERT_* fails, then destroy the virtual displays on exit */
    std::set<hwc2_display_t> virtual_displays;

    std::mutex vsync_mutex;
    std::condition_variable vsync_cv;
    hwc2_display_t vsync_display;
    int64_t vsync_timestamp;
};

void hwc2_test_vsync_callback(hwc2_callback_data_t callback_data,
        hwc2_display_t display, int64_t timestamp)
{
    if (callback_data)
        static_cast<hwc2_test *>(callback_data)->vsync_callback(display,
                timestamp);
}


static const std::array<hwc2_function_descriptor_t, 42> required_functions = {{
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

TEST_F(hwc2_test, GET_FUNCTION)
{
    hwc2_function_pointer_t pfn;

    for (hwc2_function_descriptor_t descriptor: required_functions) {
        pfn = get_function(descriptor);
        EXPECT_TRUE(pfn) << "failed to get function "
                << getFunctionDescriptorName(descriptor);
    }
}

TEST_F(hwc2_test, GET_FUNCTION_invalid_function)
{
    hwc2_function_pointer_t pfn = get_function(HWC2_FUNCTION_INVALID);
    EXPECT_FALSE(pfn) << "failed to get invalid function";
}

TEST_F(hwc2_test, GET_CAPABILITIES)
{
    std::vector<hwc2_capability_t> capabilities;

    get_capabilities(&capabilities);

    EXPECT_EQ(std::find(capabilities.begin(), capabilities.end(),
            HWC2_CAPABILITY_INVALID), capabilities.end());
}

static const std::array<hwc2_callback_descriptor_t, 3> callback_descriptors = {{
    HWC2_CALLBACK_HOTPLUG,
    HWC2_CALLBACK_REFRESH,
    HWC2_CALLBACK_VSYNC,
}};

TEST_F(hwc2_test, REGISTER_CALLBACK)
{
    hwc2_callback_data_t data = (hwc2_callback_data_t) "data";

    for (hwc2_callback_descriptor_t descriptor: callback_descriptors)
        ASSERT_NO_FATAL_FAILURE(register_callback(descriptor, data,
                []() { return; }));
}

TEST_F(hwc2_test, REGISTER_CALLBACK_bad_parameter)
{
    hwc2_callback_data_t data = (hwc2_callback_data_t) "data";
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(register_callback(HWC2_CALLBACK_INVALID, data,
            []() { return; }, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";
}

TEST_F(hwc2_test, REGISTER_CALLBACK_null_data)
{
    hwc2_callback_data_t data = nullptr;

    for (hwc2_callback_descriptor_t descriptor: callback_descriptors)
        ASSERT_NO_FATAL_FAILURE(register_callback(descriptor, data,
                []() { return; }));
}

TEST_F(hwc2_test, GET_DISPLAY_TYPE)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_display_type_t type;

    ASSERT_NO_FATAL_FAILURE(get_display_type(display, &type));
    EXPECT_EQ(type, HWC2_DISPLAY_TYPE_PHYSICAL) << "failed to return correct"
            " display type";
}

TEST_F(hwc2_test, GET_DISPLAY_TYPE_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_display_type_t type;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_type(display, &type, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, CREATE_DESTROY_LAYER)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_layer_t layer;

    ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
}

TEST_F(hwc2_test, CREATE_LAYER_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_layer_t layer;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, CREATE_LAYER_no_resources)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_layer_t> layers;
    size_t layer_cnt = 1000;

    ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));

    ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
}

TEST_F(hwc2_test, DESTROY_LAYER_bad_display)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_display_t bad_display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_layer_t layer = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(destroy_layer(bad_display, layer, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

    ASSERT_NO_FATAL_FAILURE(destroy_layer(bad_display, layer, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
}

TEST_F(hwc2_test, DESTROY_LAYER_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_layer_t layer;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, UINT64_MAX / 2, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, 0, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, UINT64_MAX - 1, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, 1, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, UINT64_MAX, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer + 1, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
}

static const std::array<hwc2_attribute_t, 2> required_attributes = {{
    HWC2_ATTRIBUTE_WIDTH,
    HWC2_ATTRIBUTE_HEIGHT,
}};

static const std::array<hwc2_attribute_t, 3> optional_attributes = {{
    HWC2_ATTRIBUTE_VSYNC_PERIOD,
    HWC2_ATTRIBUTE_DPI_X,
    HWC2_ATTRIBUTE_DPI_Y,
}};

TEST_F(hwc2_test, GET_DISPLAY_ATTRIBUTE)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    int32_t value;

    for (hwc2_config_t config: configs) {
        for (hwc2_attribute_t attribute: required_attributes) {
            ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                    attribute, &value));
            EXPECT_GE(value, 0) << "missing required attribute "
                    << getAttributeName(attribute) << " for config " << config;
        }
        for (hwc2_attribute_t attribute: optional_attributes)
            ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                    attribute, &value));
    }
}

TEST_F(hwc2_test, GET_DISPLAY_ATTRIBUTE_invalid_attribute)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    hwc2_attribute_t attribute = HWC2_ATTRIBUTE_INVALID;
    int32_t value;

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                attribute, &value, &err));
        EXPECT_EQ(value, -1) << "failed to return -1 for an invalid"
                " attribute for config " << config;
    }
}

TEST_F(hwc2_test, GET_DISPLAY_ATTRIBUTE_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_config_t config = 0;
    int32_t value;
    hwc2_error_t err = HWC2_ERROR_NONE;

    for (hwc2_attribute_t attribute: required_attributes) {
        ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                attribute, &value, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
    }

    for (hwc2_attribute_t attribute: optional_attributes) {
        ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                attribute, &value, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, GET_DISPLAY_ATTRIBUTE_bad_config)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_config_t config;
    int32_t value;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_invalid_config(display, &config));

    for (hwc2_attribute_t attribute: required_attributes) {
        ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                attribute, &value, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
    }

    for (hwc2_attribute_t attribute: optional_attributes) {
        ASSERT_NO_FATAL_FAILURE(get_display_attribute(display, config,
                attribute, &value, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, GET_DISPLAY_CONFIGS)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));
}

TEST_F(hwc2_test, GET_DISPLAY_CONFIGS_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::vector<hwc2_config_t> configs;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs, &err));

    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
    EXPECT_TRUE(configs.empty()) << "returned configs for bad display";
}

TEST_F(hwc2_test, GET_DISPLAY_CONFIGS_same)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs1, configs2;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs1));
    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs2));

    EXPECT_TRUE(std::is_permutation(configs1.begin(), configs1.end(),
            configs2.begin())) << "returned two different config sets";
}

TEST_F(hwc2_test, GET_DISPLAY_CONFIGS_duplicate)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    std::set<hwc2_config_t> configs_set(configs.begin(), configs.end());
    EXPECT_EQ(configs.size(), configs_set.size()) << "returned duplicate"
            " configs";
}

TEST_F(hwc2_test, GET_ACTIVE_CONFIG)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_config_t active_config;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_config(display, &active_config));

        EXPECT_EQ(active_config, config) << "failed to get active config";
    }
}

TEST_F(hwc2_test, GET_ACTIVE_CONFIG_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_config_t active_config;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_active_config(display, &active_config, &err));

    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, GET_ACTIVE_CONFIG_bad_config)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_config_t active_config;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    if (configs.empty())
        return;

    ASSERT_NO_FATAL_FAILURE(get_active_config(display, &active_config, &err));
    if (err == HWC2_ERROR_NONE)
        EXPECT_TRUE(std::find(configs.begin(), configs.end(), active_config)
                != configs.end()) << "active config is not found in configs for"
                " display";
    else
        EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_ACTIVE_CONFIG)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs)
        EXPECT_NO_FATAL_FAILURE(set_active_config(display, config));
}

TEST_F(hwc2_test, SET_ACTIVE_CONFIG_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_config_t config = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_active_config(display, config, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_ACTIVE_CONFIG_bad_config)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_config_t config;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_invalid_config(display, &config));

    ASSERT_NO_FATAL_FAILURE(set_active_config(display, config, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_CONFIG) << "returned wrong error code";
}

TEST_F(hwc2_test, GET_DOZE_SUPPORT)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    int32_t support = -1;

    ASSERT_NO_FATAL_FAILURE(get_doze_support(display, &support));

    EXPECT_TRUE(support == 0 || support == 1) << "invalid doze support value";
}

TEST_F(hwc2_test, GET_DOZE_SUPPORT_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    int32_t support = -1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_doze_support(display, &support, &err));

    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_POWER_MODE)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));

    int32_t support = -1;
    ASSERT_NO_FATAL_FAILURE(get_doze_support(display, &support));
    if (!support)
        return;

    ASSERT_EQ(support, 1) << "invalid doze support value";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_DOZE));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display,
            HWC2_POWER_MODE_DOZE_SUSPEND));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, SET_POWER_MODE_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    int32_t support = -1;
    ASSERT_NO_FATAL_FAILURE(get_doze_support(display, &support, &err));
    if (!support)
        return;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_DOZE, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_DOZE_SUSPEND,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_POWER_MODE_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_power_mode_t mode = (hwc2_power_mode_t)
            (HWC2_POWER_MODE_DOZE_SUSPEND + 1);
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, mode, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code "
            << mode;
}

TEST_F(hwc2_test, SET_POWER_MODE_unsupported)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    int32_t support = -1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_doze_support(display, &support, &err));
    if (support != 1)
        return;

    ASSERT_EQ(support, 1) << "invalid doze support value";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_DOZE, &err));
    EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_DOZE_SUSPEND,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED) <<  "returned wrong error code";
}

TEST_F(hwc2_test, SET_POWER_MODE_stress)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));

    int32_t support = -1;
    ASSERT_NO_FATAL_FAILURE(get_doze_support(display, &support));
    if (!support)
        return;

    ASSERT_EQ(support, 1) << "invalid doze support value";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_DOZE));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_DOZE));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display,
            HWC2_POWER_MODE_DOZE_SUSPEND));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display,
            HWC2_POWER_MODE_DOZE_SUSPEND));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, SET_VSYNC_ENABLED)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_callback_data_t data = (hwc2_callback_data_t) "data";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(register_callback(HWC2_CALLBACK_VSYNC, data,
            []() { return; }));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_ENABLE));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, SET_VSYNC_ENABLED_callback)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_display_t received_display;
    int64_t received_timestamp;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(enable_vsync(display));

    ASSERT_NO_FATAL_FAILURE(wait_for_vsync(&received_display,
            &received_timestamp));

    EXPECT_EQ(received_display, display) << "failed to get corret display";
    EXPECT_GE(received_timestamp, 0) << "failed to get valid timestamp";

    ASSERT_NO_FATAL_FAILURE(disable_vsync(display));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, SET_VSYNC_ENABLED_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    hwc2_callback_data_t data = (hwc2_callback_data_t) "data";

    ASSERT_NO_FATAL_FAILURE(register_callback(HWC2_CALLBACK_VSYNC, data,
            []() { return; }));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_ENABLE,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_VSYNC_ENABLED_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_error_t err = HWC2_ERROR_NONE;

    hwc2_callback_data_t data = (hwc2_callback_data_t) "data";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(register_callback(HWC2_CALLBACK_VSYNC, data,
            []() { return; }));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_INVALID,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, SET_VSYNC_ENABLED_stress)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;

    hwc2_callback_data_t data = (hwc2_callback_data_t) "data";

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(register_callback(HWC2_CALLBACK_VSYNC, data,
            []() { return; }));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_ENABLE));
    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_ENABLE));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE));
    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, SET_VSYNC_ENABLED_no_callback_no_power)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    uint secs = 1;

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_ENABLE));

    sleep(secs);

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE));
}

TEST_F(hwc2_test, SET_VSYNC_ENABLED_no_callback)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    uint secs = 1;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_ENABLE));

    sleep(secs);

    ASSERT_NO_FATAL_FAILURE(set_vsync_enabled(display, HWC2_VSYNC_DISABLE));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, GET_DISPLAY_NAME)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::string name;

    ASSERT_NO_FATAL_FAILURE(get_display_name(display, &name));
    EXPECT_FALSE(name.empty()) << "failed to get display name";
}

TEST_F(hwc2_test, GET_DISPLAY_NAME_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::string name;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_name(display, &name, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_LAYER_COMPOSITION_TYPE)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_BASIC, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            EXPECT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                    test_layer.get_composition()));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_composition());
    }
}

TEST_F(hwc2_test, SET_LAYER_COMPOSITION_TYPE_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                test_layer.get_composition(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer + 1,
                test_layer.get_composition(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                test_layer.get_composition(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_COMPOSITION_TYPE_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                HWC2_COMPOSITION_INVALID, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_COMPOSITION_TYPE_unsupported)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                    test_layer.get_composition(), &err));
            EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_UNSUPPORTED)
                     << "returned wrong error code";

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_composition());
    }
}

TEST_F(hwc2_test, SET_LAYER_COMPOSITION_TYPE_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                    test_layer.get_composition(), &err));
            EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_UNSUPPORTED)
                     << "returned wrong error code";
        } while (test_layer.advance_composition());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_CURSOR_POSITION)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            std::pair<int32_t, int32_t> cursor = test_layer.get_cursor();
            EXPECT_NO_FATAL_FAILURE(set_cursor_position(display, layer,
                    cursor.first, cursor.second));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_cursor());
    }
}

TEST_F(hwc2_test, SET_CURSOR_POSITION_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_layer_t layer = 0;
    int32_t x = 0, y = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_cursor_position(display, layer, x, y, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_CURSOR_POSITION_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        std::pair<int32_t, int32_t> cursor = test_layer.get_cursor();

        ASSERT_NO_FATAL_FAILURE(set_cursor_position(display, layer,
                cursor.first, cursor.second, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        EXPECT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_cursor_position(display, layer + 1,
                cursor.first, cursor.second, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_cursor_position(display, layer,
                cursor.first, cursor.second, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_CURSOR_POSITION_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            std::pair<int32_t, int32_t> cursor = test_layer.get_cursor();
            EXPECT_NO_FATAL_FAILURE(set_cursor_position(display, layer,
                    cursor.first, cursor.second));
        } while (test_layer.advance_cursor());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_BLEND_MODE)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            EXPECT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer,
                    test_layer.get_blend_mode()));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_blend_mode());
    }
}

TEST_F(hwc2_test, SET_LAYER_BLEND_MODE_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer,
                test_layer.get_blend_mode(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer + 1,
                test_layer.get_blend_mode(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer,
                test_layer.get_blend_mode(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_BLEND_MODE_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    hwc2_blend_mode_t mode = HWC2_BLEND_MODE_INVALID;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer, mode,
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_BLEND_MODE_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            EXPECT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer,
                    test_layer.get_blend_mode()));
        } while (test_layer.advance_blend_mode());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_BUFFER)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;
    buffer_handle_t handle;
    android::base::unique_fd acquire_fence;
    std::vector<hwc2_composition_t> compositions = {HWC2_COMPOSITION_DEVICE,
            HWC2_COMPOSITION_CURSOR};
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        for (auto composition: compositions) {
            do {
                do {
                    if (test_layer.get_buffer(&handle, &acquire_fence) < 0)
                        continue;
                    ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

                    ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display,
                            layer, composition, &err));
                    if (err == HWC2_ERROR_UNSUPPORTED)
                        EXPECT_NE(composition, HWC2_COMPOSITION_DEVICE)
                                << "returned wrong error code";
                    else if (err != HWC2_ERROR_NONE)
                        EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED)
                                << "returned wrong error code";

                    EXPECT_NO_FATAL_FAILURE(set_layer_buffer(display, layer,
                            handle, acquire_fence));

                    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

                } while (test_layer.advance_format());
            } while (test_layer.advance_buffer_area());
            test_layer.reset();
        }
    }
}

TEST_F(hwc2_test, SET_LAYER_BUFFER_bad_composition)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;
    buffer_handle_t handle;
    android::base::unique_fd acquire_fence;
    std::vector<hwc2_composition_t> compositions = {HWC2_COMPOSITION_CLIENT,
            HWC2_COMPOSITION_SOLID_COLOR, HWC2_COMPOSITION_SIDEBAND};
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_BASIC, width, height);

        for (auto composition: compositions) {
            do {
                do {
                    if (test_layer.get_buffer(&handle, &acquire_fence) < 0)
                        continue;
                    ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

                    ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display,
                            layer, composition, &err));
                    if (err == HWC2_ERROR_UNSUPPORTED)
                        EXPECT_NE(composition, HWC2_COMPOSITION_CLIENT)
                                << "returned wrong error code";
                    else if (err != HWC2_ERROR_NONE)
                        EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED)
                                << "returned wrong error code";

                    EXPECT_NO_FATAL_FAILURE(set_layer_buffer(display, layer,
                            handle, acquire_fence));

                    ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

                } while (test_layer.advance_format());
            } while (test_layer.advance_buffer_area());
            test_layer.reset();
        }
    }
}

TEST_F(hwc2_test, SET_LAYER_BUFFER_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer = 0;
    buffer_handle_t handle;
    android::base::unique_fd acquire_fence;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        if (test_layer.get_buffer(&handle, &acquire_fence) == 0)
            continue;

        ASSERT_NO_FATAL_FAILURE(set_layer_buffer(display, layer, handle,
                acquire_fence, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_buffer(display, layer + 1,
                handle, acquire_fence, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_buffer(display, layer, handle,
                acquire_fence, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_BUFFER_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;
    buffer_handle_t handle = nullptr;
    int32_t acquire_fence = -1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_buffer(display, layer, handle,
                acquire_fence, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER)
                << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_BUFFER_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;
    buffer_handle_t handle;
    android::base::unique_fd acquire_fence;
    std::vector<hwc2_composition_t> compositions = {HWC2_COMPOSITION_DEVICE,
            HWC2_COMPOSITION_CURSOR};
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        for (auto composition: compositions) {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            do {
                do {
                    if (test_layer.get_buffer(&handle, &acquire_fence) < 0)
                        continue;

                    ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display,
                            layer, composition, &err));
                    if (err == HWC2_ERROR_UNSUPPORTED)
                        EXPECT_NE(composition, HWC2_COMPOSITION_DEVICE)
                                << "returned wrong error code";
                    else if (err != HWC2_ERROR_NONE)
                        EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED)
                                << "returned wrong error code";

                    EXPECT_NO_FATAL_FAILURE(set_layer_buffer(display, layer,
                            handle, acquire_fence));

                } while (test_layer.advance_format());
            } while (test_layer.advance_buffer_area());
            test_layer.reset();

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        }
    }
}

TEST_F(hwc2_test, SET_LAYER_COLOR)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                        HWC2_COMPOSITION_SOLID_COLOR, &err));
            if (err != HWC2_ERROR_NONE) {
                EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED) << "returned wrong error"
                        " code";
                ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
                return;
            }

            EXPECT_NO_FATAL_FAILURE(set_layer_color(display, layer,
                    test_layer.get_color()));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_color());
    }
}

TEST_F(hwc2_test, SET_LAYER_COLOR_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_color(display, layer,
                test_layer.get_color(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_color(display, layer + 1,
                test_layer.get_color(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_color(display, layer,
                test_layer.get_color(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_COLOR_composition_type_unset)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_BASIC, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            EXPECT_NO_FATAL_FAILURE(set_layer_color(display, layer,
                    test_layer.get_color()));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_color());
    }
}

TEST_F(hwc2_test, SET_LAYER_COLOR_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                    HWC2_COMPOSITION_SOLID_COLOR, &err));
        if (err != HWC2_ERROR_NONE) {
            EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED) << "returned wrong error"
                    " code";
            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
            return;
        }

        do {
            EXPECT_NO_FATAL_FAILURE(set_layer_color(display, layer,
                    test_layer.get_color()));
        } while (test_layer.advance_color());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_DATSPACE)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            EXPECT_NO_FATAL_FAILURE(set_layer_dataspace(display, layer,
                    test_layer.get_dataspace()));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_dataspace());
    }
}

TEST_F(hwc2_test, SET_LAYER_DATSPACE_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_dataspace(display, layer,
                test_layer.get_dataspace(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_dataspace(display, layer + 1,
                test_layer.get_dataspace(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_dataspace(display, layer,
                test_layer.get_dataspace(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_DATSPACE_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            EXPECT_NO_FATAL_FAILURE(set_layer_dataspace(display, layer,
                    test_layer.get_dataspace()));
        } while (test_layer.advance_dataspace());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_DISPLAY_FRAME)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            EXPECT_NO_FATAL_FAILURE(set_layer_display_frame(display, layer,
                    test_layer.get_display_frame()));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_display_frame());
    }
}

TEST_F(hwc2_test, SET_LAYER_DISPLAY_FRAME_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_display_frame(display, layer,
                test_layer.get_display_frame(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_display_frame(display, layer + 1,
                test_layer.get_display_frame(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_display_frame(display, layer,
                test_layer.get_display_frame(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_DISPLAY_FRAME_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            EXPECT_NO_FATAL_FAILURE(set_layer_display_frame(display, layer,
                    test_layer.get_display_frame()));
        } while (test_layer.advance_display_frame());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_PLANE_ALPHA)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            do {
                ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

                ASSERT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer,
                        test_layer.get_blend_mode()));
                EXPECT_NO_FATAL_FAILURE(set_layer_plane_alpha(display, layer,
                        test_layer.get_plane_alpha()));

                ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
            } while (test_layer.advance_plane_alpha());
        } while (test_layer.advance_blend_mode());
    }
}

TEST_F(hwc2_test, SET_LAYER_PLANE_ALPHA_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_plane_alpha(display, layer,
                test_layer.get_plane_alpha(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_plane_alpha(display, layer + 1,
                test_layer.get_plane_alpha(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_plane_alpha(display, layer,
                test_layer.get_plane_alpha(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_PLANE_ALPHA_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            do {
                ASSERT_NO_FATAL_FAILURE(set_layer_blend_mode(display, layer,
                        test_layer.get_blend_mode()));
                EXPECT_NO_FATAL_FAILURE(set_layer_plane_alpha(display, layer,
                        test_layer.get_plane_alpha()));
            } while (test_layer.advance_plane_alpha());
        } while (test_layer.advance_blend_mode());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_SOURCE_CROP)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            do {
                ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

                EXPECT_NO_FATAL_FAILURE(set_layer_source_crop(display, layer,
                        test_layer.get_source_crop()));

                ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
            } while (test_layer.advance_source_crop());
        } while (test_layer.advance_buffer_area());
    }
}

TEST_F(hwc2_test, SET_LAYER_SOURCE_CROP_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_source_crop(display, layer,
                test_layer.get_source_crop(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_source_crop(display, layer + 1,
                test_layer.get_source_crop(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_source_crop(display, layer,
                test_layer.get_source_crop(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
   }
}

TEST_F(hwc2_test, SET_LAYER_SOURCE_CROP_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            do {
                EXPECT_NO_FATAL_FAILURE(set_layer_source_crop(display, layer,
                        test_layer.get_source_crop()));
            } while (test_layer.advance_source_crop());
        } while (test_layer.advance_buffer_area());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_SURFACE_DAMAGE)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            do {
                ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

                EXPECT_NO_FATAL_FAILURE(set_layer_surface_damage(display, layer,
                        test_layer.get_surface_damage()));

                ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
            } while (test_layer.advance_surface_damage());
        } while (test_layer.advance_buffer_area());
    }
}

TEST_F(hwc2_test, SET_LAYER_SURFACE_DAMAGE_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_surface_damage(display, layer,
                test_layer.get_surface_damage(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_surface_damage(display, layer + 1,
                test_layer.get_surface_damage(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_surface_damage(display, layer,
                test_layer.get_surface_damage(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_SURFACE_DAMAGE_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            do {
                EXPECT_NO_FATAL_FAILURE(set_layer_surface_damage(display, layer,
                        test_layer.get_surface_damage()));
            } while (test_layer.advance_surface_damage());
        } while (test_layer.advance_buffer_area());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_TRANSFORM)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

            EXPECT_NO_FATAL_FAILURE(set_layer_transform(display, layer,
                    test_layer.get_transform()));

            ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
        } while (test_layer.advance_transform());
    }
}

TEST_F(hwc2_test, SET_LAYER_TRANSFORM_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_DEFAULT, width, height);

        ASSERT_NO_FATAL_FAILURE(set_layer_transform(display, layer,
                test_layer.get_transform(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_transform(display, layer + 1,
                test_layer.get_transform(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_transform(display, layer,
                test_layer.get_transform(), &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_TRANSFORM_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_layer test_layer(HWC2_TEST_COVERAGE_COMPLETE, width, height);

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        do {
            EXPECT_NO_FATAL_FAILURE(set_layer_transform(display, layer,
                    test_layer.get_transform()));
        } while (test_layer.advance_transform());

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, SET_LAYER_VISIBLE_REGION)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 5;
    std::vector<hwc2_layer_t> layers;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_BASIC, width,
                height);

        do {
            for (auto &layer: layers)
                EXPECT_NO_FATAL_FAILURE(set_layer_visible_region(display,
                        layer, test_layers.get_visible_region(layer)));
        } while (test_layers.advance_visible_regions());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }
}

TEST_F(hwc2_test, SET_LAYER_VISIBLE_REGION_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_layer_t layer = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(set_layer_visible_region(display, layer, { },
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_visible_region(display, layer + 1, { },
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_visible_region(display, layer, { },
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_Z_ORDER)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    size_t layer_cnt = 10;
    std::vector<hwc2_layer_t> layers;
    int32_t width, height;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_COMPLETE,
                width, height);

        for (auto layer: layers)
            EXPECT_NO_FATAL_FAILURE(set_layer_z_order(display, layer,
                    test_layers.get_z_order(layer)));

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }
}

TEST_F(hwc2_test, SET_LAYER_Z_ORDER_bad_layer)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer = 0;
    uint32_t z_order = 0;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        ASSERT_NO_FATAL_FAILURE(set_layer_z_order(display, layer, z_order,
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_z_order(display, layer + 1, z_order,
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));

        ASSERT_NO_FATAL_FAILURE(set_layer_z_order(display, layer, z_order,
                &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_LAYER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_LAYER_Z_ORDER_update)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_layer_t layer;
    std::vector<uint32_t> z_orders = { static_cast<uint32_t>(0),
            static_cast<uint32_t>(1), static_cast<uint32_t>(UINT32_MAX * 0.25),
            static_cast<uint32_t>(UINT32_MAX * 0.5),
            static_cast<uint32_t>(UINT32_MAX) };

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        ASSERT_NO_FATAL_FAILURE(create_layer(display, &layer));

        for (uint32_t z_order: z_orders)
            EXPECT_NO_FATAL_FAILURE(set_layer_z_order(display, layer, z_order));

        ASSERT_NO_FATAL_FAILURE(destroy_layer(display, layer));
    }
}

TEST_F(hwc2_test, VALIDATE_DISPLAY_basic)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 1;
    std::vector<hwc2_layer_t> layers;
    uint32_t num_types, num_requests;
    bool has_changes, skip;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_BASIC, width,
                height);
        test_layers.require_full_display();

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            EXPECT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                    &num_requests, &has_changes));
            if (has_changes)
                EXPECT_LE(num_types, (uint32_t) layers.size())
                        << "wrong number of requests";
        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, VALIDATE_DISPLAY_basic_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 2;
    std::vector<hwc2_layer_t> layers;
    uint32_t num_types, num_requests;
    bool has_changes, skip;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_BASIC, width,
                height);
        test_layers.require_full_display();

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            EXPECT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                    &num_requests, &has_changes));
            if (has_changes)
                EXPECT_LE(num_types, (uint32_t) layers.size())
                        << "wrong number of requests";
        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, VALIDATE_DISPLAY_default_5)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 5;
    std::vector<hwc2_layer_t> layers;
    uint32_t num_types, num_requests;
    bool has_changes, skip;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_DEFAULT, width,
                height);
        test_layers.require_full_display();

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            EXPECT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                    &num_requests, &has_changes));
            if (has_changes)
                EXPECT_LE(num_types, (uint32_t) layers.size())
                        << "wrong number of requests";
        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, VALIDATE_DISPLAY_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    uint32_t num_types, num_requests;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types, &num_requests,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, GET_DISPLAY_REQUESTS_basic)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 1;
    std::vector<hwc2_layer_t> layers;
    uint32_t num_types, num_requests;
    bool has_changes, skip;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_BASIC, width,
                height);

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                    &num_requests, &has_changes));
            if (has_changes)
                EXPECT_LE(num_types, layers.size()) << "wrong number of requests";

            EXPECT_NO_FATAL_FAILURE(manage_requests(display, layers,
                    num_requests));

        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, GET_DISPLAY_REQUESTS_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_display_request_t display_requests;
    std::vector<hwc2_layer_t> layers;
    std::vector<hwc2_layer_request_t> layer_requests;
    hwc2_error_t err = HWC2_ERROR_NONE;

    EXPECT_NO_FATAL_FAILURE(get_display_requests(display, &display_requests,
            &layers, &layer_requests, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, GET_DISPLAY_REQUESTS_not_validated)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 3;
    uint32_t num_types, num_requests;
    hwc2_display_request_t display_requests;
    std::vector<hwc2_layer_t> layers;
    std::vector<hwc2_layer_request_t> layer_requests;
    bool has_changes;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_requests(display, &display_requests,
            &layers, &layer_requests, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));

    ASSERT_NO_FATAL_FAILURE(get_display_requests(display, &display_requests,
            &layers, &layer_requests, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    for (auto layer: layers)
        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                HWC2_COMPOSITION_CLIENT));

    ASSERT_NO_FATAL_FAILURE(get_display_requests(display, &display_requests,
            &layers, &layer_requests, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
            &num_requests, &has_changes));

    for (auto layer: layers)
        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                HWC2_COMPOSITION_DEVICE));

    ASSERT_NO_FATAL_FAILURE(get_display_requests(display, &display_requests,
            &layers, &layer_requests, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, GET_CHANGED_COMPOSITION_TYPES_basic)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 1;
    std::vector<hwc2_layer_t> layers;
    uint32_t num_types, num_requests;
    bool has_changes, skip;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_BASIC, width,
                height);

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                    &num_requests, &has_changes));
            if (has_changes)
                EXPECT_LE(num_types, layers.size()) << "wrong number of requests";

            EXPECT_NO_FATAL_FAILURE(manage_composition_changes(display,
                    test_layers, layers, num_types));

        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, GET_CHANGED_COMPOSITION_TYPES_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::vector<hwc2_layer_t> layers;
    std::vector<hwc2_composition_t> types;
    hwc2_error_t err = HWC2_ERROR_NONE;

    EXPECT_NO_FATAL_FAILURE(get_changed_composition_types(display, &layers,
            &types, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, GET_CHANGED_COMPOSITION_TYPES_not_validated)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 3;
    uint32_t num_types, num_requests;
    std::vector<hwc2_layer_t> layers;
    std::vector<hwc2_composition_t> types;
    bool has_changes;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_changed_composition_types(display, &layers,
            &types, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));

    ASSERT_NO_FATAL_FAILURE(get_changed_composition_types(display, &layers,
            &types, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    for (auto layer: layers)
        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                HWC2_COMPOSITION_CLIENT));

    ASSERT_NO_FATAL_FAILURE(get_changed_composition_types(display, &layers,
            &types, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
            &num_requests, &has_changes));

    for (auto layer: layers)
        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                HWC2_COMPOSITION_DEVICE));

    ASSERT_NO_FATAL_FAILURE(get_changed_composition_types(display, &layers,
            &types, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, ACCEPT_DISPLAY_CHANGES_basic)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 1;
    std::vector<hwc2_layer_t> layers;
    uint32_t num_types, num_requests;
    bool has_changes, skip;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_BASIC, width,
                height);

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                    &num_requests, &has_changes));
            if (has_changes)
                EXPECT_LE(num_types, layers.size()) << "wrong number of requests";

            ASSERT_NO_FATAL_FAILURE(manage_composition_changes(display,
                    test_layers, layers, num_types));

            EXPECT_NO_FATAL_FAILURE(accept_display_changes(display));

        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, ACCEPT_DISPLAY_CHANGES_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    EXPECT_NO_FATAL_FAILURE(accept_display_changes(display, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, ACCEPT_DISPLAY_CHANGES_not_validated)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 5;
    uint32_t num_types, num_requests;
    std::vector<hwc2_layer_t> layers;
    bool has_changes;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(accept_display_changes(display, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));

    ASSERT_NO_FATAL_FAILURE(accept_display_changes(display, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    for (auto layer: layers)
        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                HWC2_COMPOSITION_CLIENT));

    ASSERT_NO_FATAL_FAILURE(accept_display_changes(display, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
            &num_requests, &has_changes));

    for (auto layer: layers)
        ASSERT_NO_FATAL_FAILURE(set_layer_composition_type(display, layer,
                HWC2_COMPOSITION_DEVICE));

    ASSERT_NO_FATAL_FAILURE(accept_display_changes(display, &err));
    EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

    ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, GET_CLIENT_TARGET_SUPPORT)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    android_pixel_format_t format = HAL_PIXEL_FORMAT_RGBA_8888;
    android_dataspace_t dataspace = HAL_DATASPACE_UNKNOWN;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(get_client_target_support(display, width,
                height, format, dataspace));
    }
}

TEST_F(hwc2_test, GET_CLIENT_TARGET_SUPPORT_bad_display)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_display_t bad_display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_client_target_support test_client_target_support(
                HWC2_TEST_COVERAGE_DEFAULT, width, height);

        do {
            const std::pair<int32_t, int32_t> buffer_area =
                    test_client_target_support.get_buffer_area();
            ASSERT_NO_FATAL_FAILURE(get_client_target_support(bad_display,
                    buffer_area.first, buffer_area.second,
                    test_client_target_support.get_format(),
                    test_client_target_support.get_dataspace(), &err));
            EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
        } while (test_client_target_support.advance());
    }
}

TEST_F(hwc2_test, GET_CLIENT_TARGET_SUPPORT_unsupported)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));
        hwc2_test_client_target_support test_client_target_support(
                HWC2_TEST_COVERAGE_DEFAULT, width, height);

        do {
            const std::pair<int32_t, int32_t> buffer_area =
                    test_client_target_support.get_buffer_area();
            ASSERT_NO_FATAL_FAILURE(get_client_target_support(display,
                    buffer_area.first, buffer_area.second,
                    test_client_target_support.get_format(),
                    test_client_target_support.get_dataspace(), &err));
            EXPECT_TRUE(err == HWC2_ERROR_NONE
                    || err == HWC2_ERROR_UNSUPPORTED)
                    << "returned wrong error code";
        } while (test_client_target_support.advance());
    }
}

TEST_F(hwc2_test, SET_CLIENT_TARGET_basic)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    size_t layer_cnt = 1;
    std::vector<hwc2_layer_t> layers;
    uint32_t num_types, num_requests;
    bool has_changes, skip;
    bool flip_client_target;
    android_pixel_format_t format = HAL_PIXEL_FORMAT_RGBA_8888;
    android_dataspace_t dataspace = HAL_DATASPACE_UNKNOWN;
    hwc_region_t damage = { };
    buffer_handle_t handle;
    int32_t acquire_fence;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_BASIC, width,
                height);

        std::set<hwc2_layer_t> client_layers;
        std::set<hwc2_layer_t> clear_layers;
        hwc2_test_client_target test_client_target;

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            ASSERT_NO_FATAL_FAILURE(validate_display(display, &num_types,
                    &num_requests, &has_changes));
            if (has_changes)
                EXPECT_LE(num_types, layers.size())
                        << "wrong number of requests";

            ASSERT_NO_FATAL_FAILURE(manage_composition_changes(display,
                    test_layers, layers, num_types, &client_layers));
            ASSERT_NO_FATAL_FAILURE(manage_requests(display, layers,
                    num_requests, &clear_layers, &flip_client_target));
            ASSERT_EQ(test_client_target.get_buffer(test_layers, client_layers,
                    clear_layers, flip_client_target, width,
                    height, format, &handle, &acquire_fence), 0);
            EXPECT_NO_FATAL_FAILURE(set_client_target(display, handle,
                    acquire_fence, dataspace, damage));

            if (acquire_fence >= 0)
                close(acquire_fence);

            client_layers.clear();
            clear_layers.clear();

        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, SET_CLIENT_TARGET_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::vector<hwc2_layer_t> layers;
    int32_t width = 0, height = 0;
    hwc2_test_layers test_layers(layers, HWC2_TEST_COVERAGE_DEFAULT, width, height);
    std::set<hwc2_layer_t> client_layers;
    std::set<hwc2_layer_t> flip_client_target_layers;
    bool flip_client_target = true;
    android_pixel_format_t format = HAL_PIXEL_FORMAT_RGBA_8888;
    android_dataspace_t dataspace = HAL_DATASPACE_UNKNOWN;
    hwc_region_t damage = { };
    buffer_handle_t handle;
    int32_t acquire_fence;
    hwc2_error_t err = HWC2_ERROR_NONE;

    hwc2_test_client_target test_client_target;

    ASSERT_EQ(test_client_target.get_buffer(test_layers, client_layers,
            flip_client_target_layers, flip_client_target, width, height,
            format, &handle, &acquire_fence), 0);

    EXPECT_NO_FATAL_FAILURE(set_client_target(display, handle,
            acquire_fence, dataspace, damage, &err));

    if (acquire_fence >= 0)
        close(acquire_fence);

    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, PRESENT_DISPLAY_default_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages;
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_default_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 2;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages;
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_default_3)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 3;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages;
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_default_4)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 4;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages;
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_default_5)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 5;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages;
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_default_6)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 6;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages;
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_blend_mode_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_BLEND_MODE, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_PLANE_ALPHA, HWC2_TEST_COVERAGE_BASIC}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_blend_mode_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 2;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_BLEND_MODE, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_PLANE_ALPHA, HWC2_TEST_COVERAGE_BASIC}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_blend_mode_3)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 3;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_BLEND_MODE, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_PLANE_ALPHA, HWC2_TEST_COVERAGE_BASIC}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_buffer_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_BUFFER_AREA, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_FORMAT, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_color_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_COMPOSITION, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_COLOR, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_composition_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_COMPOSITION, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_cursor_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_COMPOSITION, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_CURSOR, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_cursor_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 2;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_COMPOSITION, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_CURSOR, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_BASIC}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_dataspace_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DATASPACE, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_display_frame_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_display_frame_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 2;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_display_frame_3)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 3;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_display_frame_4)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 4;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_format_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_FORMAT, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_plane_alpha_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_BLEND_MODE, HWC2_TEST_COVERAGE_BASIC},
            {HWC2_TEST_PLANE_ALPHA, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_plane_alpha_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 2;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_BLEND_MODE, HWC2_TEST_COVERAGE_BASIC},
            {HWC2_TEST_PLANE_ALPHA, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_plane_alpha_3)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 3;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_BLEND_MODE, HWC2_TEST_COVERAGE_BASIC},
            {HWC2_TEST_PLANE_ALPHA, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_source_crop_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_BASIC},
            {HWC2_TEST_SOURCE_CROP, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_source_crop_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 2;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_BASIC},
            {HWC2_TEST_SOURCE_CROP, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_source_crop_3)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 3;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_BASIC},
            {HWC2_TEST_SOURCE_CROP, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_surface_damage_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_SURFACE_DAMAGE, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_transform_1)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_TRANSFORM, HWC2_TEST_COVERAGE_COMPLETE}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_transform_2)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 2;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::map<hwc2_test_property_t, hwc2_test_coverage_t> coverages =
            {{HWC2_TEST_TRANSFORM, HWC2_TEST_COVERAGE_COMPLETE},
            {HWC2_TEST_DISPLAY_FRAME, HWC2_TEST_COVERAGE_BASIC}};
    bool full_display_required = true;

    ASSERT_NO_FATAL_FAILURE(present_display(display, layer_cnt, coverage,
            coverages, full_display_required));
}

TEST_F(hwc2_test, PRESENT_DISPLAY_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    int32_t present_fence;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(present_display(display, &present_fence, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, PRESENT_DISPLAY_not_validated)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    size_t layer_cnt = 1;
    hwc2_test_coverage_t coverage = HWC2_TEST_COVERAGE_DEFAULT;
    std::vector<hwc2_config_t> configs;
    int32_t width, height;
    std::vector<hwc2_layer_t> layers;
    bool skip;
    int32_t present_fence;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_ON));
    ASSERT_NO_FATAL_FAILURE(enable_vsync(display));

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));
        ASSERT_NO_FATAL_FAILURE(get_active_dimensions(display, &width, &height));

        ASSERT_NO_FATAL_FAILURE(create_layers(display, layers, layer_cnt));
        hwc2_test_layers test_layers(layers, coverage, width, height);

        do {
            ASSERT_NO_FATAL_FAILURE(set_layer_properties(display, layers,
                    test_layers, &skip));
            if (skip)
                continue;

            ASSERT_NO_FATAL_FAILURE(wait_for_vsync());

            ASSERT_NO_FATAL_FAILURE(present_display(display, &present_fence, &err));
            EXPECT_EQ(err, HWC2_ERROR_NOT_VALIDATED) << "returned wrong error code";

        } while (test_layers.advance());

        ASSERT_NO_FATAL_FAILURE(destroy_layers(display, layers));
    }

    ASSERT_NO_FATAL_FAILURE(disable_vsync(display));
    ASSERT_NO_FATAL_FAILURE(set_power_mode(display, HWC2_POWER_MODE_OFF));
}

TEST_F(hwc2_test, GET_RELEASE_FENCES_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::vector<hwc2_layer_t> layers;
    std::vector<int32_t> fences;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_release_fences(display, &layers, &fences, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

static const std::array<android_color_mode, 9> android_color_modes = {{
    HAL_COLOR_MODE_NATIVE,
    HAL_COLOR_MODE_STANDARD_BT601_625,
    HAL_COLOR_MODE_STANDARD_BT601_625_UNADJUSTED,
    HAL_COLOR_MODE_STANDARD_BT601_525,
    HAL_COLOR_MODE_STANDARD_BT601_525_UNADJUSTED,
    HAL_COLOR_MODE_STANDARD_BT709,
    HAL_COLOR_MODE_DCI_P3,
    HAL_COLOR_MODE_SRGB,
    HAL_COLOR_MODE_ADOBE_RGB,
}};

TEST_F(hwc2_test, GET_COLOR_MODES)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    std::vector<android_color_mode_t> color_modes;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        ASSERT_NO_FATAL_FAILURE(get_color_modes(display, &color_modes));

        EXPECT_NE(std::find(color_modes.begin(), color_modes.end(),
                HAL_COLOR_MODE_NATIVE), color_modes.end()) << "all displays"
                " must support HAL_COLOR_MODE_NATIVE";

        color_modes.clear();
    }
}

TEST_F(hwc2_test, GET_COLOR_MODES_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::vector<android_color_mode_t> color_modes;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_color_modes(display, &color_modes, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_COLOR_MODES)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    android_color_mode_t color_mode = HAL_COLOR_MODE_NATIVE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        EXPECT_NO_FATAL_FAILURE(set_color_mode(display, color_mode));
    }
}

TEST_F(hwc2_test, SET_COLOR_MODES_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    android_color_mode_t color_mode = HAL_COLOR_MODE_NATIVE;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_color_mode(display, color_mode, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_COLOR_MODES_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    android_color_mode_t color_mode = static_cast<android_color_mode_t>(-1);
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        ASSERT_NO_FATAL_FAILURE(set_color_mode(display, color_mode, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_COLOR_MODES_unsupported)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        for (android_color_mode_t color_mode: android_color_modes) {
            ASSERT_NO_FATAL_FAILURE(set_color_mode(display, color_mode, &err));

            EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_UNSUPPORTED)
                    << "returned wrong error code";
        }
    }
}

TEST_F(hwc2_test, GET_HDR_CAPABILITIES)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    std::vector<android_hdr_t> hdr_capabilities;
    float max_luminance, max_average_luminance, min_luminance;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        EXPECT_NO_FATAL_FAILURE(get_hdr_capabilities(display, &hdr_capabilities,
                &max_luminance, &max_average_luminance, &min_luminance));

        if (hdr_capabilities.empty())
            continue;

        EXPECT_GE(max_luminance, max_average_luminance);
        EXPECT_GE(max_average_luminance, max_luminance);

        hdr_capabilities.clear();
    }
}

TEST_F(hwc2_test, GET_HDR_CAPABILITIES_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    std::vector<android_hdr_t> hdr_capabilities;
    float max_luminance, max_average_luminance, min_luminance;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_hdr_capabilities(display, &hdr_capabilities,
            &max_luminance, &max_average_luminance, &min_luminance, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

static const std::array<float, 16> identity_matrix = {{
    1.0,  0.0,  0.0,  0.0,
    0.0,  1.0,  0.0,  0.0,
    0.0,  0.0,  1.0,  0.0,
    0.0,  0.0,  0.0,  1.0,
}};

static const std::array<const std::array<float, 16>, 7>
        example_matrices = {{
    identity_matrix,
    {{ 0.0,  0.0,  0.0,  0.0,
       0.0,  0.0,  0.0,  0.0,
       0.0,  0.0,  0.0,  0.0,
       0.0,  0.0,  0.0,  1.0}},
    {{ 2.0,  0.0,  0.0,  0.0,
       0.0,  0.5,  0.0,  0.0,
       0.0,  0.0,  1.5,  0.0,
       0.0,  0.0,  0.0,  1.0}},
    {{ 1.0,  0.0,  0.0,  0.0,
       0.5,  1.0,  0.0,  0.0,
       0.0,  0.0,  1.0,  0.0,
       0.0,  0.0,  0.0,  1.0}},
    {{ 1.0,  0.0,  0.0,  0.0,
       0.0,  1.0,  0.0,  0.0,
       0.0,  0.0,  1.0,  0.0,
       5.0,  2.0,  9.0,  1.0}},
    {{ 1.0,  1.0,  1.0,  0.0,
       1.0,  1.0,  1.0,  0.0,
       1.0,  1.0,  1.0,  0.0,
       1.0,  1.0,  1.0,  1.0}},
    {{ 1.6, -0.5, -0.2,  0.0,
      -0.4,  2.3, -0.3,  0.0,
      -0.7, -0.1,  3.0,  0.0,
       0.9,  0.8,  1.6,  1.0}},
}};

TEST_F(hwc2_test, SET_COLOR_TRANSFORM)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    android_color_transform_t hint = HAL_COLOR_TRANSFORM_IDENTITY;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        EXPECT_NO_FATAL_FAILURE(set_color_transform(display, identity_matrix,
                hint));
    }
}

TEST_F(hwc2_test, SET_COLOR_TRANSFORM_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    android_color_transform_t hint = HAL_COLOR_TRANSFORM_IDENTITY;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(set_color_transform(display, identity_matrix, hint,
            &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";
}

TEST_F(hwc2_test, SET_COLOR_TRANSFORM_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    android_color_transform_t hint = static_cast<android_color_transform_t>(-1);
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        ASSERT_NO_FATAL_FAILURE(set_color_transform(display, identity_matrix,
                hint, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";
    }
}

TEST_F(hwc2_test, SET_COLOR_TRANSFORM_arbitrary_matrix)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    std::vector<hwc2_config_t> configs;
    android_color_transform_t hint = HAL_COLOR_TRANSFORM_ARBITRARY_MATRIX;

    ASSERT_NO_FATAL_FAILURE(get_display_configs(display, &configs));

    for (hwc2_config_t config: configs) {
        ASSERT_NO_FATAL_FAILURE(set_active_config(display, config));

        for (const std::array<float, 16> &matrix:
                example_matrices)
            EXPECT_NO_FATAL_FAILURE(set_color_transform(display, matrix, hint));
    }
}

TEST_F(hwc2_test, CREATE_DESTROY_VIRTUAL_DISPLAY)
{
    hwc2_display_t display;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_COMPLETE);
    android_pixel_format_t format, desired_format;
    hwc2_error_t err = HWC2_ERROR_NONE;

    do {
        const std::pair<uint32_t, uint32_t> &dimension =
                test_virtual_display.get_display_dimension();
        format = test_virtual_display.get_format();
        desired_format = format;

        ASSERT_NO_FATAL_FAILURE(create_virtual_display(dimension.first,
                dimension.second, &desired_format, &display, &err));

        EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_NO_RESOURCES
                || err == HWC2_ERROR_UNSUPPORTED) << "returned wrong error code";
        EXPECT_GE(desired_format, 0) << "invalid format";

        if (err == HWC2_ERROR_NONE)
            ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display));

    } while (test_virtual_display.advance());
}

TEST_F(hwc2_test, CREATE_DESTROY_VIRTUAL_DISPLAY_multiple)
{
    std::vector<hwc2_display_t> displays;
    hwc2_display_t display;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_COMPLETE);
    android_pixel_format_t format, desired_format;
    hwc2_error_t err = HWC2_ERROR_NONE;

    do {
        const std::pair<uint32_t, uint32_t> &dimension =
                test_virtual_display.get_display_dimension();
        format = test_virtual_display.get_format();
        desired_format = format;

        ASSERT_NO_FATAL_FAILURE(create_virtual_display(dimension.first,
                dimension.second, &desired_format, &display, &err));

        EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_NO_RESOURCES
                || err == HWC2_ERROR_UNSUPPORTED) << "returned wrong error code";
        EXPECT_GE(desired_format, 0) << "invalid format";

        if (err == HWC2_ERROR_NONE)
            displays.push_back(display);

    } while (test_virtual_display.advance());

    for (hwc2_display_t display: displays)
        EXPECT_NO_FATAL_FAILURE(destroy_virtual_display(display));
}

TEST_F(hwc2_test, DESTROY_VIRTUAL_DISPLAY_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_DEFAULT);
    android_pixel_format_t desired_format;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    do {
        const std::pair<uint32_t, uint32_t> &dimension =
                test_virtual_display.get_display_dimension();
        desired_format = test_virtual_display.get_format();

        ASSERT_NO_FATAL_FAILURE(create_virtual_display(dimension.first,
                dimension.second, &desired_format, &display, &err));
        EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_NO_RESOURCES
                || err == HWC2_ERROR_UNSUPPORTED);

        if (err != HWC2_ERROR_NONE)
            return;

        ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display + 1, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

        ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display));

        ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    } while (test_virtual_display.advance());
}

TEST_F(hwc2_test, DESTROY_VIRTUAL_DISPLAY_bad_parameter)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display, &err));
    EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER) << "returned wrong error code";
}

TEST_F(hwc2_test, GET_MAX_VIRTUAL_DISPLAY_COUNT)
{
    uint32_t max_cnt;

    ASSERT_NO_FATAL_FAILURE(get_max_virtual_display_count(&max_cnt));
}

TEST_F(hwc2_test, GET_MAX_VIRTUAL_DISPLAY_COUNT_duplicate)
{
    uint32_t max_cnt1, max_cnt2;

    ASSERT_NO_FATAL_FAILURE(get_max_virtual_display_count(&max_cnt1));
    ASSERT_NO_FATAL_FAILURE(get_max_virtual_display_count(&max_cnt2));

    EXPECT_EQ(max_cnt1, max_cnt2) << "returned two different max virtual"
            " display counts";
}

TEST_F(hwc2_test, GET_MAX_VIRTUAL_DISPLAY_COUNT_create_max)
{
    std::vector<hwc2_display_t> displays;
    hwc2_display_t display;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_COMPLETE);
    android_pixel_format_t desired_format;
    uint32_t max_cnt;
    hwc2_error_t err = HWC2_ERROR_NONE;

    ASSERT_NO_FATAL_FAILURE(get_max_virtual_display_count(&max_cnt));

    while (displays.size() < max_cnt) {
        const std::pair<uint32_t, uint32_t> &dimension =
                test_virtual_display.get_display_dimension();
        desired_format = test_virtual_display.get_format();

        ASSERT_NO_FATAL_FAILURE(create_virtual_display(dimension.first,
                dimension.second, &desired_format, &display, &err));

        EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_UNSUPPORTED)
                << "returned wrong error code";

        if (err == HWC2_ERROR_NONE)
            displays.push_back(display);

        if (!test_virtual_display.advance()) {
            if (displays.empty())
                break;
            test_virtual_display.reset();
        }
    }

    for (hwc2_display_t display: displays)
        EXPECT_NO_FATAL_FAILURE(destroy_virtual_display(display));
}

TEST_F(hwc2_test, SET_OUTPUT_BUFFER)
{
    hwc2_display_t display;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_COMPLETE);
    android_pixel_format_t desired_format;
    buffer_handle_t handle;
    android::base::unique_fd acquire_fence;
    hwc2_error_t err = HWC2_ERROR_NONE;

    do {
        const std::pair<uint32_t, uint32_t> &dimension =
                test_virtual_display.get_display_dimension();
        desired_format = test_virtual_display.get_format();

        ASSERT_NO_FATAL_FAILURE(create_virtual_display(dimension.first,
                dimension.second, &desired_format, &display, &err));

        EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_NO_RESOURCES
                || err == HWC2_ERROR_UNSUPPORTED)
                << "returned wrong error code";

        if (err != HWC2_ERROR_NONE)
            continue;

        if (test_virtual_display.get_buffer(&handle, &acquire_fence) >= 0)
            EXPECT_NO_FATAL_FAILURE(set_output_buffer(display, handle,
                    acquire_fence));

        ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display));

    } while (test_virtual_display.advance());
}

TEST_F(hwc2_test, SET_OUTPUT_BUFFER_bad_display)
{
    hwc2_display_t display = HWC_NUM_PHYSICAL_DISPLAY_TYPES + 1;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_COMPLETE);
    buffer_handle_t handle;
    android::base::unique_fd acquire_fence;
    hwc2_error_t err = HWC2_ERROR_NONE;

    do {
        if (test_virtual_display.get_buffer(&handle, &acquire_fence) < 0)
            continue;

        ASSERT_NO_FATAL_FAILURE(set_output_buffer(display, handle,
                acquire_fence, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_DISPLAY) << "returned wrong error code";

    } while (test_virtual_display.advance());
}

TEST_F(hwc2_test, SET_OUTPUT_BUFFER_bad_parameter)
{
    hwc2_display_t display;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_COMPLETE);
    android_pixel_format_t desired_format;
    buffer_handle_t handle = nullptr;
    uint32_t release_fence = -1;
    hwc2_error_t err = HWC2_ERROR_NONE;

    do {
        const std::pair<uint32_t, uint32_t> &dimension =
                test_virtual_display.get_display_dimension();
        desired_format = test_virtual_display.get_format();

        ASSERT_NO_FATAL_FAILURE(create_virtual_display(dimension.first,
                dimension.second, &desired_format, &display, &err));

        EXPECT_TRUE(err == HWC2_ERROR_NONE || err == HWC2_ERROR_NO_RESOURCES
                || err == HWC2_ERROR_UNSUPPORTED);

        if (err != HWC2_ERROR_NONE)
            continue;

        ASSERT_NO_FATAL_FAILURE(set_output_buffer(display, handle,
                release_fence, &err));
        EXPECT_EQ(err, HWC2_ERROR_BAD_PARAMETER)
                << "returned wrong error code";

        if (err == HWC2_ERROR_NONE)
            ASSERT_NO_FATAL_FAILURE(destroy_virtual_display(display));

    } while (test_virtual_display.advance());
}

TEST_F(hwc2_test, SET_OUTPUT_BUFFER_unsupported)
{
    hwc2_display_t display = HWC_DISPLAY_PRIMARY;
    hwc2_test_virtual_display test_virtual_display(HWC2_TEST_COVERAGE_COMPLETE);
    buffer_handle_t handle;
    android::base::unique_fd acquire_fence;
    hwc2_error_t err = HWC2_ERROR_NONE;

    do {
        if (test_virtual_display.get_buffer(&handle, &acquire_fence) < 0)
            continue;

        ASSERT_NO_FATAL_FAILURE(set_output_buffer(display, handle,
                acquire_fence, &err));
        EXPECT_EQ(err, HWC2_ERROR_UNSUPPORTED) << "returned wrong error code";

    } while (test_virtual_display.advance());
}
