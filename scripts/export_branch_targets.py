# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Exports verified branch targets for a guest module, for the static rewrite pass.

Why this exists
---------------
shadPS4 rewrites guest instructions ahead of time. An instruction shorter than a near
jump has to borrow bytes from its neighbours, which is only safe when every address that
control flow can enter is known: a branch landing inside a borrowed range would land in
the middle of the jump that replaced it.

At runtime the emulator recovers branch targets itself, but it gives up on a function
whose indirect jump it cannot resolve, and then refuses to borrow anything in that
function. That refusal is what leaves estimate instructions unpatched in Gundam Versus
and Maxi Boost ON, and it is not something the emulator can fix on its own: a jump table
holds offsets relative to a base the emulator cannot identify, so no amount of scanning
proves an address is *not* a target.

A disassembler that recovers switch tables can prove it. This script exports what it
found so the emulator can borrow neighbours in exactly the functions where the target set
is known to be complete, and nowhere else.

Bodies are partitioned by `.eh_frame_hdr`, not by the disassembler's own idea of where
functions begin. The emulator partitions code by the unwind table, so an export
partitioned any other way describes bodies it never walks, and a body spanning two of the
disassembler's functions would be refused for holding code no entry covers.

Usage
-----
    idat -A -S"export_branch_targets.py <output-path>" <database>

The emulator loads the result from `<user>/branch_targets/<title serial>/<module>.txt`;
the serial is part of the path because every title's main module is called eboot.bin.
"""

import struct

import idaapi
import idautils
import idc

import ida_bytes
import ida_funcs
import ida_nalt
import ida_segment
import ida_ua
import ida_xref

FORMAT_VERSION = 1

DW_EH_PE_OMIT = 0xFF
DW_EH_PE_ABSPTR = 0x00
DW_EH_PE_UDATA2 = 0x02
DW_EH_PE_UDATA4 = 0x03
DW_EH_PE_UDATA8 = 0x04
DW_EH_PE_SDATA2 = 0x0A
DW_EH_PE_SDATA4 = 0x0B
DW_EH_PE_SDATA8 = 0x0C
DW_EH_PE_PCREL = 0x10
DW_EH_PE_DATAREL = 0x30

PT_GNU_EH_FRAME = 0x6474E550

_FORMATS = {
    DW_EH_PE_ABSPTR: ("<Q", 8),
    DW_EH_PE_UDATA2: ("<H", 2),
    DW_EH_PE_UDATA4: ("<I", 4),
    DW_EH_PE_UDATA8: ("<Q", 8),
    DW_EH_PE_SDATA2: ("<h", 2),
    DW_EH_PE_SDATA4: ("<i", 4),
    DW_EH_PE_SDATA8: ("<q", 8),
}


class UnsupportedEncoding(Exception):
    """An `.eh_frame_hdr` encoding this exporter does not decode.

    Raised rather than guessed at: a misread table gives the wrong body boundaries, and
    the emulator would then trust target sets for bodies they do not cover.
    """


def read_encoded(data, offset, encoding, datarel_base, pc_base):
    """Decodes one pointer, mirroring Dwarf::getEncodedP in src/core/loader/dwarf.cpp."""
    layout = _FORMATS.get(encoding & 0x0F)
    if layout is None:
        raise UnsupportedEncoding("value format %#x" % (encoding & 0x0F))

    fmt, size = layout
    (value,) = struct.unpack_from(fmt, data, offset)
    offset += size

    modifier = encoding & 0x70
    if modifier == DW_EH_PE_PCREL:
        value += pc_base
    elif modifier == DW_EH_PE_DATAREL:
        value += datarel_base
    elif modifier != DW_EH_PE_ABSPTR:
        raise UnsupportedEncoding("modifier %#x" % modifier)

    if encoding & 0x80:
        raise UnsupportedEncoding("indirect pointers")
    return value & 0xFFFFFFFFFFFFFFFF, offset


def eh_frame_function_starts(path):
    """Body entry addresses from the module's `.eh_frame_hdr` search table.

    Returns virtual addresses, which the caller shifts by the image base. This is the same
    table `Dwarf::DecodeEHHdrTable` reads, so the partition matches the one the rewrite
    pass walks.
    """
    with open(path, "rb") as handle:
        image = handle.read()

    if image[:6] != b"\x7fELF\x02\x01":
        raise UnsupportedEncoding("not a little-endian 64-bit ELF")

    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", image, 54)

    header = None
    for index in range(phnum):
        entry = phoff + index * phentsize
        p_type = struct.unpack_from("<I", image, entry)[0]
        if p_type != PT_GNU_EH_FRAME:
            continue
        p_offset, p_vaddr = struct.unpack_from("<QQ", image, entry + 8)
        p_filesz = struct.unpack_from("<Q", image, entry + 32)[0]
        header = (p_offset, p_vaddr, p_filesz)
        break
    if header is None:
        raise UnsupportedEncoding("no PT_GNU_EH_FRAME segment")

    file_offset, vaddr, size = header
    table = image[file_offset : file_offset + size]
    if len(table) < 4:
        raise UnsupportedEncoding("eh_frame_hdr too short")

    if table[0] != 1:
        raise UnsupportedEncoding("eh_frame_hdr version %d" % table[0])
    eh_frame_ptr_enc, fde_count_enc, table_enc = table[1], table[2], table[3]

    cursor = 4
    if eh_frame_ptr_enc != DW_EH_PE_OMIT:
        _, cursor = read_encoded(table, cursor, eh_frame_ptr_enc, vaddr, vaddr + cursor)
    if fde_count_enc == DW_EH_PE_OMIT or table_enc == DW_EH_PE_OMIT:
        return []
    fde_count, cursor = read_encoded(table, cursor, fde_count_enc, vaddr, vaddr + cursor)

    starts = []
    for _ in range(fde_count):
        start, cursor = read_encoded(table, cursor, table_enc, vaddr, vaddr + cursor)
        _, cursor = read_encoded(table, cursor, table_enc, vaddr, vaddr + cursor)
        starts.append(start)
    return starts


def module_base():
    """Lowest mapped segment, which the emulator's module-relative offsets count from."""
    lowest = None
    for index in range(ida_segment.get_segm_qty()):
        segment = ida_segment.getnseg(index)
        if segment is None:
            continue
        if lowest is None or segment.start_ea < lowest:
            lowest = segment.start_ea
    return lowest if lowest is not None else 0


