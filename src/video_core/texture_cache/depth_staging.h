// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <optional>

#include "common/types.h"

namespace VideoCore {

enum class DepthConversion : u32 {
    None,
    D16ToD24,
    D16ToD32,
};

struct TilingFormat {
    u32 host_bytes_per_pixel;
    DepthConversion depth_conversion;
};

struct Dispatch2D {
    u32 x;
    u32 y;

    bool operator==(const Dispatch2D&) const = default;
};

[[nodiscard]] constexpr std::optional<u64> TryGuestToHostBytes(const u64 guest_bytes,
                                                               const u32 guest_bytes_per_pixel,
                                                               const TilingFormat& tiling_format) {
    if (guest_bytes_per_pixel == 0 || tiling_format.host_bytes_per_pixel == 0 ||
        guest_bytes % guest_bytes_per_pixel != 0) {
        return std::nullopt;
    }
    const u64 num_texels = guest_bytes / guest_bytes_per_pixel;
    if (num_texels > std::numeric_limits<u64>::max() / tiling_format.host_bytes_per_pixel) {
        return std::nullopt;
    }
    return num_texels * tiling_format.host_bytes_per_pixel;
}

[[nodiscard]] constexpr bool NeedsTilingPipeline(const bool is_tiled,
                                                 const DepthConversion conversion) {
    return is_tiled || conversion != DepthConversion::None;
}

[[nodiscard]] constexpr std::optional<Dispatch2D> TryComputeDispatch2D(
    const u64 num_invocations, const u32 local_size_x, const u32 max_group_count_x,
    const u32 max_group_count_y) {
    if (num_invocations == 0) {
        return Dispatch2D{};
    }
    if (local_size_x == 0 || max_group_count_x == 0 || max_group_count_y == 0) {
        return std::nullopt;
    }

    const u64 num_groups =
        num_invocations / local_size_x + static_cast<u64>(num_invocations % local_size_x != 0);
    const u64 groups_y =
        num_groups / max_group_count_x + static_cast<u64>(num_groups % max_group_count_x != 0);
    if (groups_y > max_group_count_y) {
        return std::nullopt;
    }
    const u64 groups_x = num_groups / groups_y + static_cast<u64>(num_groups % groups_y != 0);
    return Dispatch2D{.x = static_cast<u32>(groups_x), .y = static_cast<u32>(groups_y)};
}

[[nodiscard]] constexpr u32 D16ToD24(const u16 depth) {
    return (u32{depth} << 8) + (u32{depth} + 128u) / 257u;
}

[[nodiscard]] constexpr u16 D24ToD16(const u32 depth) {
    constexpr u64 D16Max = std::numeric_limits<u16>::max();
    constexpr u64 D24Max = 0x00ffffff;
    return static_cast<u16>(((depth & D24Max) * D16Max + D24Max / 2) / D24Max);
}

} // namespace VideoCore
