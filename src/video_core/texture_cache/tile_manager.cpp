// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <string_view>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/depth_format.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 micro_tiled_mips;
    u32 num_mips;
    u32 num_texels;
    std::array<MipInfo, MAX_MIPS> mips;
};
static_assert(sizeof(TilingInfo) == 16 + sizeof(MipInfo) * MAX_MIPS);
static_assert(offsetof(TilingInfo, mips) == 16);

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

TileManager::TileManager(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, StreamBuffer& stream_buffer_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, stream_buffer{stream_buffer_} {
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

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, const bool is_tiler,
                                            const DepthConversion depth_conversion,
                                            const bool is_linear) {
    ASSERT(depth_conversion == DepthConversion::None || info.num_bits == 16);
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .depth_conversion = depth_conversion,
        .is_tiler = is_tiler,
        .is_linear = is_linear,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return *it->second;
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = !is_linear && AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 14> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }
    spec_data[12] = u32(depth_conversion);
    spec_data[13] = u32(is_linear);

    std::span<const u32> code;
    switch (info.num_bits) {
    case 8:
        code = is_macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                        : std::span<const u32>{TILING_MICRO_8_COMP};
        break;
    case 16:
        code = is_macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                        : std::span<const u32>{TILING_MICRO_16_COMP};
        break;
    case 32:
        code = is_macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                        : std::span<const u32>{TILING_MICRO_32_COMP};
        break;
    case 64:
        code = is_macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                        : std::span<const u32>{TILING_MICRO_64_COMP};
        break;
    case 96:
        code = is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
        break;
    case 128:
        code = is_macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                        : std::span<const u32>{TILING_MICRO_128_COMP};
        break;
    default:
        UNREACHABLE_MSG("Unsupported tiling pixel width {}", info.num_bits);
    }

    const auto module = Vulkan::CompileSPV(code, device);
    std::array<vk::SpecializationMapEntry, 14> spec_entries{};
    u32 spec_count = 0;
    const auto add_spec = [&](u32 constant_id) {
        spec_entries[spec_count++] = vk::SpecializationMapEntry{
            constant_id, constant_id * u32(sizeof(u32)), sizeof(u32)};
    };
    add_spec(0);
    add_spec(1);
    add_spec(2);
    add_spec(3);
    if (is_macro) {
        for (u32 constant_id = 4; constant_id < 12; ++constant_id) {
            add_spec(constant_id);
        }
    }
    add_spec(12);
    add_spec(13);

    const vk::SpecializationInfo specialization{
        .mapEntryCount = spec_count,
        .pMapEntries = spec_entries.data(),
        .dataSize = spec_data.size() * sizeof(u32),
        .pData = spec_data.data(),
    };

    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);

    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
        .pSpecializationInfo = &specialization,
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };

    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    const auto handle = *pipeline;
    tiling_pipelines.emplace(key, std::move(pipeline));
    device.destroyShaderModule(module);
    return handle;
}