def is_indirect_jump(address):
    """True for `jmp reg` and `jmp [mem]`, the forms whose targets are not encoded."""
    insn = ida_ua.insn_t()
    if ida_ua.decode_insn(insn, address) == 0:
        return False
    if idc.print_insn_mnem(address) != "jmp":
        return False
    return insn.ops[0].type not in (ida_ua.o_near, ida_ua.o_far)


def code_xref_targets(address):
    """Addresses this instruction can transfer control to, excluding fall-through."""
    targets = []
    xref = ida_xref.xrefblk_t()
    ok = xref.first_from(address, ida_xref.XREF_ALL)
    while ok:
        if xref.iscode and xref.type != ida_xref.fl_F:
            targets.append(xref.to)
        ok = xref.next_from()
    return targets


def entry_points_in(start, end):
    """Every address in the range that something can branch or call to.

    Nothing is subtracted. An address reached from outside the body is an entry point
    whether it belongs to a called function or a switch case, and a borrowed range must
    not straddle either.
    """
    targets = set()
    for address in idautils.Heads(start, end):
        xref = ida_xref.xrefblk_t()
        ok = xref.first_to(address, ida_xref.XREF_ALL)
        while ok:
            if xref.iscode and xref.type != ida_xref.fl_F:
                targets.add(address)
                break
            ok = xref.next_to()
    return targets


# How far back to look for what a jump register means. An epilogue or a vtable load sits
# within a handful of instructions of the jump; anything further is a different story.
LOOKBACK_INSTRUCTIONS = 12

_TERMINATORS = ("jmp", "ret", "retn", "iret", "ud2")

