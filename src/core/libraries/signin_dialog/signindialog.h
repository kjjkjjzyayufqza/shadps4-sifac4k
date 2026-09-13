// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include "common/types.h"
#include "core/libraries/system/commondialog.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::SigninDialog {

// Layout of the parameter block games pass to sceSigninDialogOpen (size field is 16).
struct OrbisSigninDialogParam {
    u32 size;
    s32 user_id;
    std::array<u8, 8> reserved;
};
static_assert(sizeof(OrbisSigninDialogParam) == 16);

// Only the leading result code is defined; the remaining bytes are reserved and caller-owned.
struct OrbisSigninDialogResult {
    CommonDialog::Result result;
};

CommonDialog::Error PS4_SYSV_ABI sceSigninDialogInitialize();
CommonDialog::Error PS4_SYSV_ABI sceSigninDialogOpen(const OrbisSigninDialogParam* param);
CommonDialog::Status PS4_SYSV_ABI sceSigninDialogGetStatus();
CommonDialog::Status PS4_SYSV_ABI sceSigninDialogUpdateStatus();
CommonDialog::Error PS4_SYSV_ABI sceSigninDialogGetResult(OrbisSigninDialogResult* result);
CommonDialog::Error PS4_SYSV_ABI sceSigninDialogClose();
CommonDialog::Error PS4_SYSV_ABI sceSigninDialogTerminate();

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::SigninDialog
