// SPDX-FileCopyrightText: Copyright (C) 2001-2024 Free Software Foundation, Inc.
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>
#include <unordered_map>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/loader/dwarf.h"

namespace Dwarf {

template <typename T>
T get(uintptr_t addr) {
    T val;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(T));
    return val;
}

// Windows static guest red-zone protection
static size_t GetEncodedSize(u8 encoding) {
    switch (encoding & DW_EH_PE_format_mask) {
    case DW_EH_PE_ptr:
        return sizeof(uintptr_t);
    case DW_EH_PE_udata2:
    case DW_EH_PE_sdata2:
        return sizeof(u16);
    case DW_EH_PE_udata4:
    case DW_EH_PE_sdata4:
        return sizeof(u32);
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8:
        return sizeof(u64);
    default:
        return 0;
    }
}

static uintptr_t getEncodedP(uintptr_t& addr, uintptr_t end, u8 encoding, uintptr_t datarelBase) {
    const uintptr_t startAddr = addr;
    const u8* p = (u8*)addr;
    uintptr_t result;

    // First get value
    switch (encoding & DW_EH_PE_format_mask) { // Windows static guest red-zone protection
    case DW_EH_PE_ptr:
        result = get<uintptr_t>(addr);
        p += sizeof(uintptr_t);
        addr = (uintptr_t)p;
        break;
    case DW_EH_PE_udata2:
        result = get<u16>(addr);
        p += sizeof(u16);
        addr = (uintptr_t)p;
        break;
    case DW_EH_PE_udata4:
        result = get<u32>(addr);
        p += sizeof(u32);
        addr = (uintptr_t)p;
        break;
    case DW_EH_PE_udata8:
        result = get<u64>(addr);
        p += sizeof(u64);
        addr = (uintptr_t)p;
        break;
    case DW_EH_PE_sdata2:
        // Sign extend from signed 16-bit value.
        result = get<s16>(addr);
        p += sizeof(s16);
        addr = (uintptr_t)p;
        break;
    case DW_EH_PE_sdata4:
        // Sign extend from signed 32-bit value.
        result = get<s32>(addr);
        p += sizeof(s32);
        addr = (uintptr_t)p;
        break;
    case DW_EH_PE_sdata8:
        result = get<s64>(addr);
        p += sizeof(s64);
        addr = (uintptr_t)p;
        break;
    default:
        UNREACHABLE_MSG("unknown pointer encoding");
    }

    // Then add relative offset
    switch (encoding & DW_EH_PE_application_mask) { // Windows static guest red-zone protection
    case DW_EH_PE_absptr:
        // do nothing
        break;
    case DW_EH_PE_pcrel:
        result += startAddr;
        break;
    case DW_EH_PE_textrel:
        UNREACHABLE_MSG("DW_EH_PE_textrel pointer encoding not supported");
        break;
    case DW_EH_PE_datarel:
        // DW_EH_PE_datarel is only valid in a few places, so the parameter has a
        // default value of 0, and we abort in the event that someone calls this
        // function with a datarelBase of 0 and DW_EH_PE_datarel encoding.
        if (datarelBase == 0)
            UNREACHABLE_MSG("DW_EH_PE_datarel is invalid with a datarelBase of 0");
        result += datarelBase;
        break;
    case DW_EH_PE_funcrel:
        UNREACHABLE_MSG("DW_EH_PE_funcrel pointer encoding not supported");
        break;
    case DW_EH_PE_aligned:
        UNREACHABLE_MSG("DW_EH_PE_aligned pointer encoding not supported");
        break;
    default:
        UNREACHABLE_MSG("unknown pointer encoding");
        break;
    }

    if (encoding & DW_EH_PE_indirect) {
        result = get<uintptr_t>(result);
    }

    return result;
}