# Instructions an epilogue is made of. The walk back to a teardown may cross only these:
# a teardown separated from the jump by real computation says nothing about where the jump
# goes, and accepting it would vouch for a body on a coincidence.
_EPILOGUE_MNEMONICS = ("pop", "leave", "nop", "mov", "add", "sub", "lea", "xor")


def _is_branch_target(address):
    xref = ida_xref.xrefblk_t()
    ok = xref.first_to(address, ida_xref.XREF_ALL)
    while ok:
        if xref.iscode and xref.type != ida_xref.fl_F:
            return True
        ok = xref.next_to()
    return False


def _walk_back(address, floor):
    """Instructions immediately before `address`, newest first.

    Stops at a label or a control transfer, so what it yields is the one path that
    actually reaches the jump rather than whatever happens to lie above it.
    """
    cursor = address
    for _ in range(LOOKBACK_INSTRUCTIONS):
        if _is_branch_target(cursor):
            return
        cursor = idc.prev_head(cursor, floor)
        if cursor == idaapi.BADADDR or cursor < floor:
            return
        mnemonic = idc.print_insn_mnem(cursor)
        if mnemonic.startswith("j") or mnemonic in _TERMINATORS:
            return
        yield cursor, mnemonic


def leaves_body(address, floor):
    """Whether an unrecovered indirect jump transfers control out of the body.

    Two signatures say it does, and a jump table can show neither:

    - A stack frame teardown reaching the jump through nothing but epilogue. A switch case
      runs with the frame intact, because it is still inside the function and still uses
      its locals; popping the callee-saved registers and unwinding rsp first only makes
      sense when the jump is a tail call. The run has to be unbroken: a teardown with real
      computation between it and the jump is a coincidence, not a signature.
    - The jump register loaded from a single memory slot, as `mov rax, [rdi]` then
      `mov rax, [rax+28h]` loads a vtable slot. Reading a jump table instead needs an
      index and a scale, which is the shape refused below.
    """
    insn = ida_ua.insn_t()
    if ida_ua.decode_insn(insn, address) == 0:
        return False
    target_register = insn.ops[0].reg if insn.ops[0].type == ida_ua.o_reg else None

    for cursor, mnemonic in _walk_back(address, floor):
        if mnemonic in ("pop", "leave"):
            return True
        if mnemonic in ("add", "sub", "lea") and idc.print_operand(cursor, 0) == "rsp":
            return True

        if target_register is not None:
            previous = ida_ua.insn_t()
            if ida_ua.decode_insn(previous, cursor) == 0:
                return False
            writes_target = (
                previous.ops[0].type == ida_ua.o_reg and previous.ops[0].reg == target_register
            )
            if writes_target:
                # The defining instruction. A single-slot load is a function pointer;
                # anything else - arithmetic, a table base, another register - says nothing.
                return (
                    mnemonic == "mov"
                    and previous.ops[1].type
                    in (ida_ua.o_mem, ida_ua.o_phrase, ida_ua.o_displ)
                    and "*" not in idc.print_operand(cursor, 1)
                )

        if mnemonic not in _EPILOGUE_MNEMONICS:
            return False
    return False


def is_padding(address):
    """Alignment filler between bodies, which control flow never enters."""
    return ida_bytes.get_byte(address) in (0x00, 0x90, 0xCC)


