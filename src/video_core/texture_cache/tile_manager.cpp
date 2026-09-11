// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <cstddef>
#include <limits>
#include <string_view>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/depth_format.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_comp.h"

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    u32 num_texels;
    std::array<ImageInfo::MipInfo, 16> mips;
};
static_assert(sizeof(TilingInfo) == 272);
static_assert(offsetof(TilingInfo, mips) == 16);

[[nodiscard]] constexpr u32 BitsPerPixelId(const u32 num_bits) {
    switch (num_bits) {
    case 8:
        return 0;
    case 16:
        return 1;
    case 32:
        return 2;
    case 64:
        return 3;
    case 96:
        return 4;
    case 128:
        return 5;
    default:
        UNREACHABLE_MSG("Unsupported image bits per pixel {}", num_bits);
    }
}

void ValidateStorageDescriptor(const Vulkan::Instance& instance, const vk::DeviceSize offset,
                               const vk::DeviceSize range, const std::string_view name) {
    ASSERT_MSG(range > 0, "{} storage descriptor has an empty range", name);
    ASSERT_MSG(offset % instance.StorageMinAlignment() == 0,
               "{} storage descriptor offset {:#x} is not aligned to {:#x}", name, offset,
               instance.StorageMinAlignment());
    ASSERT_MSG(range <= instance.StorageMaxSize(),
               "{} storage descriptor range {:#x} exceeds the device limit {:#x}", name, range,
               instance.StorageMaxSize());
}

TileManager::TileManager(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         StreamBuffer& stream_buffer_)
    : instance{instance}, scheduler{scheduler}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);
}

TileManager::~TileManager() = default;

TileManager::ScratchBuffer TileManager::GetScratchBuffer(vk::DeviceSize size) {
    constexpr auto usage =
        vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;

    const vk::BufferCreateInfo buffer_ci = {
        .size = size,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info{
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkBuffer buffer;
    VmaAllocation allocation;
    const auto buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
    const auto result = vmaCreateBuffer(instance.GetAllocator(), &buffer_ci_unsafe, &alloc_info,
                                        &buffer, &allocation, nullptr);
    ASSERT(result == VK_SUCCESS);
    Vulkan::SetObjectName(instance.GetDevice(), vk::Buffer{buffer}, "Tile buffer");
    return {buffer, allocation};
}

TilingFormat GetTilingFormat(const ImageInfo& info, const vk::Format host_format) {
    const u32 guest_bytes_per_pixel = info.num_bits / 8;
    if (!info.props.is_depth) {
        return {.host_bytes_per_pixel = guest_bytes_per_pixel,
                .depth_conversion = DepthConversion::None};
    }

    // Buffer-image copies address a depth aspect using the host format's depth plane. When a
    // combined D16/S8 attachment falls back to D24/S8 or D32/S8, the staging layout and values
    // must be widened before the copy instead of treating the guest's 16-bit data as 32-bit.
    return GetDepthTilingFormat(info.pixel_format, host_format, guest_bytes_per_pixel);
}

u64 GuestToHostBytes(const u64 guest_bytes, const u32 guest_bytes_per_pixel,
                     const TilingFormat& tiling_format) {
    const auto host_bytes = TryGuestToHostBytes(guest_bytes, guest_bytes_per_pixel, tiling_format);
    ASSERT_MSG(host_bytes.has_value(),
               "Invalid host image byte size conversion: guest_bytes={}, guest_bpp={}, host_bpp={}",
               guest_bytes, guest_bytes_per_pixel, tiling_format.host_bytes_per_pixel);
    return host_bytes.value_or(0);
}

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, const bool is_tiler,
                                            const DepthConversion depth_conversion,
                                            const bool is_linear) {
    ASSERT_MSG(std::has_single_bit(info.num_samples), "Invalid sample count {}", info.num_samples);
    const u32 sample_id = std::countr_zero(info.num_samples);
    ASSERT_MSG(sample_id < NUM_SAMPLE_COUNTS, "Unsupported sample count {}", info.num_samples);
    const u32 pipeline_id = u32(info.tile_mode) * NUM_BPPS + BitsPerPixelId(info.num_bits);
    const u32 pl_id = ((pipeline_id * NUM_SAMPLE_COUNTS + sample_id) * NUM_DEPTH_CONVERSIONS +
                       u32(depth_conversion)) *
                          NUM_PIPELINE_LAYOUTS +
                      u32(is_linear);
    auto& tiling_pipelines = is_tiler ? tilers : detilers;
    if (auto pipeline = *tiling_pipelines[pl_id]; pipeline != VK_NULL_HANDLE) {
        return pipeline;
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const u32 linear_bits_per_pixel =
        depth_conversion == DepthConversion::None ? info.num_bits : 32;
    std::vector<std::string> defines = {
        fmt::format("BITS_PER_PIXEL={}", info.num_bits),
        fmt::format("LINEAR_BITS_PER_PIXEL={}", linear_bits_per_pixel),
        fmt::format("DEPTH_CONVERSION={}", u32(depth_conversion)),
        fmt::format("NUM_SAMPLES={}", info.num_samples),
        fmt::format("ARRAY_MODE={}", u32(info.array_mode)),
        fmt::format("MICRO_TILE_MODE={}", u32(micro_tile_mode)),
        fmt::format("MICRO_TILE_THICKNESS={}", AmdGpu::GetMicroTileThickness(info.array_mode)),
    };
    if (is_linear) {
        defines.emplace_back("IS_LINEAR=1");
    }
    if (AmdGpu::IsMacroTiled(info.array_mode)) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        const u32 num_banks = AmdGpu::GetNumBanks(macro_tile_mode);
        defines.emplace_back(
            fmt::format("PIPE_CONFIG={}", u32(AmdGpu::GetPipeConfig(info.tile_mode))));
        defines.emplace_back(fmt::format("BANK_WIDTH={}", AmdGpu::GetBankWidth(macro_tile_mode)));
        defines.emplace_back(fmt::format("BANK_HEIGHT={}", AmdGpu::GetBankHeight(macro_tile_mode)));
        defines.emplace_back(fmt::format("NUM_BANKS={}", num_banks));
        defines.emplace_back(fmt::format("NUM_BANK_BITS={}", std::bit_width(num_banks) - 1));
        defines.emplace_back(fmt::format(
            "TILE_SPLIT_BYTES={}", AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode,
                                                              micro_tile_mode, info.num_bits)));
        defines.emplace_back(
            fmt::format("MACRO_TILE_ASPECT={}", AmdGpu::GetMacrotileAspect(macro_tile_mode)));
    }
    if (is_tiler) {
        defines.emplace_back(fmt::format("IS_TILER=1"));
    }

    const auto& module = Vulkan::Compile(HostShaders::TILING_COMP,
                                         vk::ShaderStageFlagBits::eCompute, device, defines);
    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Compiling shader {}", module_name);
    for (const auto& def : defines) {
        LOG_INFO(Render_Vulkan, "#define {}", def);
    }
    Vulkan::SetObjectName(device, module, module_name);
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };
    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    tiling_pipelines[pl_id] = std::move(pipeline);
    device.destroyShaderModule(module);
    return *tiling_pipelines[pl_id];
}