bool DecodeEHHdr(uintptr_t ehHdrStart, uintptr_t ehHdrEnd, EHHeaderInfo& ehHdrInfo) {
    auto p = ehHdrStart;
    // Ensure that we don't read data beyond the end of .eh_frame_hdr
    if (ehHdrEnd - ehHdrStart < 4) {
        // Don't print a message for an empty .eh_frame_hdr (this can happen if
        // the linker script defines symbols for it even in the empty case).
        if (ehHdrEnd == ehHdrStart) {
            return false;
        }
        LOG_ERROR(Core_Linker,
                  "Unsupported .eh_frame_hdr at {:#x} "
                  "need at least 4 bytes of data but only got {:#x}",
                  ehHdrStart, ehHdrEnd - ehHdrStart);
        return false;
    }

    const u8 version = get<u8>(p++);
    if (version != 1) {
        LOG_CRITICAL(Core_Linker, "Unsupported .eh_frame_hdr version: {:#x} at {:#x}", version,
                     ehHdrStart);
        return false;
    }

    const u8 eh_frame_ptr_enc = get<u8>(p++);
    const u8 fde_count_enc = get<u8>(p++);
    ehHdrInfo.table_enc = get<u8>(p++);

    ehHdrInfo.eh_frame_ptr = getEncodedP(p, ehHdrEnd, eh_frame_ptr_enc, ehHdrStart);
    ehHdrInfo.fde_count =
        fde_count_enc == DW_EH_PE_omit ? 0 : getEncodedP(p, ehHdrEnd, fde_count_enc, ehHdrStart);
    ehHdrInfo.table = p;
    ehHdrInfo.datarel_base = ehHdrStart; // Windows static guest red-zone protection

    return true;
}

// Windows static guest red-zone protection
bool DecodeEHHdrTable(const EHHeaderInfo& ehHdrInfo, uintptr_t ehHdrEnd,
                      std::vector<uintptr_t>& functionStarts) {
    functionStarts.clear();
    if (ehHdrInfo.fde_count == 0 || ehHdrInfo.table_enc == DW_EH_PE_omit) {
        return true;
    }

    const size_t encoded_size = GetEncodedSize(ehHdrInfo.table_enc);
    if (encoded_size == 0 || ehHdrInfo.table > ehHdrEnd) {
        return false;
    }

    const size_t entry_size = encoded_size * 2;
    const size_t remaining_size = ehHdrEnd - ehHdrInfo.table;
    if (ehHdrInfo.fde_count > remaining_size / entry_size) {
        return false;
    }

    functionStarts.reserve(ehHdrInfo.fde_count);
    uintptr_t cursor = ehHdrInfo.table;
    for (size_t index = 0; index < ehHdrInfo.fde_count; ++index) {
        functionStarts.push_back(
            getEncodedP(cursor, ehHdrEnd, ehHdrInfo.table_enc, ehHdrInfo.datarel_base));
        getEncodedP(cursor, ehHdrEnd, ehHdrInfo.table_enc, ehHdrInfo.datarel_base);
    }
    return true;
}

namespace {

/// Bounds-checked cursor over one .eh_frame record in the loaded image. Every read fails
/// instead of running past the record, so a corrupt length cannot walk into other data.
class RecordReader {
public:
    RecordReader(uintptr_t cursor_, uintptr_t start_, uintptr_t end_)
        : cursor{cursor_}, start{start_}, end{end_} {}

    [[nodiscard]] uintptr_t Position() const {
        return cursor;
    }

    template <typename T>
    [[nodiscard]] bool Read(T& value) {
        if (!Has(sizeof(T))) {
            return false;
        }
        value = get<T>(cursor);
        cursor += sizeof(T);
        return true;
    }

    [[nodiscard]] bool Skip(size_t size) {
        if (!Has(size)) {
            return false;
        }
        cursor += size;
        return true;
    }

