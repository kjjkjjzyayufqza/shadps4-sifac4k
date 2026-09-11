// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/depth_staging.h"

namespace VideoCore {

inline TilingFormat GetDepthTilingFormat(vk::Format guest_format, vk::Format host_format,
                                         u32 guest_bytes_per_pixel) {
    const bool guest_is_d16 =
        guest_format == vk::Format::eD16Unorm || guest_format == vk::Format::eD16UnormS8Uint;
    if (guest_is_d16 && host_format == vk::Format::eD24UnormS8Uint) {
        return {4, DepthConversion::D16ToD24};
    }
    if (guest_is_d16 &&
        (host_format == vk::Format::eD32Sfloat || host_format == vk::Format::eD32SfloatS8Uint)) {
        return {4, DepthConversion::D16ToD32};
    }
    return {guest_bytes_per_pixel, DepthConversion::None};
}

} // namespace VideoCore
