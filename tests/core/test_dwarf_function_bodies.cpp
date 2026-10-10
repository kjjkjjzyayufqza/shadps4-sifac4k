// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The unwind search table says where each body starts; only its FDE says where it ends. The
// static rewrite pass bounds its search for estimates the walk never reached by that end, and
// data can sit between one body and the next, so a wrong end either misses code or reports
// data as code. Every malformed input therefore has to yield no bodies at all.

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "common/types.h"
#include "core/loader/dwarf.h"

namespace {

constexpr u8 PcRelSData4 = Dwarf::DW_EH_PE_pcrel | Dwarf::DW_EH_PE_sdata4;
constexpr u8 DataRelSData4 = Dwarf::DW_EH_PE_datarel | Dwarf::DW_EH_PE_sdata4;

/// A module image holding .eh_frame, .eh_frame_hdr and the code ranges they describe.
class UnwindImage {
public:
    static constexpr size_t CodeOffset = 0x300;

    struct Body {
        size_t start;
        size_t size;
    };

    UnwindImage() : bytes(0x400) {
        const size_t plain_cie = PutCie(0x000, "zR", false);
        const size_t personality_cie = PutCie(0x060, "zPLR", true);
        fde_offsets = {
            PutFde(0x020, plain_cie, bodies[0], false),
            PutFde(0x040, plain_cie, bodies[1], false),
            PutFde(0x0a0, personality_cie, bodies[2], true),
        };
        PutHeader();
    }

    [[nodiscard]] uintptr_t Address(size_t offset) const {
        return reinterpret_cast<uintptr_t>(bytes.data()) + offset;
    }

    [[nodiscard]] bool Decode(std::vector<Dwarf::FunctionBody>& out) const {
        Dwarf::EHHeaderInfo info;
        const uintptr_t header_end = Address(header_offset + header_size);
        if (!Dwarf::DecodeEHHdr(Address(header_offset), header_end, info)) {
            return false;
        }
        return Dwarf::DecodeEHFunctionBodies(info, header_end, Address(0), Address(bytes.size()),
                                             out);
    }

    void Put8At(size_t offset, u8 value) {
        bytes[offset] = value;
    }

