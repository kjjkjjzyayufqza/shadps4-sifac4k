// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/vk_image_format.h"
#include "video_core/texture_cache/depth_format.h"

namespace Vulkan {
namespace {

vk::ImageCreateInfo DepthRequest(vk::Format format = vk::Format::eD16UnormS8Uint) {
    return {.flags =
                vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {128, 64, 1},
            .mipLevels = 1,
            .arrayLayers = 2,
            .samples = vk::SampleCountFlagBits::e4,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eTransferDst};
}

ImageFormatSupport Supported() {
    return {
        .result = vk::Result::eSuccess,
        .properties = {.maxExtent = {4096, 4096, 1},
                       .maxMipLevels = 13,
                       .maxArrayLayers = 256,
                       .sampleCounts = vk::SampleCountFlagBits::e1 | vk::SampleCountFlagBits::e4,
                       .maxResourceSize = 1ULL << 32}};
}

TEST(ImageFormat, NativeD16RemainsTwoBytesWhenSupported) {
    for (const auto guest : {vk::Format::eD16Unorm, vk::Format::eD16UnormS8Uint}) {
        unsigned int queries{}, creates{};
        const auto selected = TryCreateDepthImage(
            DepthRequest(guest),
            [&](const auto&) {
                ++queries;
                return Supported();
            },
            [&](const auto&) {
                ++creates;
                return vk::Result::eSuccess;
            });
        ASSERT_EQ(selected.result, vk::Result::eSuccess);
        EXPECT_EQ(selected.format, guest);
        EXPECT_EQ(queries, 1);
        EXPECT_EQ(creates, 1);
        const auto layout = VideoCore::GetDepthTilingFormat(guest, selected.format, 2);
        EXPECT_EQ(layout.host_bytes_per_pixel, 2);
        EXPECT_EQ(layout.depth_conversion, VideoCore::DepthConversion::None);
    }
}

TEST(ImageFormat, UnsupportedNativeSelectsD24AndConvertsStaging) {
    const auto selected = TryCreateDepthImage(
        DepthRequest(),
        [](const auto& info) {
            return info.format == vk::Format::eD16UnormS8Uint ? ImageFormatSupport{} : Supported();
        },
        [](const auto&) { return vk::Result::eSuccess; });
    ASSERT_EQ(selected.result, vk::Result::eSuccess);
    EXPECT_EQ(selected.format, vk::Format::eD24UnormS8Uint);
    const auto layout = VideoCore::GetDepthTilingFormat(DepthRequest().format, selected.format, 2);
    EXPECT_EQ(layout.host_bytes_per_pixel, 4);
    EXPECT_EQ(layout.depth_conversion, VideoCore::DepthConversion::D16ToD24);
}

TEST(ImageFormat, UnsupportedD24SelectsD32WithStencilIntact) {
    const auto selected = TryCreateDepthImage(
        DepthRequest(),
        [](const auto& info) {
            return info.format == vk::Format::eD32SfloatS8Uint ? Supported() : ImageFormatSupport{};
        },
        [](const auto&) { return vk::Result::eSuccess; });
    ASSERT_EQ(selected.result, vk::Result::eSuccess);
    EXPECT_EQ(selected.format, vk::Format::eD32SfloatS8Uint);
    const auto layout = VideoCore::GetDepthTilingFormat(DepthRequest().format, selected.format, 2);
    EXPECT_EQ(layout.host_bytes_per_pixel, 4);
    EXPECT_EQ(layout.depth_conversion, VideoCore::DepthConversion::D16ToD32);
}

TEST(ImageFormat, PlainD16FallsBackWithoutAddingStencil) {
    const auto selected = TryCreateDepthImage(
        DepthRequest(vk::Format::eD16Unorm),
        [](const auto& info) {
            return info.format == vk::Format::eD32Sfloat ? Supported() : ImageFormatSupport{};
        },
        [](const auto&) { return vk::Result::eSuccess; });
    EXPECT_EQ(selected.format, vk::Format::eD32Sfloat);
}

TEST(ImageFormat, ChecksExtentLevelsLayersAndSamplesBeforeCreation) {
    for (unsigned int constraint = 0; constraint < 6; ++constraint) {
        std::vector<vk::Format> created;
        const auto selected = TryCreateDepthImage(
            DepthRequest(),
            [&](const auto& info) {
                auto support = Supported();
                if (info.format == vk::Format::eD16UnormS8Uint) {
                    switch (constraint) {
                    case 0:
                        support.properties.maxExtent.width = 64;
                        break;
                    case 1:
                        support.properties.maxExtent.height = 32;
                        break;
                    case 2:
                        support.properties.maxExtent.depth = 0;
                        break;
                    case 3:
                        support.properties.maxMipLevels = 0;
                        break;
                    case 4:
                        support.properties.maxArrayLayers = 1;
                        break;
                    case 5:
                        support.properties.sampleCounts = vk::SampleCountFlagBits::e1;
                        break;
                    }
                }
                return support;
            },
            [&](const auto& info) {
                created.push_back(info.format);
                return vk::Result::eSuccess;
            });
        EXPECT_EQ(selected.format, vk::Format::eD24UnormS8Uint);
        ASSERT_EQ(created.size(), 1);
        EXPECT_EQ(created.front(), vk::Format::eD24UnormS8Uint);
    }
}

TEST(ImageFormat, PreservesActualImageParametersAcrossCandidates) {
    const auto requested = DepthRequest();
    unsigned int queries{};
    const auto selected = TryCreateDepthImage(
        requested,
        [&](const auto& info) {
            ++queries;
            EXPECT_EQ(info.flags, requested.flags);
            EXPECT_EQ(info.usage, requested.usage);
            EXPECT_EQ(info.imageType, requested.imageType);
            EXPECT_EQ(info.tiling, requested.tiling);
            EXPECT_EQ(info.extent, requested.extent);
            EXPECT_EQ(info.samples, requested.samples);
            EXPECT_EQ(info.mipLevels, requested.mipLevels);
            EXPECT_EQ(info.arrayLayers, requested.arrayLayers);
            return info.format == vk::Format::eD32SfloatS8Uint ? Supported() : ImageFormatSupport{};
        },
        [](const auto&) { return vk::Result::eSuccess; });
    EXPECT_EQ(queries, 3);
    EXPECT_EQ(selected.format, vk::Format::eD32SfloatS8Uint);
}

TEST(ImageFormat, ExplicitCreationFormatFailureTriesNextCandidate) {
    unsigned int attempts{};
    const auto selected = TryCreateDepthImage(
        DepthRequest(), [](const auto&) { return Supported(); },
        [&](const auto& info) {
            ++attempts;
            return info.format == vk::Format::eD32SfloatS8Uint
                       ? vk::Result::eSuccess
                       : vk::Result::eErrorFormatNotSupported;
        });
    EXPECT_EQ(attempts, 3);
    EXPECT_EQ(selected.format, vk::Format::eD32SfloatS8Uint);
}

TEST(ImageFormat, CreationResourceAndDeviceErrorsAreNotRetried) {
    for (const auto error : {vk::Result::eErrorOutOfHostMemory, vk::Result::eErrorOutOfDeviceMemory,
                             vk::Result::eErrorDeviceLost, vk::Result::eErrorUnknown}) {
        unsigned int attempts{};
        const auto selected = TryCreateDepthImage(
            DepthRequest(), [](const auto&) { return Supported(); },
            [&](const auto&) {
                ++attempts;
                return error;
            });
        EXPECT_EQ(selected.result, error);
        EXPECT_EQ(attempts, 1);
        EXPECT_EQ(selected.format, vk::Format::eUndefined);
    }
}

TEST(ImageFormat, QueryErrorsDoNotBecomeFormatFailures) {
    unsigned int queries{}, creates{};
    const auto selected = TryCreateDepthImage(
        DepthRequest(),
        [&](const auto&) {
            ++queries;
            return ImageFormatSupport{.result = vk::Result::eErrorOutOfHostMemory};
        },
        [&](const auto&) {
            ++creates;
            return vk::Result::eSuccess;
        });
    EXPECT_EQ(selected.result, vk::Result::eErrorOutOfHostMemory);
    EXPECT_EQ(queries, 1);
    EXPECT_EQ(creates, 0);
}

TEST(ImageFormat, NoUsableCandidateDoesNotCreateAnUnsupportedImage) {
    unsigned int creates{};
    const auto selected = TryCreateDepthImage(
        DepthRequest(), [](const auto&) { return ImageFormatSupport{}; },
        [&](const auto&) {
            ++creates;
            return vk::Result::eSuccess;
        });
    EXPECT_EQ(selected.result, vk::Result::eErrorFormatNotSupported);
    EXPECT_EQ(creates, 0);
}

TEST(ImageFormat, DoesNotReduceSamplesToHideMissingSupport) {
    unsigned int creates{};
    const auto selected = TryCreateDepthImage(
        DepthRequest(),
        [](const auto&) {
            auto support = Supported();
            support.properties.sampleCounts = vk::SampleCountFlagBits::e1;
            return support;
        },
        [&](const auto&) {
            ++creates;
            return vk::Result::eSuccess;
        });
    EXPECT_EQ(selected.result, vk::Result::eErrorFormatNotSupported);
    EXPECT_EQ(creates, 0);
}

} // namespace
} // namespace Vulkan