def analyze_range(start, end):
    """Returns (indirect_jump_count, complete).

    `complete` says whether every indirect jump in the range is accounted for: the
    disassembler recovered its switch table, or the jump transfers control out of the body
    entirely, as a dispatch through a vtable slot does.
    """
    indirect_jumps = 0
    address = start
    while address < end:
        flags = ida_bytes.get_full_flags(address)
        if not ida_bytes.is_code(flags):
            # Not code. Identified data - a jump table, a constant pool - sits inside
            # bodies all the time and the pass never decodes it, because it follows flow.
            # Alignment filler is likewise ordinary. Bytes the disassembler left
            # unexplored are the risk: they could be code holding a jump that this export
            # would then claim does not exist.
            if not ida_bytes.is_data(flags) and not is_padding(address):
                return indirect_jumps, False
            address += max(1, ida_bytes.get_item_size(address))
            continue

        size = max(1, ida_bytes.get_item_size(address))
        if not is_indirect_jump(address):
            address += size
            continue

        indirect_jumps += 1
        switch = ida_nalt.get_switch_info(address)
        if switch is not None and switch.jumps != idaapi.BADADDR:
            # Recovered switch: its cases are xrefs, so entry_points_in already has them.
            address += size
            continue

        targets = code_xref_targets(address)
        if any(start <= target < end for target in targets):
            return indirect_jumps, False
        if not targets:
            # No recovered edge, so the judgment has to come from the code around it.
            #
            # An indexed operand is how an absolute jump table is read, and an unrecovered
            # one is exactly the case that must stay forbidden.
            if "*" in idc.print_operand(address, 0):
                return indirect_jumps, False
            # A single slot - `jmp qword ptr [rax+70h]`, a vtable dispatch, or an import
            # thunk through `[rip+disp]` - names one function pointer and cannot land
            # inside this body. A bare register needs the surrounding code to say so.
            insn = ida_ua.insn_t()
            ida_ua.decode_insn(insn, address)
            if insn.ops[0].type == ida_ua.o_reg and not leaves_body(address, start):
                return indirect_jumps, False
        address += size
    return indirect_jumps, True


def define_missing_bodies(starts, base):
    """Turns unwind-table entries the disassembler never reached into functions.

    Auto-analysis finds functions by following flow, so a body only ever entered through
    an unresolved indirect jump stays unexplored. The unwind table says it is a function,
    on the compiler's own record, so defining it is not a guess - and leaving it undefined
    would refuse the whole body for holding bytes nothing explains.
    """
    defined = 0
    for index, start_vaddr in enumerate(starts):
        start = start_vaddr + base
        segment = ida_segment.getseg(start)
        if segment is None or ida_funcs.get_func(start) is not None:
            continue
        end = starts[index + 1] + base if index + 1 < len(starts) else segment.end_ea
        if ida_funcs.add_func(start, min(end, segment.end_ea)):
            defined += 1
    if defined:
        idaapi.auto_wait()
    return defined


def main(output_path):
    idaapi.auto_wait()

    base = module_base()
    starts = sorted(set(eh_frame_function_starts(ida_nalt.get_input_file_path())))
    if not starts:
        raise UnsupportedEncoding("the unwind table lists no bodies")

    defined = define_missing_bodies(starts, base)

    exported = 0
    refused = 0
    total_targets = 0
    lines = [
        "# shadPS4 verified branch targets",
        "# Bodies are partitioned by .eh_frame_hdr, matching the rewrite pass.",
        "# Addresses are module-relative, matching the emulator's own logging.",
        "version %d" % FORMAT_VERSION,
        "base 0x%x" % base,
    ]

    for index, start_vaddr in enumerate(starts):
        start = start_vaddr + base
        segment = ida_segment.getseg(start)
        if segment is None:
            continue
        end = starts[index + 1] + base if index + 1 < len(starts) else segment.end_ea
        end = min(end, segment.end_ea)
        if end <= start:
            continue

        indirect_jumps, complete = analyze_range(start, end)
        if indirect_jumps == 0:
            # The emulator recovers these on its own; exporting them would only add bulk.
            continue
        if not complete:
            # Saying nothing leaves the emulator's own conservative refusal in place,
            # which is the safe answer.
            refused += 1
            continue

        targets = entry_points_in(start, end)
        lines.append("func 0x%x 0x%x" % (start - base, end - base))
        for target in sorted(targets):
            lines.append("target 0x%x" % (target - base))
        exported += 1
        total_targets += len(targets)

    lines.append("")
    with open(output_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))

    print(
        "[branch-targets] %d bodies in the unwind table, %d newly defined, %d exported, "
        "%d refused, %d targets, base 0x%x -> %s"
        % (len(starts), defined, exported, refused, total_targets, base, output_path)
    )


if __name__ == "__main__":
    ARGV = idc.ARGV if hasattr(idc, "ARGV") else []
    if len(ARGV) < 2:
        print('[branch-targets] usage: -S"export_branch_targets.py <output-path>"')
    else:
        main(ARGV[1])
        idc.qexit(0)
