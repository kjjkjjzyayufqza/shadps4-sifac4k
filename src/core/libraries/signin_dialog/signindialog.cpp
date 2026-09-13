// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/signin_dialog/signindialog.h"

namespace Libraries::SigninDialog {

using CommonDialog::Error;
using CommonDialog::Result;
using CommonDialog::Status;

static Status g_status = Status::NONE;
static Result g_result = Result::OK;

Error PS4_SYSV_ABI sceSigninDialogInitialize() {
    if (!CommonDialog::g_isInitialized) {
        LOG_ERROR(Lib_SigninDialog, "common dialog system is not initialized");
        return Error::NOT_SYSTEM_INITIALIZED;
    }
    if (g_status != Status::NONE) {
        LOG_ERROR(Lib_SigninDialog, "already initialized");
        return Error::ALREADY_INITIALIZED;
    }
    if (CommonDialog::g_isUsed) {
        LOG_ERROR(Lib_SigninDialog, "another common dialog is in use");
        return Error::BUSY;
    }
    g_status = Status::INITIALIZED;
    CommonDialog::g_isUsed = true;
    LOG_INFO(Lib_SigninDialog, "initialized");
    return Error::OK;
}

Error PS4_SYSV_ABI sceSigninDialogOpen(const OrbisSigninDialogParam* param) {
    if (g_status != Status::INITIALIZED && g_status != Status::FINISHED) {
        LOG_ERROR(Lib_SigninDialog, "called in invalid state {}", static_cast<u32>(g_status));
        return Error::INVALID_STATE;
    }
    if (param == nullptr) {
        LOG_ERROR(Lib_SigninDialog, "param is null");
        return Error::ARG_NULL;
    }
    if (param->size != sizeof(OrbisSigninDialogParam)) {
        LOG_ERROR(Lib_SigninDialog, "unsupported param size {}", param->size);
        return Error::PARAM_INVALID;
    }

    // shadNet credentials are configured per user outside the guest, so there is no interactive
    // sign-in step: the dialog completes immediately and reports the user's current state. A
    // user that is not signed in is reported as a cancelled sign-in, as on hardware.
    const bool signed_in = Np::NpHandler::GetInstance().IsPsnSignedIn(param->user_id);
    g_result = signed_in ? Result::OK : Result::USER_CANCELED;
    g_status = Status::FINISHED;
    LOG_INFO(Lib_SigninDialog, "user_id={} signed_in={}", param->user_id, signed_in);
    return Error::OK;
}

Status PS4_SYSV_ABI sceSigninDialogGetStatus() {
    LOG_TRACE(Lib_SigninDialog, "status={}", static_cast<u32>(g_status));
    return g_status;
}

Status PS4_SYSV_ABI sceSigninDialogUpdateStatus() {
    LOG_TRACE(Lib_SigninDialog, "status={}", static_cast<u32>(g_status));
    return g_status;
}

Error PS4_SYSV_ABI sceSigninDialogGetResult(OrbisSigninDialogResult* result) {
    if (g_status == Status::NONE) {
        LOG_ERROR(Lib_SigninDialog, "not initialized");
        return Error::NOT_INITIALIZED;
    }
    if (result == nullptr) {
        LOG_ERROR(Lib_SigninDialog, "result is null");
        return Error::ARG_NULL;
    }
    if (g_status != Status::FINISHED) {
        LOG_ERROR(Lib_SigninDialog, "dialog has not finished");
        return Error::NOT_FINISHED;
    }
    result->result = g_result;
    LOG_INFO(Lib_SigninDialog, "result={}", static_cast<u32>(g_result));
    return Error::OK;
}

Error PS4_SYSV_ABI sceSigninDialogClose() {
    if (g_status == Status::NONE) {
        LOG_ERROR(Lib_SigninDialog, "not initialized");
        return Error::NOT_INITIALIZED;
    }
    // The dialog finishes during Open, so it is never running when a close is requested.
    if (g_status != Status::RUNNING) {
        return Error::NOT_RUNNING;
    }
    return Error::OK;
}

Error PS4_SYSV_ABI sceSigninDialogTerminate() {
    if (g_status == Status::NONE) {
        LOG_ERROR(Lib_SigninDialog, "not initialized");
        return Error::NOT_INITIALIZED;
    }
    g_status = Status::NONE;
    CommonDialog::g_isUsed = false;
    LOG_INFO(Lib_SigninDialog, "terminated");
    return Error::OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("mlYGfmqE3fQ", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogInitialize);
    LIB_FUNCTION("JlpJVoRWv7U", "libSceSigninDialog", 1, "libSceSigninDialog", sceSigninDialogOpen);
    LIB_FUNCTION("2m077aeC+PA", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogGetStatus);
    LIB_FUNCTION("Bw31liTFT3A", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogUpdateStatus);
    LIB_FUNCTION("nqG7rqnYw1U", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogGetResult);
    LIB_FUNCTION("M3OkENHcyiU", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogClose);
    LIB_FUNCTION("LXlmS6PvJdU", "libSceSigninDialog", 1, "libSceSigninDialog",
                 sceSigninDialogTerminate);
};

} // namespace Libraries::SigninDialog