    [[nodiscard]] bool ReadULEB128(u64& value) {
        value = 0;
        for (u32 shift = 0; shift < 64; shift += 7) {
            u8 byte;
            if (!Read(byte)) {
                return false;
            }
            value |= static_cast<u64>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool SkipLEB128() {
        u64 ignored;
        return ReadULEB128(ignored);
    }

    [[nodiscard]] bool ReadString(std::string_view& value) {
        for (uintptr_t scan = cursor; scan >= start && scan < end; ++scan) {
            if (get<u8>(scan) == 0) {
                value = {reinterpret_cast<const char*>(cursor), scan - cursor};
                cursor = scan + 1;
                return true;
            }
        }
        return false;
    }

    /// Reads a pointer in one of the fixed-size formats, applied absolutely or relative to
    /// the field itself. Those are the only forms an FDE's pc_begin and pc_range take in
    /// practice; anything else is refused rather than guessed at.
    [[nodiscard]] bool ReadEncoded(u8 encoding, uintptr_t& value) {
        const uintptr_t field = cursor;
        u64 raw;
        switch (encoding & DW_EH_PE_format_mask) {
        case DW_EH_PE_ptr:
        case DW_EH_PE_udata8:
        case DW_EH_PE_sdata8:
            if (!Read(raw)) {
                return false;
            }
            break;
        case DW_EH_PE_udata2: {
            u16 narrow;
            if (!Read(narrow)) {
                return false;
            }
            raw = narrow;
            break;
        }
        case DW_EH_PE_sdata2: {
            s16 narrow;
            if (!Read(narrow)) {
                return false;
            }
            raw = static_cast<u64>(static_cast<s64>(narrow));
            break;
        }
        case DW_EH_PE_udata4: {
            u32 narrow;
            if (!Read(narrow)) {
                return false;
            }
            raw = narrow;
            break;
        }
        case DW_EH_PE_sdata4: {
            s32 narrow;
            if (!Read(narrow)) {
                return false;
            }
            raw = static_cast<u64>(static_cast<s64>(narrow));
            break;
        }
        default:
            return false;
        }

        if ((encoding & DW_EH_PE_indirect) != 0) {
            return false;
        }
        switch (encoding & DW_EH_PE_application_mask) {
        case DW_EH_PE_absptr:
            break;
        case DW_EH_PE_pcrel:
            raw += field;
            break;
        default:
            return false;
        }
        value = static_cast<uintptr_t>(raw);
        return true;
    }

    /// Reads a record's length field and bounds this reader to the record it introduces.
    /// Only the 32-bit form is accepted: a zero length is a terminator, not a record, and
    /// 64-bit records do not occur in the modules this serves.
    [[nodiscard]] bool EnterRecord() {
        u32 length;
        if (!Read(length) || length == 0 || length == 0xffffffff || !Has(length)) {
            return false;
        }
        end = cursor + length;
        return true;
    }

private:
    [[nodiscard]] bool Has(size_t size) const {
        return cursor >= start && cursor <= end && end - cursor >= size;
    }

    uintptr_t cursor;
    uintptr_t start;
    uintptr_t end;
};

/// Reads the pointer encoding a CIE prescribes for the pc_begin and pc_range of its FDEs.
bool DecodeCieFdeEncoding(uintptr_t cie, uintptr_t imageStart, uintptr_t imageEnd,
                          u8& fdeEncoding) {
    RecordReader reader{cie, imageStart, imageEnd};
    u32 cie_id;
    u8 version;
    std::string_view augmentation;
    if (!reader.EnterRecord() || !reader.Read(cie_id) || cie_id != 0 || !reader.Read(version) ||
        (version != 1 && version != 3) || !reader.ReadString(augmentation) ||
        !reader.SkipLEB128() || !reader.SkipLEB128()) {
        return false;
    }
    if (version == 1 ? !reader.Skip(sizeof(u8)) : !reader.SkipLEB128()) {
        return false;
    }

    fdeEncoding = DW_EH_PE_absptr;
    if (augmentation.empty()) {
        return true;
    }
    u64 augmentation_size;
    if (augmentation.front() != 'z' || !reader.ReadULEB128(augmentation_size)) {
        return false;
    }
    const uintptr_t augmentation_end = reader.Position() + augmentation_size;
    for (const char field : augmentation.substr(1)) {
        switch (field) {
        case 'R':
            if (!reader.Read(fdeEncoding)) {
                return false;
            }
            break;
        case 'L':
            if (!reader.Skip(sizeof(u8))) {
                return false;
            }
            break;
        case 'P': {
            u8 personality_encoding;
            if (!reader.Read(personality_encoding)) {
                return false;
            }
            const u8 format = personality_encoding & DW_EH_PE_format_mask;
            if (format == DW_EH_PE_uleb128 || format == DW_EH_PE_sleb128) {
                if (!reader.SkipLEB128()) {
                    return false;
                }
                break;
            }
            const size_t size = GetEncodedSize(personality_encoding);
            if (size == 0 || !reader.Skip(size)) {
                return false;
            }
            break;
        }
        case 'S':
        case 'B':
            break;
        default:
            return false;
        }
    }
    return reader.Position() <= augmentation_end;
}

/// Reads the body one FDE describes. CIE encodings are cached, since every FDE of a
/// compilation unit shares one CIE.
bool DecodeFde(uintptr_t fde, uintptr_t imageStart, uintptr_t imageEnd,
               std::unordered_map<uintptr_t, u8>& cieEncodings, FunctionBody& body) {
    RecordReader reader{fde, imageStart, imageEnd};
    if (!reader.EnterRecord()) {
        return false;
    }
    const uintptr_t cie_pointer_field = reader.Position();
    u32 cie_offset;
    if (!reader.Read(cie_offset) || cie_offset == 0 || cie_offset > cie_pointer_field) {
        return false;
    }
    const uintptr_t cie = cie_pointer_field - cie_offset;

    auto encoding = cieEncodings.find(cie);
    if (encoding == cieEncodings.end()) {
        u8 decoded_encoding;
        if (!DecodeCieFdeEncoding(cie, imageStart, imageEnd, decoded_encoding)) {
            return false;
        }
        encoding = cieEncodings.emplace(cie, decoded_encoding).first;
    }

    uintptr_t pc_begin;
    uintptr_t pc_range;
    if (!reader.ReadEncoded(encoding->second, pc_begin) ||
        !reader.ReadEncoded(encoding->second & DW_EH_PE_format_mask, pc_range) ||
        pc_begin + pc_range < pc_begin) {
        return false;
    }
    body = {.start = pc_begin, .end = pc_begin + pc_range};
    return true;
}

} // namespace

bool DecodeEHFunctionBodies(const EHHeaderInfo& ehHdrInfo, uintptr_t ehHdrEnd, uintptr_t imageStart,
                            uintptr_t imageEnd, std::vector<FunctionBody>& bodies) {
    bodies.clear();
    if (ehHdrInfo.fde_count == 0 || ehHdrInfo.table_enc == DW_EH_PE_omit) {
        return true;
    }

    // The same checks DecodeEHHdrTable makes, plus refusing the table encodings that
    // getEncodedP only answers by aborting.
    const size_t encoded_size = GetEncodedSize(ehHdrInfo.table_enc);
    const u8 table_application = ehHdrInfo.table_enc & DW_EH_PE_application_mask;
    if (encoded_size == 0 || ehHdrInfo.table > ehHdrEnd ||
        ehHdrInfo.fde_count > (ehHdrEnd - ehHdrInfo.table) / (encoded_size * 2) ||
        (table_application != DW_EH_PE_absptr && table_application != DW_EH_PE_pcrel &&
         table_application != DW_EH_PE_datarel)) {
        return false;
    }

    std::unordered_map<uintptr_t, u8> cie_encodings;
    bodies.reserve(ehHdrInfo.fde_count);
    uintptr_t cursor = ehHdrInfo.table;
    for (size_t index = 0; index < ehHdrInfo.fde_count; ++index) {
        const uintptr_t start =
            getEncodedP(cursor, ehHdrEnd, ehHdrInfo.table_enc, ehHdrInfo.datarel_base);
        const uintptr_t fde =
            getEncodedP(cursor, ehHdrEnd, ehHdrInfo.table_enc, ehHdrInfo.datarel_base);
        FunctionBody body;
        if (!DecodeFde(fde, imageStart, imageEnd, cie_encodings, body) || body.start != start) {
            bodies.clear();
            return false;
        }
        bodies.push_back(body);
    }
    return true;
}

} // namespace Dwarf
