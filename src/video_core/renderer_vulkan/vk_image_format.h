// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

struct ImageFormatSupport {
    vk::Result result{vk::Result::eErrorFormatNotSupported};
    vk::ImageFormatProperties properties{};
};

struct ImageFormatSelection {
    vk::Result result{vk::Result::eErrorFormatNotSupported};
    vk::Format format{vk::Format::eUndefined};
    vk::SampleCountFlags supported_samples{};
};

constexpr std::array<vk::Format, 3> DepthFormatCandidates(vk::Format requested) {
    switch (requested) {
    case vk::Format::eD16UnormS8Uint:
        return {requested, vk::Format::eD24UnormS8Uint, vk::Format::eD32SfloatS8Uint};
    case vk::Format::eD16Unorm:
        return {requested, vk::Format::eD32Sfloat, vk::Format::eUndefined};
    default:
        return {requested, vk::Format::eUndefined, vk::Format::eUndefined};
    }
}

inline bool FitsImageFormat(const vk::ImageCreateInfo& info,
                            const vk::ImageFormatProperties& properties) {
    return info.extent.width <= properties.maxExtent.width &&
           info.extent.height <= properties.maxExtent.height &&
           info.extent.depth <= properties.maxExtent.depth &&
           info.mipLevels <= properties.maxMipLevels &&
           info.arrayLayers <= properties.maxArrayLayers &&
           static_cast<bool>(properties.sampleCounts & info.samples);
}

template <typename Query, typename Create>
ImageFormatSelection TryCreateDepthImage(const vk::ImageCreateInfo& requested, Query&& query,
                                         Create&& create) {
    for (const auto format : DepthFormatCandidates(requested.format)) {
        if (format == vk::Format::eUndefined) {
            break;
        }
        auto candidate = requested;
        candidate.format = format;
        const auto support = query(candidate);
        if (support.result == vk::Result::eErrorFormatNotSupported) {
            continue;
        }
        if (support.result != vk::Result::eSuccess) {
            return {.result = support.result};
        }
        if (!FitsImageFormat(candidate, support.properties)) {
            continue;
        }
        const auto result = create(candidate);
        if (result == vk::Result::eErrorFormatNotSupported) {
            continue;
        }
        if (result != vk::Result::eSuccess) {
            return {.result = result};
        }
        return {.result = result,
                .format = format,
                .supported_samples = support.properties.sampleCounts};
    }
    return {};
}

} // namespace Vulkan
