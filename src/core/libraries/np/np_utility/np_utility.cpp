// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <httplib.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/http.h"
#include "core/libraries/network/http_error.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_utility/np_utility.h"
#include "core/libraries/np/np_utility/np_utility_ctx.h"

namespace Libraries::Np::NpUtility {

struct LookupTitleCtx {
    s32 userId = -1;
    OrbisNpId selfNpId{};
    std::optional<LookupTimeouts> timeouts;
};

std::mutex g_lookup_mutex;
std::map<OrbisNpLookupTitleCtxId, LookupTitleCtx> g_lookup_title_ctxs;
std::map<OrbisNpLookupRequestId, std::shared_ptr<LookupRequestCtx>> g_lookup_requests;
OrbisNpLookupTitleCtxId g_lookup_next_ctx_id = 1;
OrbisNpLookupRequestId g_lookup_next_req_id = 1;

std::string_view OnlineIdView(const OrbisNpOnlineId& id) {
    return std::string_view(id.data, strnlen(id.data, ORBIS_NP_ONLINEID_MAX_LENGTH));
}

//***********************************
// NpLookup: title context management
//***********************************
s32 PS4_SYSV_ABI sceNpLookupCreateTitleCtx(const OrbisNpId* selfNpId) {
    if (selfNpId == nullptr) {
        LOG_ERROR(Lib_NpUtility, "selfNpId is null");
        return ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    }
    std::lock_guard lock(g_lookup_mutex);
    if (static_cast<s32>(g_lookup_title_ctxs.size()) >= ORBIS_NP_LOOKUP_MAX_CTX_NUM) {
        LOG_ERROR(Lib_NpUtility, "too many title contexts ({})", g_lookup_title_ctxs.size());
        return ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
    }
    const s32 userId =
        Libraries::Np::NpHandler::GetInstance().GetUserIdByOnlineId(selfNpId->handle);
    const OrbisNpLookupTitleCtxId id = g_lookup_next_ctx_id++;
    g_lookup_title_ctxs[id] = LookupTitleCtx{.userId = userId, .selfNpId = *selfNpId};
    LOG_INFO(Lib_NpUtility, "id={} userId={} onlineId='{}'", id, userId,
             OnlineIdView(selfNpId->handle));
    return id;
}

s32 PS4_SYSV_ABI sceNpLookupCreateTitleCtxA(UserService::OrbisUserServiceUserId userId) {
    std::lock_guard lock(g_lookup_mutex);
    if (static_cast<s32>(g_lookup_title_ctxs.size()) >= ORBIS_NP_LOOKUP_MAX_CTX_NUM) {
        LOG_ERROR(Lib_NpUtility, "too many title contexts ({})", g_lookup_title_ctxs.size());
        return ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
    }
    const OrbisNpId selfNpId = Libraries::Np::NpHandler::GetInstance().GetNpId(userId);
    const OrbisNpLookupTitleCtxId id = g_lookup_next_ctx_id++;
    g_lookup_title_ctxs[id] = LookupTitleCtx{.userId = userId, .selfNpId = selfNpId};
    LOG_INFO(Lib_NpUtility, "id={} userId={}", id, userId);
    return id;
}

s32 PS4_SYSV_ABI sceNpLookupDeleteTitleCtx(OrbisNpLookupTitleCtxId titleCtxId) {
    std::lock_guard lock(g_lookup_mutex);
    if (!g_lookup_title_ctxs.contains(titleCtxId)) {
        LOG_ERROR(Lib_NpUtility, "Invalid titleCtxId {}", titleCtxId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    for (auto it = g_lookup_requests.begin(); it != g_lookup_requests.end();) {
        if (it->second->titleCtxId == titleCtxId) {
            it->second->SetResult(ORBIS_NP_COMMUNITY_ERROR_ABORTED);
            it = g_lookup_requests.erase(it);
        } else {
            ++it;
        }
    }
    g_lookup_title_ctxs.erase(titleCtxId);
    LOG_INFO(Lib_NpUtility, "id={}", titleCtxId);
    return ORBIS_OK;
}

//***********************************
// NpLookup: request management
//***********************************
static s32 CreateLookupRequest(OrbisNpLookupTitleCtxId titleCtxId, bool isAsync) {
    std::lock_guard lock(g_lookup_mutex);
    auto ctx_it = g_lookup_title_ctxs.find(titleCtxId);
    if (ctx_it == g_lookup_title_ctxs.end()) {
        LOG_ERROR(Lib_NpUtility, "Invalid titleCtxId {}", titleCtxId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    if (static_cast<s32>(g_lookup_requests.size()) >= ORBIS_NP_LOOKUP_MAX_REQUEST_NUM) {
        LOG_ERROR(Lib_NpUtility, "too many requests ({})", g_lookup_requests.size());
        return ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
    }
    const OrbisNpLookupRequestId id = g_lookup_next_req_id++;
    auto req = std::make_shared<LookupRequestCtx>();
    req->titleCtxId = titleCtxId;
    req->isAsync = isAsync;
    req->timeouts = ctx_it->second.timeouts;
    g_lookup_requests[id] = std::move(req);
    LOG_INFO(Lib_NpUtility, "id={} titleCtxId={} async={}", id, titleCtxId, isAsync);
    return id;
}

s32 PS4_SYSV_ABI sceNpLookupCreateRequest(OrbisNpLookupTitleCtxId titleCtxId) {
    return CreateLookupRequest(titleCtxId, /*isAsync=*/false);
}

s32 PS4_SYSV_ABI sceNpLookupCreateAsyncRequest(
    OrbisNpLookupTitleCtxId titleCtxId, const OrbisNpLookupCreateAsyncRequestParameter* param) {
    return CreateLookupRequest(titleCtxId, /*isAsync=*/true);
}

s32 PS4_SYSV_ABI sceNpLookupDeleteRequest(OrbisNpLookupRequestId reqId) {
    std::lock_guard lock(g_lookup_mutex);
    auto it = g_lookup_requests.find(reqId);
    if (it == g_lookup_requests.end()) {
        LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    {
        std::lock_guard rlock(it->second->mutex);
        it->second->aborted = true;
    }
    it->second->SetResult(ORBIS_NP_COMMUNITY_ERROR_ABORTED);
    g_lookup_requests.erase(it);
    LOG_INFO(Lib_NpUtility, "reqId={}", reqId);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupAbortRequest(OrbisNpLookupRequestId reqId) {
    std::shared_ptr<LookupRequestCtx> req;
    {
        std::lock_guard lock(g_lookup_mutex);
        auto it = g_lookup_requests.find(reqId);
        if (it == g_lookup_requests.end()) {
            LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
            return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        req = it->second;
    }
    {
        std::lock_guard rlock(req->mutex);
        req->aborted = true;
    }
    req->SetResult(ORBIS_NP_COMMUNITY_ERROR_ABORTED);
    LOG_INFO(Lib_NpUtility, "reqId={}", reqId);
    return ORBIS_OK;
}

//***********************************
// NpLookup: search + async completion
//***********************************
s32 PS4_SYSV_ABI sceNpLookupNpId(OrbisNpLookupRequestId reqId, const OrbisNpOnlineId* onlineId,
                                 OrbisNpId* npId, void* option) {
    LOG_INFO(Lib_NpUtility, "reqId={} onlineId='{}' npId={} option={}", reqId,
             onlineId != nullptr ? OnlineIdView(*onlineId) : std::string_view("(null)"),
             static_cast<const void*>(npId), static_cast<const void*>(option));
    if (option != nullptr) {
        LOG_ERROR(Lib_NpUtility, "option is not supported (option={})", option);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    if (onlineId == nullptr || npId == nullptr) {
        LOG_ERROR(Lib_NpUtility, "onlineId or npId is null");
        return ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    }

    std::shared_ptr<LookupRequestCtx> req;
    s32 searching_user_id = -1;
    {
        std::lock_guard lock(g_lookup_mutex);
        auto it = g_lookup_requests.find(reqId);
        if (it == g_lookup_requests.end()) {
            LOG_ERROR(Lib_NpUtility, "invalid reqId {}", reqId);
            return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        req = it->second;
        if (auto ctx_it = g_lookup_title_ctxs.find(req->titleCtxId);
            ctx_it != g_lookup_title_ctxs.end()) {
            searching_user_id = ctx_it->second.userId;
        }
    }
    {
        std::lock_guard rlock(req->mutex);
        if (req->aborted) {
            LOG_ERROR(Lib_NpUtility, "reqId {} was aborted", reqId);
            return ORBIS_NP_COMMUNITY_ERROR_ABORTED;
        }
        if (req->used) {
            LOG_ERROR(Lib_NpUtility, "reqId {} was already used", reqId);
            return ORBIS_NP_COMMUNITY_ERROR_INVALID_TYPE;
        }
        req->used = true;
    }

    // Global online ID to account ID resolution
    const std::string targetOnlineId(OnlineIdView(*onlineId));
    Libraries::Np::NpHandler::GetInstance().ResolveOnlineId(
        searching_user_id, targetOnlineId,
        [req, npId](s32 result, u64 /*accountId*/, const std::string& canonicalOnlineId) {
            if (result == ORBIS_OK && npId != nullptr) {
                SetNpId(*npId, canonicalOnlineId);
            }
            req->SetResult(result);
        });

    if (!req->isAsync) {
        return req->Wait();
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupPollAsync(OrbisNpLookupRequestId reqId, s32* result) {
    std::shared_ptr<LookupRequestCtx> req;
    {
        std::lock_guard lock(g_lookup_mutex);
        auto it = g_lookup_requests.find(reqId);
        if (it == g_lookup_requests.end()) {
            LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
            return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        req = it->second;
    }
    std::lock_guard rlock(req->mutex);
    if (!req->result.has_value()) {
        return ORBIS_NP_LOOKUP_POLL_ASYNC_RET_RUNNING;
    }
    if (result != nullptr) {
        *result = *req->result;
    }
    LOG_INFO(Lib_NpUtility, "reqId={} completed (result={:#x})", reqId,
             static_cast<u32>(*req->result));
    return ORBIS_OK;
}

// Timeout rules shared by sceNpLookupSetTimeout() and sceNpWordFilterSetTimeout().
static s32 ValidateCommunityTimeouts(const LookupTimeouts& t) {
    if (t.resolveRetry < 0) {
        LOG_ERROR(Lib_NpUtility, "resolveRetry < 0");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    if (t.resolveTimeout != ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT && t.resolveTimeout < 1'000'000) {
        LOG_ERROR(Lib_NpUtility, "resolveTimeout < 1'000'000");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    if (t.connTimeout != ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT && t.connTimeout < 10'000'000) {
        LOG_ERROR(Lib_NpUtility, "connTimeout < 10'000'000");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    if (t.sendTimeout != ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT && t.sendTimeout < 10'000'000) {
        LOG_ERROR(Lib_NpUtility, "sendTimeout < 10'000'000");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    if (t.recvTimeout != ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT && t.recvTimeout < 10'000'000) {
        LOG_ERROR(Lib_NpUtility, "recvTimeout < 10'000'000");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    if (t.resolveRetry == 0 && t.resolveTimeout == ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT &&
        t.connTimeout == ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT &&
        t.sendTimeout == ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT &&
        t.recvTimeout == ORBIS_NP_LOOKUP_TIMEOUT_NO_EFFECT) {
        LOG_ERROR(Lib_NpUtility, "all timeouts are no effect");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupSetTimeout(s32 id, s32 resolveRetry, u32 resolveTimeout,
                                       u32 connTimeout, u32 sendTimeout, u32 recvTimeout) {
    LOG_INFO(Lib_NpUtility,
             "id={} resolveRetry={} resolveTimeout={} connTimeout={} "
             "sendTimeout={} recvTimeout={}",
             id, resolveRetry, resolveTimeout, connTimeout, sendTimeout, recvTimeout);

    const LookupTimeouts timeouts{
        .resolveRetry = resolveRetry,
        .resolveTimeout = resolveTimeout,
        .connTimeout = connTimeout,
        .sendTimeout = sendTimeout,
        .recvTimeout = recvTimeout,
    };
    if (const s32 ret = ValidateCommunityTimeouts(timeouts); ret != ORBIS_OK) {
        return ret;
    }

    std::lock_guard lock(g_lookup_mutex);
    if (auto ctx_it = g_lookup_title_ctxs.find(id); ctx_it != g_lookup_title_ctxs.end()) {
        ctx_it->second.timeouts = timeouts;
        return ORBIS_OK;
    }
    if (auto req_it = g_lookup_requests.find(id); req_it != g_lookup_requests.end()) {
        std::lock_guard rlock(req_it->second->mutex);
        req_it->second->timeouts = timeouts;
        return ORBIS_OK;
    }
    LOG_ERROR(Lib_NpUtility, "invalid id {}", id);
    return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
}

s32 PS4_SYSV_ABI sceNpLookupWaitAsync(OrbisNpLookupRequestId reqId, s32* result) {
    std::shared_ptr<LookupRequestCtx> req;
    {
        std::lock_guard lock(g_lookup_mutex);
        auto it = g_lookup_requests.find(reqId);
        if (it == g_lookup_requests.end()) {
            LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
            return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
        }
        req = it->second;
    }
    const s32 r = req->Wait();
    if (result != nullptr) {
        *result = r;
    }
    LOG_INFO(Lib_NpUtility, "reqId={} completed (result={:#x})", reqId, static_cast<u32>(r));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntAbortRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckAvailability() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckAvailabilityA() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckAvailabilityAll() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckAvailabilityAllA() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckServiceAvailability() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckServiceAvailabilityA() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckServiceAvailabilityAll() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCheckServiceAvailabilityAllA() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntCreateRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntDestroyRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntFinalize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppInfoIntInitialize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLink2IntAbortRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLink2IntCreateRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLink2IntDestroyRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLink2IntFinalize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLink2IntGetCompatibleTitleIdList() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLink2IntGetCompatibleTitleIdNum() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLink2IntInitialize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLinkIntAbortRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLinkIntCreateRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLinkIntDestroyRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLinkIntFinalize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLinkIntGetCompatibleTitleIdList() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLinkIntGetCompatibleTitleIdNum() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpAppLaunchLinkIntInitialize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

//***********************************
// NpBandwidthTest
//***********************************
// libSceNpUtility measures throughput with plain HTTP against PSN test hosts: a 128 KiB upload,
// repeated with 1 MiB when the first one finishes within 100 ms, then a 2 MiB download. The status
// stays RUNNING until that worker finishes, and Shutdown joins it before reporting bits per
// second. shadNet serves the same test from its WebAPI server (/networktest/post and
// /networktest/get_2m), so the result reflects the real path to the configured server. As in the
// library, a failed step ends the test with its error as the result, and an upload is not judged
// by the response status.
constexpr u64 kBandwidthUploadProbeBytes = 128 * 1024;
constexpr u64 kBandwidthUploadBytes = 1024 * 1024;
constexpr size_t kBandwidthChunkBytes = 1024;
constexpr std::chrono::microseconds kBandwidthProbeThreshold{100'000};
// Same bounds NpHandler uses for its shadNet WebAPI requests.
constexpr time_t kBandwidthConnectTimeoutSec = 5;
constexpr time_t kBandwidthTransferTimeoutSec = 10;

struct BandwidthTestCtx {
    std::jthread worker;
    std::atomic<bool> finished = false;
    std::atomic<bool> aborted = false;
    // Written only by the worker and read after Shutdown has joined it.
    s32 result = ORBIS_OK;
    u64 uploadBytes = 0;
    std::chrono::microseconds uploadTime{};
    u64 downloadBytes = 0;
    std::chrono::microseconds downloadTime{};
};

std::mutex g_bandwidth_mutex;
std::map<s32, std::shared_ptr<BandwidthTestCtx>> g_bandwidth_tests;
s32 g_bandwidth_next_ctx_id = 1;

static s32 BandwidthTransferError(const BandwidthTestCtx& test, httplib::Error error) {
    // Aborting cancels the transfer from the content callbacks; the library reports that as an
    // aborted HTTP request.
    if (test.aborted) {
        return ORBIS_HTTP_ERROR_ABORTED;
    }
    return Libraries::Http::TranslateHttpClientError(static_cast<int>(error));
}

static s32 RunBandwidthUpload(httplib::Client& client, BandwidthTestCtx& test, u64 bytes) {
    static const std::array<char, kBandwidthChunkBytes> zeros{};
    const auto start = std::chrono::steady_clock::now();
    const auto res = client.Post(
        "/networktest/post", static_cast<size_t>(bytes),
        [&test](size_t, size_t length, httplib::DataSink& sink) {
            if (test.aborted) {
                return false;
            }
            return sink.write(zeros.data(), std::min(length, zeros.size()));
        },
        "application/octet-stream");
    if (!res) {
        return BandwidthTransferError(test, res.error());
    }
    test.uploadBytes = bytes;
    test.uploadTime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return ORBIS_OK;
}

static s32 RunBandwidthDownload(httplib::Client& client, BandwidthTestCtx& test) {
    int status = 0;
    u64 received = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto res = client.Get(
        "/networktest/get_2m",
        [&status](const httplib::Response& response) {
            status = response.status;
            return response.status == 200;
        },
        [&test, &received](const char*, size_t length) {
            received += length;
            return !test.aborted;
        });
    if (status != 0 && status != 200) {
        LOG_ERROR(Lib_NpUtility, "bandwidth download returned HTTP {}", status);
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_BAD_RESPONSE;
    }
    if (!res) {
        return BandwidthTransferError(test, res.error());
    }
    test.downloadBytes = received;
    test.downloadTime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    return ORBIS_OK;
}

static s32 MeasureBandwidth(BandwidthTestCtx& test) {
    if (!EmulatorSettings.IsConnectedToNetwork() || !EmulatorSettings.IsShadNetEnabled()) {
        // Without shadNet there is no test server to reach.
        return ORBIS_HTTP_ERROR_NETWORK;
    }
    httplib::Client client(EmulatorSettings.GetShadNetWebApiServer());
    client.set_connection_timeout(kBandwidthConnectTimeoutSec);
    client.set_read_timeout(kBandwidthTransferTimeoutSec);
    client.set_write_timeout(kBandwidthTransferTimeoutSec);

    if (const s32 ret = RunBandwidthUpload(client, test, kBandwidthUploadProbeBytes);
        ret != ORBIS_OK) {
        return ret;
    }
    if (test.uploadTime <= kBandwidthProbeThreshold) {
        if (const s32 ret = RunBandwidthUpload(client, test, kBandwidthUploadBytes);
            ret != ORBIS_OK) {
            return ret;
        }
    }
    return RunBandwidthDownload(client, test);
}

// Published to the Matching2 signaling layer; peers advertise it to each other.
static std::atomic<u32> g_last_upload_bps{0};

static double BitsPerSecond(u64 bytes, std::chrono::microseconds time) {
    if (time.count() <= 0) {
        return 0.0;
    }
    return static_cast<double>(bytes) * 8.0 * 1'000'000.0 / static_cast<double>(time.count());
}

s32 PS4_SYSV_ABI sceNpBandwidthTestInitStart(const OrbisNpBandwidthTestInitParam* param) {
    if (param == nullptr) {
        LOG_ERROR(Lib_NpUtility, "param is null");
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_ARGUMENT;
    }
    // Titles built with SDK 2.50 or later must leave the reserved area zeroed.
    s32 sdk_version = 0;
    if (Libraries::Kernel::sceKernelGetCompiledSdkVersion(&sdk_version) >= 0 &&
        static_cast<u32>(sdk_version) >= 0x2500000 &&
        std::ranges::any_of(param->reserved, [](u8 b) { return b != 0; })) {
        LOG_ERROR(Lib_NpUtility, "reserved field is not zero");
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_ARGUMENT;
    }
    if (param->size != sizeof(OrbisNpBandwidthTestInitParam)) {
        LOG_ERROR(Lib_NpUtility, "invalid size {}", param->size);
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_SIZE;
    }

    auto test = std::make_shared<BandwidthTestCtx>();
    s32 id = 0;
    {
        std::lock_guard lock(g_bandwidth_mutex);
        id = g_bandwidth_next_ctx_id++;
        g_bandwidth_tests[id] = test;
    }
    // The library runs the test on a thread created with the requested priority and affinity; a
    // host thread performs the same transfers here. The context outlives the worker because
    // Shutdown joins it before releasing the context.
    BandwidthTestCtx* ctx = test.get();
    ctx->worker = std::jthread([ctx] {
        Common::SetCurrentThreadName("NpBandwidthTest");
        ctx->result = MeasureBandwidth(*ctx);
        ctx->finished = true;
    });
    LOG_INFO(Lib_NpUtility, "contextId={}", id);
    return id;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestGetStatus(s32 contextId, s32* status) {
    if (status == nullptr) {
        LOG_ERROR(Lib_NpUtility, "status is null");
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_GET_STATUS_INVALID_ARGUMENT;
    }
    std::lock_guard lock(g_bandwidth_mutex);
    auto it = g_bandwidth_tests.find(contextId);
    if (it == g_bandwidth_tests.end()) {
        LOG_ERROR(Lib_NpUtility, "invalid contextId {}", contextId);
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_CONTEXT_NOT_FOUND;
    }
    *status = it->second->finished ? ORBIS_NP_BANDWIDTH_TEST_STATUS_FINISHED
                                   : ORBIS_NP_BANDWIDTH_TEST_STATUS_RUNNING;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestAbort(s32 contextId) {
    std::lock_guard lock(g_bandwidth_mutex);
    auto it = g_bandwidth_tests.find(contextId);
    if (it == g_bandwidth_tests.end()) {
        LOG_ERROR(Lib_NpUtility, "invalid contextId {}", contextId);
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_CONTEXT_NOT_FOUND;
    }
    it->second->aborted = true;
    LOG_INFO(Lib_NpUtility, "contextId={}", contextId);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestShutdown(s32 contextId, OrbisNpBandwidthTestResult* result) {
    if (result == nullptr) {
        LOG_ERROR(Lib_NpUtility, "result is null");
        return ORBIS_NP_BANDWIDTH_TEST_ERROR_INVALID_ARGUMENT;
    }
    std::shared_ptr<BandwidthTestCtx> test;
    {
        std::lock_guard lock(g_bandwidth_mutex);
        auto it = g_bandwidth_tests.find(contextId);
        if (it == g_bandwidth_tests.end()) {
            LOG_ERROR(Lib_NpUtility, "invalid contextId {}", contextId);
            return ORBIS_NP_BANDWIDTH_TEST_ERROR_CONTEXT_NOT_FOUND;
        }
        test = std::move(it->second);
        g_bandwidth_tests.erase(it);
    }
    // Like the library, shutting down waits for a test that is still running.
    test->worker.join();
    result->uploadBps = BitsPerSecond(test->uploadBytes, test->uploadTime);
    result->downloadBps = BitsPerSecond(test->downloadBytes, test->downloadTime);
    result->result = test->result;
    g_last_upload_bps.store(
        static_cast<u32>(
            std::min(result->uploadBps, static_cast<double>(std::numeric_limits<u32>::max()))),
        std::memory_order_relaxed);
    LOG_INFO(Lib_NpUtility, "contextId={} upload={:.0f} bps download={:.0f} bps result={:#x}",
             contextId, result->uploadBps, result->downloadBps, static_cast<u32>(result->result));
    return ORBIS_OK;
}

u32 GetLastMeasuredUploadBps() {
    return g_last_upload_bps.load(std::memory_order_relaxed);
}

s32 PS4_SYSV_ABI sceNpBandwidthTestDownloadOnlyInitStart() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestInitStartDownload() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestInitStartUpload() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestShutdownWithDetailedInfo() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpBandwidthTestUploadOnlyInitStart() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetAbortRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetCensorComment() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetConvertJidToNpId() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetConvertNpIdToJid() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetCreateRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetCreateTitleCtx() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetDeleteRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetDeleteTitleCtx() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetInit() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetInitWithFunctionPointer() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetInitWithMemoryPool() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetIsInit() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetNpId() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetSanitizeComment() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetSetTimeout() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpLookupNetTerm() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntAbortRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntCreateRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntDestroyRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntFinalize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntGetServiceAvailability() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntGetServiceAvailabilityA() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntInitialize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceChecker2IntIsSetServiceType() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntAbortRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntCreateRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntDestroyRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntFinalize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntGetAvailability() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntGetAvailabilityList() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntInitialize() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpServiceCheckerIntIsCached() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTitleMetadataIntAbortRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTitleMetadataIntCreateRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTitleMetadataIntDeleteRequest() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTitleMetadataIntGetInfo() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpTitleMetadataIntGetNpTitleId() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpUtilityInit() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpUtilityTerm() {
    LOG_ERROR(Lib_NpUtility, "(STUBBED) called");
    return ORBIS_OK;
}

//***********************************
// NpWordFilter
//***********************************
// Word filter title contexts and requests follow the NP community object model used by NpLookup
// (ids start at 1, async completion through Poll/Wait, deleting a context aborts its requests).
// Argument checks follow libSceNpUtility. shadNet has no word list, so every comment is accepted
// unchanged: censoring and sanitizing complete with ORBIS_OK, which is what PSN reports for an
// acceptable comment.
struct WordFilterTitleCtx {
    s32 userId = -1;
    OrbisNpId selfNpId{};
    std::optional<LookupTimeouts> timeouts;
};

std::mutex g_word_filter_mutex;
std::map<OrbisNpWordFilterTitleCtxId, WordFilterTitleCtx> g_word_filter_title_ctxs;
std::map<OrbisNpWordFilterRequestId, std::shared_ptr<LookupRequestCtx>> g_word_filter_requests;
OrbisNpWordFilterTitleCtxId g_word_filter_next_ctx_id = 1;
OrbisNpWordFilterRequestId g_word_filter_next_req_id = 1;

static s32 CreateWordFilterTitleCtx(s32 userId, const OrbisNpId& selfNpId) {
    std::lock_guard lock(g_word_filter_mutex);
    if (static_cast<s32>(g_word_filter_title_ctxs.size()) >= ORBIS_NP_WORD_FILTER_MAX_CTX_NUM) {
        LOG_ERROR(Lib_NpUtility, "too many title contexts ({})", g_word_filter_title_ctxs.size());
        return ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
    }
    const OrbisNpWordFilterTitleCtxId id = g_word_filter_next_ctx_id++;
    g_word_filter_title_ctxs[id] = WordFilterTitleCtx{.userId = userId, .selfNpId = selfNpId};
    LOG_INFO(Lib_NpUtility, "id={} userId={} onlineId='{}'", id, userId,
             OnlineIdView(selfNpId.handle));
    return id;
}

s32 PS4_SYSV_ABI sceNpWordFilterCreateTitleCtx(const OrbisNpId* selfNpId) {
    if (selfNpId == nullptr) {
        LOG_ERROR(Lib_NpUtility, "selfNpId is null");
        return ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    }
    const s32 userId =
        Libraries::Np::NpHandler::GetInstance().GetUserIdByOnlineId(selfNpId->handle);
    return CreateWordFilterTitleCtx(userId, *selfNpId);
}

s32 PS4_SYSV_ABI sceNpWordFilterCreateTitleCtxA(UserService::OrbisUserServiceUserId userId) {
    // The library resolves the caller's NP ID first, so a signed-out user fails here.
    auto& handler = Libraries::Np::NpHandler::GetInstance();
    if (!handler.IsPsnSignedIn(userId)) {
        LOG_ERROR(Lib_NpUtility, "userId={} is not signed in", userId);
        return ORBIS_NP_ERROR_SIGNED_OUT;
    }
    return CreateWordFilterTitleCtx(userId, handler.GetNpId(userId));
}

s32 PS4_SYSV_ABI sceNpWordFilterDeleteTitleCtx(OrbisNpWordFilterTitleCtxId titleCtxId) {
    std::lock_guard lock(g_word_filter_mutex);
    if (!g_word_filter_title_ctxs.contains(titleCtxId)) {
        LOG_ERROR(Lib_NpUtility, "Invalid titleCtxId {}", titleCtxId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    for (auto it = g_word_filter_requests.begin(); it != g_word_filter_requests.end();) {
        if (it->second->titleCtxId == titleCtxId) {
            it->second->SetResult(ORBIS_NP_COMMUNITY_ERROR_ABORTED);
            it = g_word_filter_requests.erase(it);
        } else {
            ++it;
        }
    }
    g_word_filter_title_ctxs.erase(titleCtxId);
    LOG_INFO(Lib_NpUtility, "id={}", titleCtxId);
    return ORBIS_OK;
}

static s32 CreateWordFilterRequest(OrbisNpWordFilterTitleCtxId titleCtxId, bool isAsync) {
    std::lock_guard lock(g_word_filter_mutex);
    auto ctx_it = g_word_filter_title_ctxs.find(titleCtxId);
    if (ctx_it == g_word_filter_title_ctxs.end()) {
        LOG_ERROR(Lib_NpUtility, "Invalid titleCtxId {}", titleCtxId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    if (static_cast<s32>(g_word_filter_requests.size()) >= ORBIS_NP_WORD_FILTER_MAX_REQUEST_NUM) {
        LOG_ERROR(Lib_NpUtility, "too many requests ({})", g_word_filter_requests.size());
        return ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS;
    }
    const OrbisNpWordFilterRequestId id = g_word_filter_next_req_id++;
    auto req = std::make_shared<LookupRequestCtx>();
    req->titleCtxId = titleCtxId;
    req->isAsync = isAsync;
    req->timeouts = ctx_it->second.timeouts;
    g_word_filter_requests[id] = std::move(req);
    LOG_INFO(Lib_NpUtility, "id={} titleCtxId={} async={}", id, titleCtxId, isAsync);
    return id;
}

s32 PS4_SYSV_ABI sceNpWordFilterCreateRequest(OrbisNpWordFilterTitleCtxId titleCtxId) {
    return CreateWordFilterRequest(titleCtxId, /*isAsync=*/false);
}

s32 PS4_SYSV_ABI
sceNpWordFilterCreateAsyncRequest(OrbisNpWordFilterTitleCtxId titleCtxId,
                                  const OrbisNpWordFilterCreateAsyncRequestParameter* param) {
    if (param == nullptr || param->size != sizeof(OrbisNpWordFilterCreateAsyncRequestParameter)) {
        LOG_ERROR(Lib_NpUtility, "invalid async request parameter");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    return CreateWordFilterRequest(titleCtxId, /*isAsync=*/true);
}

static std::shared_ptr<LookupRequestCtx> FindWordFilterRequest(OrbisNpWordFilterRequestId reqId) {
    std::lock_guard lock(g_word_filter_mutex);
    auto it = g_word_filter_requests.find(reqId);
    return it != g_word_filter_requests.end() ? it->second : nullptr;
}

s32 PS4_SYSV_ABI sceNpWordFilterDeleteRequest(OrbisNpWordFilterRequestId reqId) {
    std::lock_guard lock(g_word_filter_mutex);
    auto it = g_word_filter_requests.find(reqId);
    if (it == g_word_filter_requests.end()) {
        LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    {
        std::lock_guard rlock(it->second->mutex);
        it->second->aborted = true;
    }
    it->second->SetResult(ORBIS_NP_COMMUNITY_ERROR_ABORTED);
    g_word_filter_requests.erase(it);
    LOG_INFO(Lib_NpUtility, "reqId={}", reqId);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpWordFilterAbortRequest(OrbisNpWordFilterRequestId reqId) {
    const auto req = FindWordFilterRequest(reqId);
    if (req == nullptr) {
        LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    {
        std::lock_guard rlock(req->mutex);
        req->aborted = true;
    }
    req->SetResult(ORBIS_NP_COMMUNITY_ERROR_ABORTED);
    LOG_INFO(Lib_NpUtility, "reqId={}", reqId);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpWordFilterSetTimeout(s32 id, s32 resolveRetry, u32 resolveTimeout,
                                           u32 connTimeout, u32 sendTimeout, u32 recvTimeout) {
    const LookupTimeouts timeouts{
        .resolveRetry = resolveRetry,
        .resolveTimeout = resolveTimeout,
        .connTimeout = connTimeout,
        .sendTimeout = sendTimeout,
        .recvTimeout = recvTimeout,
    };
    if (const s32 ret = ValidateCommunityTimeouts(timeouts); ret != ORBIS_OK) {
        return ret;
    }
    std::lock_guard lock(g_word_filter_mutex);
    if (auto ctx_it = g_word_filter_title_ctxs.find(id); ctx_it != g_word_filter_title_ctxs.end()) {
        ctx_it->second.timeouts = timeouts;
        return ORBIS_OK;
    }
    if (auto req_it = g_word_filter_requests.find(id); req_it != g_word_filter_requests.end()) {
        std::lock_guard rlock(req_it->second->mutex);
        req_it->second->timeouts = timeouts;
        return ORBIS_OK;
    }
    LOG_ERROR(Lib_NpUtility, "invalid id {}", id);
    return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
}

// Validates the comment arguments shared by censoring and sanitizing, then claims the request for
// its single transaction.
static s32 BeginWordFilterTransaction(OrbisNpWordFilterRequestId reqId, const char* comment,
                                      const void* option,
                                      std::shared_ptr<LookupRequestCtx>& out_req) {
    if (comment == nullptr) {
        LOG_ERROR(Lib_NpUtility, "comment is null");
        return ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    }
    if (option != nullptr) {
        LOG_ERROR(Lib_NpUtility, "option must be null");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    if (strnlen(comment, ORBIS_NP_WORD_FILTER_COMMENT_MAX_LENGTH + 1) >
        ORBIS_NP_WORD_FILTER_COMMENT_MAX_LENGTH) {
        LOG_ERROR(Lib_NpUtility, "comment longer than {} bytes",
                  ORBIS_NP_WORD_FILTER_COMMENT_MAX_LENGTH);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    auto req = FindWordFilterRequest(reqId);
    if (req == nullptr) {
        LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    std::lock_guard rlock(req->mutex);
    if (req->aborted) {
        return ORBIS_NP_COMMUNITY_ERROR_ABORTED;
    }
    if (req->used) {
        // libSceNpUtility refuses a second transaction on a request with this code.
        LOG_ERROR(Lib_NpUtility, "reqId={} already used", reqId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_TYPE;
    }
    req->used = true;
    out_req = std::move(req);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpWordFilterCensorComment(OrbisNpWordFilterRequestId reqId, const char* comment,
                                              void* option) {
    std::shared_ptr<LookupRequestCtx> req;
    if (const s32 ret = BeginWordFilterTransaction(reqId, comment, option, req); ret != ORBIS_OK) {
        return ret;
    }
    req->SetResult(ORBIS_OK);
    LOG_INFO(Lib_NpUtility, "reqId={} accepted {} bytes", reqId, strlen(comment));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpWordFilterSanitizeComment(OrbisNpWordFilterRequestId reqId,
                                                const char* comment, char* sanitizedComment,
                                                void* option) {
    if (sanitizedComment == nullptr) {
        LOG_ERROR(Lib_NpUtility, "sanitizedComment is null");
        return ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT;
    }
    std::shared_ptr<LookupRequestCtx> req;
    if (const s32 ret = BeginWordFilterTransaction(reqId, comment, option, req); ret != ORBIS_OK) {
        return ret;
    }
    const size_t length = strnlen(comment, ORBIS_NP_WORD_FILTER_COMMENT_MAX_LENGTH);
    std::memcpy(sanitizedComment, comment, length);
    sanitizedComment[length] = '\0';
    req->SetResult(ORBIS_OK);
    LOG_INFO(Lib_NpUtility, "reqId={} returned {} bytes unchanged", reqId, length);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpWordFilterPollAsync(OrbisNpWordFilterRequestId reqId, s32* result) {
    if (result == nullptr) {
        LOG_ERROR(Lib_NpUtility, "result is null");
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ARGUMENT;
    }
    const auto req = FindWordFilterRequest(reqId);
    if (req == nullptr || !req->isAsync) {
        LOG_ERROR(Lib_NpUtility, "Invalid async reqId {}", reqId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    std::lock_guard rlock(req->mutex);
    if (!req->result.has_value()) {
        return ORBIS_NP_LOOKUP_POLL_ASYNC_RET_RUNNING;
    }
    *result = *req->result;
    LOG_INFO(Lib_NpUtility, "reqId={} completed (result={:#x})", reqId,
             static_cast<u32>(*req->result));
    return ORBIS_NP_LOOKUP_POLL_ASYNC_RET_FINISHED;
}

s32 PS4_SYSV_ABI sceNpWordFilterWaitAsync(OrbisNpWordFilterRequestId reqId, s32* result) {
    const auto req = FindWordFilterRequest(reqId);
    if (req == nullptr) {
        LOG_ERROR(Lib_NpUtility, "Invalid reqId {}", reqId);
        return ORBIS_NP_COMMUNITY_ERROR_INVALID_ID;
    }
    const s32 r = req->Wait();
    if (result != nullptr) {
        *result = r;
    }
    LOG_INFO(Lib_NpUtility, "reqId={} completed (result={:#x})", reqId, static_cast<u32>(r));
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("Y797Sw9-jqY", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntAbortRequest);
    LIB_FUNCTION("UUhI+IUMrcE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckAvailability);
    LIB_FUNCTION("ASonnwltwEk", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckAvailabilityA);
    LIB_FUNCTION("jXx0+2Wd1q8", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckAvailabilityAll);
    LIB_FUNCTION("f1OwQ7jdqn0", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckAvailabilityAllA);
    LIB_FUNCTION("1mfDBl40Dms", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckServiceAvailability);
    LIB_FUNCTION("XAmDowAQhFs", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckServiceAvailabilityA);
    LIB_FUNCTION("BaihFa8LBw0", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckServiceAvailabilityAll);
    LIB_FUNCTION("JcqdKidhuK0", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCheckServiceAvailabilityAllA);
    LIB_FUNCTION("cXpyESo49ko", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntCreateRequest);
    LIB_FUNCTION("pRgpBtHx8P4", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppInfoIntDestroyRequest);
    LIB_FUNCTION("s9+zoKE8cBA", "libSceNpUtility", 1, "libSceNpUtility", sceNpAppInfoIntFinalize);
    LIB_FUNCTION("l6Dl+2zlua0", "libSceNpUtility", 1, "libSceNpUtility", sceNpAppInfoIntInitialize);
    LIB_FUNCTION("OHCO6MMFvdQ", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLink2IntAbortRequest);
    LIB_FUNCTION("B6IXdHGBL-g", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLink2IntCreateRequest);
    LIB_FUNCTION("0H0JBpVp03o", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLink2IntDestroyRequest);
    LIB_FUNCTION("FWonlDV6d5k", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLink2IntFinalize);
    LIB_FUNCTION("PdYx470F6B8", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLink2IntGetCompatibleTitleIdList);
    LIB_FUNCTION("tesM6ViaX6M", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLink2IntGetCompatibleTitleIdNum);
    LIB_FUNCTION("DK6xpBP1gxw", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLink2IntInitialize);
    LIB_FUNCTION("AQV4A8YFx44", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLinkIntAbortRequest);
    LIB_FUNCTION("9YhwG4DhwtU", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLinkIntCreateRequest);
    LIB_FUNCTION("-8Wn4YKZLMM", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLinkIntDestroyRequest);
    LIB_FUNCTION("TnQqJsyek5o", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLinkIntFinalize);
    LIB_FUNCTION("GB7Fhk5SUaA", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLinkIntGetCompatibleTitleIdList);
    LIB_FUNCTION("X4elOoiAtB4", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLinkIntGetCompatibleTitleIdNum);
    LIB_FUNCTION("1F4yweQoqgg", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpAppLaunchLinkIntInitialize);
    LIB_FUNCTION("kvdMF48mB3Y", "libSceNpUtility", 1, "libSceNpUtility", sceNpBandwidthTestAbort);
    LIB_FUNCTION("DWWW02MbKdk", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestDownloadOnlyInitStart);
    LIB_FUNCTION("BYIZGKm6bO4", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestGetStatus);
    LIB_FUNCTION("jktww3yJXnc", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestInitStart);
    LIB_FUNCTION("hqzi1IHdQQQ", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestInitStartDownload);
    LIB_FUNCTION("mA0zsbqm+kA", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestInitStartUpload);
    LIB_FUNCTION("pLr1fEQS1z8", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestShutdown);
    LIB_FUNCTION("tyArYWj+1QE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestShutdownWithDetailedInfo);
    LIB_FUNCTION("oXOyqxO8dX8", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpBandwidthTestUploadOnlyInitStart);
    LIB_FUNCTION("eYz4v5Uek9U", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupAbortRequest);
    LIB_FUNCTION("JA4+sS39GMs", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupCreateAsyncRequest);
    LIB_FUNCTION("iQr9UxPHUFs", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupCreateRequest);
    LIB_FUNCTION("8533Q+LU7EQ", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupCreateTitleCtx);
    LIB_FUNCTION("vT9xhqPO6+0", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupCreateTitleCtxA);
    LIB_FUNCTION("wLaxchvEEnk", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupDeleteRequest);
    LIB_FUNCTION("mtqDK9zkoIE", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupDeleteTitleCtx);
    LIB_FUNCTION("1O96muPzhgU", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetAbortRequest);
    LIB_FUNCTION("N0iF180VjGk", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetCensorComment);
    LIB_FUNCTION("UI5t6Rx6s5I", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetConvertJidToNpId);
    LIB_FUNCTION("ieROYX4vspk", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetConvertNpIdToJid);
    LIB_FUNCTION("KUIRsku7EPk", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetCreateRequest);
    LIB_FUNCTION("8DPEdJh9RkE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetCreateTitleCtx);
    LIB_FUNCTION("HL-venrRcnQ", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetDeleteRequest);
    LIB_FUNCTION("dxpUx7z9StY", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetDeleteTitleCtx);
    LIB_FUNCTION("zVZE+fAhgFY", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupNetInit);
    LIB_FUNCTION("DiUk6-mq--0", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetInitWithFunctionPointer);
    LIB_FUNCTION("cpnwZeVIq8E", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetInitWithMemoryPool);
    LIB_FUNCTION("ZXlTj9RRCFo", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupNetIsInit);
    LIB_FUNCTION("2nEVmFiV6OI", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupNetNpId);
    LIB_FUNCTION("jJH2P7KA4XU", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpLookupNetSanitizeComment);
    LIB_FUNCTION("NWtf77WCXJs", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupNetSetTimeout);
    LIB_FUNCTION("Dbd5BY0QjG0", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupNetTerm);
    LIB_FUNCTION("T6tnM1Uti4g", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupNpId);
    LIB_FUNCTION("V4EVrruHuy8", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupPollAsync);
    LIB_FUNCTION("0MV72WO7V34", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupSetTimeout);
    LIB_FUNCTION("YX9dAus6baE", "libSceNpUtility", 1, "libSceNpUtility", sceNpLookupWaitAsync);
    LIB_FUNCTION("Kq+ftR9LHlE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntAbortRequest);
    LIB_FUNCTION("IG1Kd+k6U3s", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntCreateRequest);
    LIB_FUNCTION("hBsBswrAiGM", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntDestroyRequest);
    LIB_FUNCTION("cvZrmlSlwn8", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntFinalize);
    LIB_FUNCTION("aUgLCb3pSOo", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntGetServiceAvailability);
    LIB_FUNCTION("Yp2yK5YXb78", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntGetServiceAvailabilityA);
    LIB_FUNCTION("-Afi-JoRZ-U", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntInitialize);
    LIB_FUNCTION("ukBq62OPAYA", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceChecker2IntIsSetServiceType);
    LIB_FUNCTION("waeEzwwYfZY", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntAbortRequest);
    LIB_FUNCTION("YLXt-vGw4Kg", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntCreateRequest);
    LIB_FUNCTION("85ZWdzWYgas", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntDestroyRequest);
    LIB_FUNCTION("LSQ3xApEoxY", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntFinalize);
    LIB_FUNCTION("wIX00Brskoc", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntGetAvailability);
    LIB_FUNCTION("MjOFdwXYRKY", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntGetAvailabilityList);
    LIB_FUNCTION("rT9Yk55JGho", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntInitialize);
    LIB_FUNCTION("az7fl9snOqw", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpServiceCheckerIntIsCached);
    LIB_FUNCTION("rei4kjOSiyc", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpTitleMetadataIntAbortRequest);
    LIB_FUNCTION("A1XQslLAA-Y", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpTitleMetadataIntCreateRequest);
    LIB_FUNCTION("tynva-9jrtI", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpTitleMetadataIntDeleteRequest);
    LIB_FUNCTION("-McDhX8tnWE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpTitleMetadataIntGetInfo);
    LIB_FUNCTION("LXHkrCV453o", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpTitleMetadataIntGetNpTitleId);
    LIB_FUNCTION("W6iWw8aUQtA", "libSceNpUtility", 1, "libSceNpUtility", sceNpUtilityInit);
    LIB_FUNCTION("M5Jyo9TKYPI", "libSceNpUtility", 1, "libSceNpUtility", sceNpUtilityTerm);
    LIB_FUNCTION("rAOOqDAxBIk", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterAbortRequest);
    LIB_FUNCTION("1dMndqL-QgE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterCensorComment);
    LIB_FUNCTION("IEB+vgVoQbw", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterCreateAsyncRequest);
    LIB_FUNCTION("iCq5xW5KQW4", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterCreateRequest);
    LIB_FUNCTION("r9BgI0PfJZg", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterCreateTitleCtx);
    LIB_FUNCTION("6p9jvljuvsw", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterCreateTitleCtxA);
    LIB_FUNCTION("PYFS1H70bDs", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterDeleteRequest);
    LIB_FUNCTION("t0P5z5yuFPA", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterDeleteTitleCtx);
    LIB_FUNCTION("ur5SShyG0dk", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterPollAsync);
    LIB_FUNCTION("Jj4mkpFO2gE", "libSceNpUtility", 1, "libSceNpUtility",
                 sceNpWordFilterSanitizeComment);
    LIB_FUNCTION("Fa4dVWgmffk", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterSetTimeout);
    LIB_FUNCTION("87ivWj5yKzg", "libSceNpUtility", 1, "libSceNpUtility", sceNpWordFilterWaitAsync);
    LIB_FUNCTION("8533Q+LU7EQ", "libSceNpUtilityCompat", 1, "libSceNpUtility",
                 sceNpLookupCreateTitleCtx);
    LIB_FUNCTION("T6tnM1Uti4g", "libSceNpUtilityCompat", 1, "libSceNpUtility", sceNpLookupNpId);
    LIB_FUNCTION("r9BgI0PfJZg", "libSceNpUtilityCompat", 1, "libSceNpUtility",
                 sceNpWordFilterCreateTitleCtx);
};

} // namespace Libraries::Np::NpUtility