    void Put32At(size_t offset, u32 value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    [[nodiscard]] u32 Get32At(size_t offset) const {
        u32 value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    /// Offset of the search table's first recorded start.
    [[nodiscard]] size_t FirstTableStart() const {
        return header_offset + 12;
    }

    const std::array<Body, 3> bodies = {{
        {CodeOffset + 0x00, 0x40},
        {CodeOffset + 0x40, 0x20},
        {CodeOffset + 0x60, 0x10},
    }};
    std::array<size_t, 3> fde_offsets{};
    size_t augmentation_offset{};

private:
    size_t PutCie(size_t offset, std::string_view augmentation, bool with_personality) {
        size_t cursor = offset + 4;
        Put32(cursor, 0); // CIE id
        Put8(cursor, 1);  // version
        if (!with_personality) {
            augmentation_offset = cursor;
        }
        for (const char c : augmentation) {
            Put8(cursor, static_cast<u8>(c));
        }
        Put8(cursor, 0);
        Put8(cursor, 1);    // code alignment factor, ULEB128
        Put8(cursor, 0x78); // data alignment factor -8, SLEB128
        Put8(cursor, 16);   // return address register
        if (with_personality) {
            Put8(cursor, 7); // augmentation data size
            Put8(cursor, Dwarf::DW_EH_PE_indirect | PcRelSData4);
            Put32(cursor, 0x1234);     // personality pointer, never dereferenced
            Put8(cursor, PcRelSData4); // LSDA encoding
            Put8(cursor, PcRelSData4); // FDE encoding
        } else {
            Put8(cursor, 1);
            Put8(cursor, PcRelSData4);
        }
        FinishRecord(offset, cursor);
        return offset;
    }

    size_t PutFde(size_t offset, size_t cie, const Body& body, bool with_lsda) {
        size_t cursor = offset + 4;
        Put32(cursor, static_cast<u32>(cursor - cie));
        const size_t pc_begin_field = cursor;
        Put32(cursor, static_cast<u32>(static_cast<s32>(body.start - pc_begin_field)));
        Put32(cursor, static_cast<u32>(body.size));
        if (with_lsda) {
            Put8(cursor, 4);
            Put32(cursor, 0);
        } else {
            Put8(cursor, 0);
        }
        FinishRecord(offset, cursor);
        return offset;
    }

    void PutHeader() {
        size_t cursor = header_offset;
        Put8(cursor, 1);
        Put8(cursor, PcRelSData4);            // eh_frame_ptr encoding
        Put8(cursor, Dwarf::DW_EH_PE_udata4); // fde_count encoding
        Put8(cursor, DataRelSData4);          // table encoding
        Put32(cursor, static_cast<u32>(static_cast<s32>(0 - static_cast<s64>(cursor))));
        Put32(cursor, static_cast<u32>(fde_offsets.size()));
        for (size_t index = 0; index < fde_offsets.size(); ++index) {
            Put32(cursor, static_cast<u32>(bodies[index].start - header_offset));
            Put32(cursor, static_cast<u32>(fde_offsets[index] - header_offset));
        }
        header_size = cursor - header_offset;
    }

    void FinishRecord(size_t offset, size_t& cursor) {
        while ((cursor - offset) % 4 != 0) {
            Put8(cursor, 0); // DW_CFA_nop
        }
        Put32At(offset, static_cast<u32>(cursor - offset - 4));
    }

    void Put8(size_t& cursor, u8 value) {
        bytes[cursor++] = value;
    }

    void Put32(size_t& cursor, u32 value) {
        Put32At(cursor, value);
        cursor += sizeof(value);
    }

    static constexpr size_t header_offset = 0x100;
    size_t header_size{};
    std::vector<u8> bytes;
};

} // namespace

TEST(DwarfFunctionBodies, ReadsTheExtentOfEveryBodyWhateverItsCieCarries) {
    const UnwindImage image;
    std::vector<Dwarf::FunctionBody> bodies;
    ASSERT_TRUE(image.Decode(bodies));
    ASSERT_EQ(image.bodies.size(), bodies.size());
    for (size_t index = 0; index < bodies.size(); ++index) {
        SCOPED_TRACE(index);
        EXPECT_EQ(image.Address(image.bodies[index].start), bodies[index].start);
        EXPECT_EQ(image.Address(image.bodies[index].start + image.bodies[index].size),
                  bodies[index].end);
    }
}

// The search table and the FDE it points at must name the same start; if they disagree one of
// them is wrong, and nothing says which.
TEST(DwarfFunctionBodies, StartTheTableAndTheFdeDisagreeOnIsRefused) {
    UnwindImage image;
    image.Put32At(image.FirstTableStart(), image.Get32At(image.FirstTableStart()) + 1);
    std::vector<Dwarf::FunctionBody> bodies{{.start = 1, .end = 2}};
    EXPECT_FALSE(image.Decode(bodies));
    EXPECT_TRUE(bodies.empty());
}

TEST(DwarfFunctionBodies, RecordRunningPastTheImageIsRefused) {
    UnwindImage image;
    image.Put32At(image.fde_offsets[2], 0x1000);
    std::vector<Dwarf::FunctionBody> bodies;
    EXPECT_FALSE(image.Decode(bodies));
    EXPECT_TRUE(bodies.empty());
}

// An augmentation this reader does not know may carry data before the FDE encoding, so the
// encoding it would report is a guess.
TEST(DwarfFunctionBodies, UnknownCieAugmentationIsRefused) {
    UnwindImage image;
    image.Put8At(image.augmentation_offset + 1, 'X'); // "zR" becomes "zX"
    std::vector<Dwarf::FunctionBody> bodies;
    EXPECT_FALSE(image.Decode(bodies));
    EXPECT_TRUE(bodies.empty());
}
