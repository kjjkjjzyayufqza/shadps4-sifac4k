// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include <gtest/gtest.h>

#include "video_core/texture_cache/depth_staging.h"

namespace VideoCore {
namespace {

TEST(DepthStaging, D16ToD24MapsEndpointsAndMatchesNearestReference) {
    for (u32 depth = 0; depth <= std::numeric_limits<u16>::max(); ++depth) {
        const auto expected =
            static_cast<u32>((u64{depth} * 0x00ffffffu + std::numeric_limits<u16>::max() / 2) /
                             std::numeric_limits<u16>::max());
        ASSERT_EQ(D16ToD24(static_cast<u16>(depth)), expected) << "depth=" << depth;
    }
}

TEST(DepthStaging, D24ToD16MasksUnusedHighByteAndMatchesNearestReference) {
    for (u32 depth = 0; depth <= 0x00ffffffu; ++depth) {
        const auto expected = static_cast<u16>(
            (u64{depth} * std::numeric_limits<u16>::max() + 0x007fffffu) / 0x00ffffffu);
        ASSERT_EQ(D24ToD16(depth), expected) << "depth=" << depth;
    }
    EXPECT_EQ(D24ToD16(0xff000000u), 0u);
    EXPECT_EQ(D24ToD16(0xffffffffu), std::numeric_limits<u16>::max());
}

TEST(DepthStaging, D16D24RoundTripIsExact) {
    for (u32 depth = 0; depth <= std::numeric_limits<u16>::max(); ++depth) {
        ASSERT_EQ(D24ToD16(D16ToD24(static_cast<u16>(depth))), depth);
    }
}

TEST(DepthStaging, GuestToHostByteScalingIsChecked) {
    const TilingFormat native{.host_bytes_per_pixel = 2, .depth_conversion = DepthConversion::None};
    const TilingFormat widened{.host_bytes_per_pixel = 4,
                               .depth_conversion = DepthConversion::D16ToD24};

    EXPECT_EQ(TryGuestToHostBytes(0, 2, widened), 0u);
    EXPECT_EQ(TryGuestToHostBytes(8192, 2, native), 8192u);
    EXPECT_EQ(TryGuestToHostBytes(8192, 2, widened), 16384u);
    EXPECT_EQ(TryGuestToHostBytes(6, 2, widened), 12u);
    EXPECT_FALSE(TryGuestToHostBytes(4, 0, widened));
    EXPECT_FALSE(TryGuestToHostBytes(4, 2, TilingFormat{}));
    EXPECT_FALSE(TryGuestToHostBytes(3, 2, widened));

    constexpr u64 MaxTexels = std::numeric_limits<u64>::max() / 4;
    EXPECT_EQ(TryGuestToHostBytes(MaxTexels * 2, 2, widened), MaxTexels * 4);
    EXPECT_FALSE(TryGuestToHostBytes((MaxTexels + 1) * 2, 2, widened));
}

TEST(DepthStaging, TilingPipelineSelectionCoversLinearAndTiledConversions) {
    EXPECT_FALSE(NeedsTilingPipeline(false, DepthConversion::None));
    EXPECT_TRUE(NeedsTilingPipeline(true, DepthConversion::None));
    EXPECT_TRUE(NeedsTilingPipeline(false, DepthConversion::D16ToD24));
    EXPECT_TRUE(NeedsTilingPipeline(false, DepthConversion::D16ToD32));
    EXPECT_TRUE(NeedsTilingPipeline(true, DepthConversion::D16ToD24));
    EXPECT_TRUE(NeedsTilingPipeline(true, DepthConversion::D16ToD32));
}

TEST(DepthStaging, ComputeDispatchRespectsDeviceLimits) {
    EXPECT_EQ(TryComputeDispatch2D(0, 64, 65535, 65535), (Dispatch2D{.x = 0, .y = 0}));
    EXPECT_EQ(TryComputeDispatch2D(1, 64, 65535, 65535), (Dispatch2D{.x = 1, .y = 1}));
    EXPECT_EQ(TryComputeDispatch2D(64, 64, 65535, 65535), (Dispatch2D{.x = 1, .y = 1}));
    EXPECT_EQ(TryComputeDispatch2D(2048u * 2048u, 64, 65535, 65535),
              (Dispatch2D{.x = 32768, .y = 2}));
    EXPECT_EQ(TryComputeDispatch2D(65, 64, 1, 2), (Dispatch2D{.x = 1, .y = 2}));
    EXPECT_FALSE(TryComputeDispatch2D(65, 64, 1, 1));
    EXPECT_FALSE(TryComputeDispatch2D(1, 0, 65535, 65535));

    const auto max_u32_dispatch =
        TryComputeDispatch2D(std::numeric_limits<u32>::max(), 64, std::numeric_limits<u16>::max(),
                             std::numeric_limits<u16>::max());
    ASSERT_TRUE(max_u32_dispatch.has_value());
    EXPECT_GE(u64{max_u32_dispatch->x} * max_u32_dispatch->y * 64, std::numeric_limits<u32>::max());

    constexpr u64 Capacity = u64{3} * 5 * 64;
    EXPECT_FALSE(TryComputeDispatch2D(Capacity + 1, 64, 3, 5));
}

} // Anonymous namespace
} // namespace VideoCore
