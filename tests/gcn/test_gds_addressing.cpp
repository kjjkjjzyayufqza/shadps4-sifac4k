// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>

#include <gtest/gtest.h>

#include "gcn_test_runner.hpp"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"

namespace {
using namespace Shader;

void CheckAppendConsumeAddresses(u32 m0, bool dynamic_m0) {
    Info info{};
    info.stage = Stage::Compute;
    info.l_stage = LogicalStage::Compute;
    RuntimeInfo runtime_info{};
    runtime_info.Initialize(Stage::Compute);
    runtime_info.cs_info.workgroup_size = {64, 1, 1};
    Profile profile{};
    Pools pools;
    IR::Program program{info};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.post_order_blocks = IR::PostOrder(block);
    Gcn::Translator translator{info, runtime_info, profile};
    translator.EmitPrologue(block);
    IR::IREmitter ir{*block};
    const auto m0_value = dynamic_m0 ? ir.GetUserData(IR::ScalarReg::S0) : ir.Imm32(m0);
    ir.SetM0(m0_value);

    constexpr std::array offsets{0U, 4U, 8U, 12U, 28U, 0x104U};
    for (const auto opcode : {Gcn::OpcodeDS::DS_APPEND, Gcn::OpcodeDS::DS_CONSUME}) {
        for (const auto offset : offsets) {
            const std::array<u32, 2> words{
                0xd8000000U | (static_cast<u32>(opcode) << 18) | (1U << 17) | offset, 0U};
            Gcn::GcnCodeSlice slice{words.data(), words.data() + words.size()};
            Gcn::GcnDecodeContext decoder;
            translator.TranslateInstruction(decoder.decodeInstruction(slice));
        }
    }
    Optimization::SsaRewritePass(program);
    Optimization::ResourceDiscoveryList resources;
    for (auto& inst : *block) {
        if (Optimization::IsDataRingInstruction(inst)) {
            resources.push_back({.user = &inst});
        }
    }
    ASSERT_EQ(resources.size(), 2 * offsets.size());
    Optimization::ResourcePatchingPass(info, resources, profile);
    if (dynamic_m0) {
        m0_value.Inst()->ReplaceUsesWith(ir.Imm32(m0));
    }
    Optimization::ConstantPropagationPass(program.blocks);

    ASSERT_EQ(info.buffers.size(), 1U);
    EXPECT_EQ(info.buffers[0].buffer_type, BufferType::GdsBuffer);
    EXPECT_TRUE(info.buffers[0].is_written);
    for (size_t i = 0; i < resources.size(); ++i) {
        const auto& inst = *resources[i].user;
        const u32 expected_index = ((m0 >> 16) + offsets[i % offsets.size()]) / sizeof(u32);
        SCOPED_TRACE(::testing::Message() << "m0=" << m0 << " operation=" << i);
        ASSERT_TRUE(inst.Arg(0).IsImmediate());
        EXPECT_EQ(inst.Arg(0).U32(), expected_index);
        EXPECT_EQ(inst.Arg(1).U32(), 0U);
    }
}

TEST(GdsAddressing, AdjacentAppendConsumeCounters) {
    for (const u32 m0 : {0x000003ffU, 0x00401234U, 0x1000ffffU}) {
        CheckAppendConsumeAddresses(m0, false);
    }
}

TEST(GdsAddressing, DynamicM0AppendConsumeCounters) {
    for (const u32 m0 : {0x0000ffffU, 0x00400000U, 0x100003ffU}) {
        CheckAppendConsumeAddresses(m0, true);
    }
}

TEST(GdsAddressing, SharedAccessStillConvertsBytesToDwords) {
    Info info{};
    Profile profile{};
    Pools pools;
    IR::Program program{info};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    IR::IREmitter ir{*block};
    const auto value = ir.LoadShared(32, false, ir.Imm32(0x104U), true);
    const auto observer = ir.IAdd(IR::U32{value}, ir.Imm32(1U));
    Optimization::ResourceDiscoveryList resources{{.user = value.Inst()}};
    Optimization::ResourcePatchingPass(info, resources, profile);
    Optimization::ConstantPropagationPass(program.blocks);
    const auto* load = observer.Inst()->Arg(0).Inst();
    ASSERT_NE(load, nullptr);
    ASSERT_EQ(load->GetOpcode(), IR::Opcode::LoadBufferU32);
    EXPECT_EQ(load->Arg(1).U32(), 0x41U);
}

TEST(GdsAddressing, SubgroupAppendConsumeIndices) {
    Info info{};
    info.stage = Stage::Compute;
    info.l_stage = LogicalStage::Compute;
    info.buffers.push_back({.used_types = IR::Type::U32,
                           .buffer_type = BufferType::GdsBuffer,
                           .is_written = true});
    RuntimeInfo runtime_info{};
    runtime_info.Initialize(Stage::Compute);
    runtime_info.cs_info.workgroup_size = {64, 1, 1};
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.subgroup_size = 32;
    profile.support_int64 = true;
    Pools pools;
    IR::Program program{info};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.post_order_blocks = IR::PostOrder(block);
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    IR::IREmitter ir{*block};
    ir.Prologue();
    const auto tid = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const auto ballot = ir.UnpackUint2x32(ir.Ballot(ir.Imm1(true)));
    const auto low_mask = ir.GetAttributeU32(IR::Attribute::SubgroupLtMask, 0);
    const auto high_mask = ir.GetAttributeU32(IR::Attribute::SubgroupLtMask, 1);
    const auto rank = ir.IAdd(
        ir.BitCount(ir.BitwiseAnd(IR::U32{ir.CompositeExtract(ballot, 0)}, low_mask)),
        ir.BitCount(ir.BitwiseAnd(IR::U32{ir.CompositeExtract(ballot, 1)}, high_mask)));
    const auto append = ir.DataAppend(ir.Imm32(0U));
    ir.StoreBufferU32(1, ir.Imm32(0U), ir.IAdd(tid, ir.Imm32(32U)), ir.IAdd(append, rank), {});
    ir.WorkgroupMemoryBarrier();
    ir.Barrier();
    const auto consume = ir.DataConsume(ir.Imm32(0U));
    const auto index = ir.ISub(ir.ISub(consume, rank), ir.Imm32(1U));
    ir.StoreBufferU32(1, ir.Imm32(0U), ir.IAdd(tid, ir.Imm32(96U)), index, {});
    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
    auto runner = gcn_test::Runner::instance();
    ASSERT_TRUE(runner.has_value());
    const auto output = (*runner)->run<std::array<u32, 160>>(spirv);
    gcn_test::Runner::DestroyInstance();
    ASSERT_TRUE(output.has_value());
    EXPECT_EQ((*output)[0], 0U);
    auto sorted = *output;
    std::sort(sorted.begin() + 32, sorted.begin() + 96);
    std::sort(sorted.begin() + 96, sorted.end());
    for (u32 i = 0; i < 64; ++i) {
        EXPECT_EQ(sorted[32 + i], i);
        EXPECT_EQ(sorted[96 + i], i);
    }
}

} // namespace
