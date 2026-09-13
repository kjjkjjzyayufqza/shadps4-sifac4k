// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <core/libraries/system/userservice.h>
#include "common/types.h"
#include "core/libraries/np/np_types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::SharePlay {

constexpr int ORBIS_SHARE_PLAY_CONNECTION_STATUS_DORMANT = 0x00;
constexpr int ORBIS_SHARE_PLAY_CONNECTION_STATUS_READY = 0x01;
constexpr int ORBIS_SHARE_PLAY_CONNECTION_STATUS_CONNECTED = 0x02;

// Returned by the connection-info queries in libSceSharePlay.sprx. Both queries check the share
// daemon's state word first and fail without touching the caller's buffer when no Share Play
// session is up, which is always the case here.
constexpr int ORBIS_SHARE_PLAY_ERROR_INVALID_ARGUMENT = 0x810E0001;
constexpr int ORBIS_SHARE_PLAY_ERROR_NOT_CONNECTED = 0x810E0004;

struct OrbisSharePlayConnectionInfo {
    int status;
    int mode;
    Libraries::Np::OrbisNpOnlineId hostOnlineId;
    Libraries::Np::OrbisNpOnlineId visitorOnlineId;
    Libraries::UserService::OrbisUserServiceUserId hostUserId;
    Libraries::UserService::OrbisUserServiceUserId visitorUserId;
};
static_assert(sizeof(OrbisSharePlayConnectionInfo) == 0x38);

// The "A" query reports the same session over a wider record: sixteen bytes the shorter record
// drops sit between the visitor online id and the user ids. The module copies the daemon state
// verbatim, so the meaning of those bytes is not recoverable from the copy alone.
struct OrbisSharePlayConnectionInfoA {
    int status;
    int mode;
    Libraries::Np::OrbisNpOnlineId hostOnlineId;
    Libraries::Np::OrbisNpOnlineId visitorOnlineId;
    u8 unresolved[16];
    Libraries::UserService::OrbisUserServiceUserId hostUserId;
    Libraries::UserService::OrbisUserServiceUserId visitorUserId;
};
static_assert(sizeof(OrbisSharePlayConnectionInfoA) == 0x48);

int PS4_SYSV_ABI sceSharePlayCrashDaemon();
int PS4_SYSV_ABI sceSharePlayGetCurrentConnectionInfo(OrbisSharePlayConnectionInfo* pInfo);
int PS4_SYSV_ABI sceSharePlayGetCurrentConnectionInfoA(OrbisSharePlayConnectionInfoA* pInfo);
int PS4_SYSV_ABI sceSharePlayGetCurrentInfo();
int PS4_SYSV_ABI sceSharePlayGetEvent();
int PS4_SYSV_ABI sceSharePlayInitialize();
int PS4_SYSV_ABI sceSharePlayNotifyDialogOpen();
int PS4_SYSV_ABI sceSharePlayNotifyForceCloseForCdlg();
int PS4_SYSV_ABI sceSharePlayNotifyOpenQuickMenu();
int PS4_SYSV_ABI sceSharePlayResumeScreenForCdlg();
int PS4_SYSV_ABI sceSharePlayServerLock();
int PS4_SYSV_ABI sceSharePlayServerUnLock();
int PS4_SYSV_ABI sceSharePlaySetMode();
int PS4_SYSV_ABI sceSharePlaySetProhibition();
int PS4_SYSV_ABI sceSharePlaySetProhibitionModeWithAppId();
int PS4_SYSV_ABI sceSharePlayStartStandby();
int PS4_SYSV_ABI sceSharePlayStartStreaming();
int PS4_SYSV_ABI sceSharePlayStopStandby();
int PS4_SYSV_ABI sceSharePlayStopStreaming();
int PS4_SYSV_ABI sceSharePlayTerminate();
int PS4_SYSV_ABI Func_2E93C0EA6A6B67C4();
int PS4_SYSV_ABI Func_C1C236728D88E177();
int PS4_SYSV_ABI Func_E9E80C474781F115();
int PS4_SYSV_ABI Func_F3DD6199DA15ED44();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::SharePlay