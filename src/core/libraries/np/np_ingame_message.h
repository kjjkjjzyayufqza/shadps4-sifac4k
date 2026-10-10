// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "core/libraries/np/np_types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Np::NpInGameMessage {

// Payload libSceNpManager accepts. Its send path bounds `dataSize` to 1..512 before anything is
// queued (module libSceNpManager.sprx offset 0x2690 checks `size - 1 <= 0x1FF`), which also fixes
// the size of the buffer that precedes it.
constexpr u64 ORBIS_NP_IN_GAME_MESSAGE_DATA_SIZE_MAX = 512;

struct OrbisNpInGameMessageData {
    u8 data[ORBIS_NP_IN_GAME_MESSAGE_DATA_SIZE_MAX];
    u64 dataSize;
};
static_assert(sizeof(OrbisNpInGameMessageData) == 0x208);

s32 PS4_SYSV_ABI sceNpInGameMessageInitialize(const void* param);
s32 PS4_SYSV_ABI sceNpInGameMessageTerminate(s32 libCtxId);
s32 PS4_SYSV_ABI sceNpInGameMessageCreateHandle();
s32 PS4_SYSV_ABI sceNpInGameMessageDeleteHandle(s32 handleId);
s32 PS4_SYSV_ABI sceNpInGameMessagePrepareA(s32 libCtxId, s32 handleId, u64 reserved,
                                            const OrbisNpAccountId* to, u64 toNum);
s32 PS4_SYSV_ABI sceNpInGameMessageSendDataA(s32 libCtxId, const void* to, const void* from,
                                             const OrbisNpInGameMessageData* msg);
s32 PS4_SYSV_ABI sceNpInGameMessageGetMemoryPoolStatistics(s32 libCtxId, void* stats);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Np::NpInGameMessage