static void FillTilingInfo(TilingInfo& params, const ImageInfo& info, u32 num_mips,
                           u32 num_texels) {
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = info.micro_tiled_mips;
    params.num_mips = num_mips;
    params.num_texels = num_texels;
    ASSERT_MSG(params.num_mips <= params.mips.size(), "Tiling shader has {} mips, maximum is {}",
               params.num_mips, params.mips.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max<u16>((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max<u16>((mip_info.height + 3) / 4, 1U);
        }
    }
}

static void DispatchTexels(const Vulkan::Instance& instance, vk::CommandBuffer cmdbuf,
                           u64 num_texels) {
    const auto& max_group_count = instance.MaxComputeWorkGroupCount();
    const auto dispatch =
        TryComputeDispatch2D(num_texels, 64, max_group_count[0], max_group_count[1]);
    ASSERT_MSG(dispatch.has_value(),
               "Tiling dispatch for {} texels exceeds device workgroup limits {}x{}", num_texels,
               max_group_count[0], max_group_count[1]);
    cmdbuf.dispatch(dispatch->x, dispatch->y, 1);
}

std::pair<const Buffer*, u64> TileManager::DetileImage(const VideoCore::Buffer* in_buffer,
                                                       u64 in_offset, const ImageInfo& info,
                                                       const vk::Format host_format) {
    const auto tiling_format = GetTilingFormat(info, host_format);
    const u32 guest_bytes_per_pixel = info.num_bits / 8;
    const u64 output_size = GuestToHostBytes(info.guest_size, guest_bytes_per_pixel, tiling_format);
    const bool is_linear = !info.props.is_tiled;
    if (!NeedsTilingPipeline(info.props.is_tiled, tiling_format.depth_conversion)) {
        return {in_buffer, in_offset};
    }
    ASSERT_MSG(in_offset <= in_buffer->SizeBytes() &&
                   info.guest_size <= in_buffer->SizeBytes() - in_offset,
               "Detiler input range [{:#x}, {:#x}) exceeds buffer size {:#x}", in_offset,
               u64{in_offset} + info.guest_size, in_buffer->SizeBytes());

    const u64 num_texels = info.guest_size / guest_bytes_per_pixel;
    ASSERT_MSG(num_texels <= std::numeric_limits<u32>::max(),
               "Detiler texel count {} exceeds the shader index range", num_texels);

    TilingInfo params{};
    FillTilingInfo(params, info, info.resources.levels, static_cast<u32>(num_texels));

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto staging = runtime.GetStagingPool().Request(output_size, MemoryType::DeviceLocal, 256,
                                                          false, true);

    scheduler.EndRendering();
    runtime.FlushBarriers();

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        GetTilingPipeline(info, false, tiling_format.depth_conversion, is_linear));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer->Handle(),
        .offset = in_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
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
    DispatchTexels(instance, cmdbuf, num_texels);

    runtime.AccessBuffer(in_buffer, in_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(staging.buffer, staging.offset, output_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);

    return {staging.buffer, staging.offset};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            const VideoCore::Buffer* out_buffer, u64 out_offset) {
    const auto& info = in_image.info;
    const auto tiling_format = GetTilingFormat(info, in_image.GetImageFormat());
    const u32 guest_bytes_per_pixel = info.num_bits / 8;
    const u64 host_buffer_size =
        GuestToHostBytes(info.guest_size, guest_bytes_per_pixel, tiling_format);
    const bool is_linear = !info.props.is_tiled;
    if (!NeedsTilingPipeline(info.props.is_tiled, tiling_format.depth_conversion)) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        runtime.DownloadImage(&in_image, out_buffer, buffer_copies);
        return;
    }

    const u64 num_texels = info.guest_size / guest_bytes_per_pixel;
    ASSERT_MSG(num_texels <= std::numeric_limits<u32>::max(),
               "Tiler texel count {} exceeds the shader index range", num_texels);
    ASSERT_MSG(out_offset <= out_buffer->SizeBytes() &&
                   info.guest_size <= out_buffer->SizeBytes() - out_offset,
               "Tiled image download range [{:#x}, {:#x}) exceeds buffer size {:#x}", out_offset,
               out_offset + info.guest_size, out_buffer->SizeBytes());

    TilingInfo params{};
    FillTilingInfo(params, info, static_cast<u32>(buffer_copies.size()),
                   static_cast<u32>(num_texels));

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto staging = runtime.GetStagingPool().Request(host_buffer_size, MemoryType::DeviceLocal,
                                                          256, false, true);
    for (auto& copy : buffer_copies) {
        copy.bufferOffset =
            GuestToHostBytes(copy.bufferOffset, guest_bytes_per_pixel, tiling_format) +
            staging.offset;
    }

    runtime.DownloadImage(&in_image, staging.buffer, buffer_copies);
    runtime.FlushBarriers();

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        GetTilingPipeline(info, true, tiling_format.depth_conversion, is_linear));

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer->Handle(),
        .offset = out_offset,
        .range = info.guest_size,
    };

    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
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
    DispatchTexels(instance, cmdbuf, num_texels);

    runtime.AccessBuffer(staging.buffer, staging.offset, host_buffer_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(out_buffer, out_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
}

} // namespace VideoCore