TileManager::Result TileManager::DetileImage(Buffer& in_buffer, u32 in_offset,
                                             const ImageInfo& info, const vk::Format host_format) {
    const auto tiling_format = GetTilingFormat(info, host_format);
    const u64 output_size = GuestToHostBytes(info.guest_size, info.num_bits / 8, tiling_format);
    const bool is_linear = !info.props.is_tiled;
    if (!NeedsTilingPipeline(info.props.is_tiled, tiling_format.depth_conversion)) {
        return {in_buffer.Handle(), in_offset};
    }
    ASSERT_MSG(in_offset <= in_buffer.SizeBytes() &&
                   info.guest_size <= in_buffer.SizeBytes() - in_offset,
               "Detiler input range [{:#x}, {:#x}) exceeds buffer size {:#x}", in_offset,
               u64{in_offset} + info.guest_size, in_buffer.SizeBytes());

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    ASSERT_MSG(params.num_mips <= params.mips.size(), "Detiler has {} mips, maximum is {}",
               params.num_mips, params.mips.size());
    const u64 num_texels = info.guest_size / (info.num_bits / 8);
    ASSERT_MSG(num_texels <= std::numeric_limits<u32>::max(),
               "Detiler texel count {} exceeds the shader index range", num_texels);
    params.num_texels = static_cast<u32>(num_texels);
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [out_buffer, out_allocation] = GetScratchBuffer(output_size);
    scheduler.DeferOperation([this, out_buffer, out_allocation]() {
        vmaDestroyBuffer(instance.GetAllocator(), out_buffer, out_allocation);
    });

    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        GetTilingPipeline(info, false, tiling_format.depth_conversion, is_linear));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer.Handle(),
        .offset = in_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = out_buffer,
        .offset = 0,
        .range = output_size,
    };
    ValidateStorageDescriptor(instance, tiled_buffer_info.offset, tiled_buffer_info.range,
                              "Detiler input");
    ValidateStorageDescriptor(instance, linear_buffer_info.offset, linear_buffer_info.range,
                              "Detiler output");

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);

    const auto& max_group_count = instance.MaxComputeWorkGroupCount();
    const auto dispatch =
        TryComputeDispatch2D(num_texels, 64, max_group_count[0], max_group_count[1]);
    ASSERT_MSG(dispatch.has_value(),
               "Detiler dispatch for {} texels exceeds device workgroup limits {}x{}", num_texels,
               max_group_count[0], max_group_count[1]);
    const auto [dim_x, dim_y] = dispatch.value_or(Dispatch2D{});
    cmdbuf.dispatch(dim_x, dim_y, 1);
    return {out_buffer, 0};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            Buffer& out_buffer, const u64 out_offset, const u64 copy_size) {
    const auto& info = in_image.info;
    ASSERT_MSG(out_offset <= out_buffer.SizeBytes() &&
                   copy_size <= out_buffer.SizeBytes() - out_offset,
               "Tiled image download range [{:#x}, {:#x}) exceeds buffer size {:#x}", out_offset,
               out_offset + copy_size, out_buffer.SizeBytes());
    const auto tiling_format = GetTilingFormat(info, in_image.GetImageFormat());
    const u32 guest_bytes_per_pixel = info.num_bits / 8;
    const u64 host_buffer_size = GuestToHostBytes(copy_size, guest_bytes_per_pixel, tiling_format);
    std::vector<vk::BufferImageCopy> host_copies;
    host_copies.reserve(buffer_copies.size());
    for (const auto& copy : buffer_copies) {
        auto host_copy = copy;
        host_copy.bufferOffset =
            GuestToHostBytes(copy.bufferOffset, guest_bytes_per_pixel, tiling_format);
        host_copies.push_back(host_copy);
    }
    if (!NeedsTilingPipeline(info.props.is_tiled, tiling_format.depth_conversion)) {
        if (auto barrier = out_buffer.GetBarrier(vk::AccessFlagBits2::eTransferWrite,
                                                 vk::PipelineStageFlagBits2::eCopy, out_offset)) {
            scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
                .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers = &barrier.value(),
            });
        }
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        in_image.Download(buffer_copies, out_buffer.Handle(), out_offset, copy_size);
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    ASSERT_MSG(params.num_mips <= params.mips.size(), "Tiler has {} mips, maximum is {}",
               params.num_mips, params.mips.size());
    const u64 num_texels = copy_size / guest_bytes_per_pixel;
    ASSERT_MSG(num_texels <= std::numeric_limits<u32>::max(),
               "Tiler texel count {} exceeds the shader index range", num_texels);
    params.num_texels = static_cast<u32>(num_texels);
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto [temp_buffer, temp_allocation] = GetScratchBuffer(host_buffer_size);
    scheduler.DeferOperation([this, temp_buffer, temp_allocation]() {
        vmaDestroyBuffer(instance.GetAllocator(), temp_buffer, temp_allocation);
    });

    const auto cmdbuf = scheduler.CommandBuffer();
    in_image.Download(host_copies, temp_buffer, 0, host_buffer_size);

    if (auto barrier =
            out_buffer.GetBarrier(vk::AccessFlagBits2::eShaderWrite,
                                  vk::PipelineStageFlagBits2::eComputeShader, out_offset)) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier.value(),
        });
    }

    cmdbuf.bindPipeline(
        vk::PipelineBindPoint::eCompute,
        GetTilingPipeline(info, true, tiling_format.depth_conversion, !info.props.is_tiled));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer.Handle(),
        .offset = out_offset,
        .range = copy_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = temp_buffer,
        .offset = 0,
        .range = host_buffer_size,
    };
    ValidateStorageDescriptor(instance, tiled_buffer_info.offset, tiled_buffer_info.range,
                              "Tiler output");
    ValidateStorageDescriptor(instance, linear_buffer_info.offset, linear_buffer_info.range,
                              "Tiler input");

    const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &tiled_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .pBufferInfo = &linear_buffer_info,
        },
        {
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &params_buffer_info,
        },
    }};
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pl_layout, 0, set_writes);

    const auto& max_group_count = instance.MaxComputeWorkGroupCount();
    const auto dispatch =
        TryComputeDispatch2D(num_texels, 64, max_group_count[0], max_group_count[1]);
    ASSERT_MSG(dispatch.has_value(),
               "Tiler dispatch for {} texels exceeds device workgroup limits {}x{}", num_texels,
               max_group_count[0], max_group_count[1]);
    const auto [dim_x, dim_y] = dispatch.value_or(Dispatch2D{});
    cmdbuf.dispatch(dim_x, dim_y, 1);
}

} // namespace VideoCore
