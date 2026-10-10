// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <numeric>
#include <vector>
#include <gtest/gtest.h>
#include "core/libraries/np/np_score/np_score_ctx.h"

using Libraries::Np::NpScore::ScoreGameDataXfer;

namespace {

// Neither sceNpScoreRecordGameData nor sceNpScoreGetGameData takes an offset, so a title moving a
// replay larger than its staging buffer relies on the library continuing where the last call
// stopped. These tests pin that behaviour without needing a game or a server: the state machine
// they exercise is the whole of it.

std::vector<u8> MakeBlob(size_t size, u8 seed = 0) {
    std::vector<u8> blob(size);
    for (size_t i = 0; i < size; ++i) {
        blob[i] = static_cast<u8>(seed + i);
    }
    return blob;
}

// Feeds `blob` through the upload accumulator in `chunk` sized pieces the way the API would, and
// returns what would be submitted once the last piece arrives.
std::vector<u8> AccumulateUpload(ScoreGameDataXfer& xfer, const std::vector<u8>& blob,
                                 size_t chunk) {
    const u64 total = blob.size();
    for (size_t off = 0; off < blob.size(); off += chunk) {
        const size_t n = std::min(chunk, blob.size() - off);
        if (!xfer.SameSendTarget(/*board=*/1, /*pc=*/0, /*score=*/100)) {
            xfer.BeginSend(1, 0, 100, total);
        }
        xfer.sendBuf.insert(xfer.sendBuf.end(), blob.begin() + off, blob.begin() + off + n);
        if (xfer.sendBuf.size() < total) {
            continue;
        }
        std::vector<u8> complete = std::move(xfer.sendBuf);
        xfer.EndSend();
        return complete;
    }
    return {};
}

} // namespace

TEST(NpScoreGameDataXfer, UploadAccumulatesEveryChunkBeforeSubmitting) {
    // The defect this replaces truncated a replay to its first chunk, so the case that matters is
    // a total that is a whole number of chunks larger than one.
    const std::vector<u8> blob = MakeBlob(3 * 4096);
    ScoreGameDataXfer xfer;
    const std::vector<u8> submitted = AccumulateUpload(xfer, blob, 4096);
    ASSERT_EQ(submitted.size(), blob.size());
    EXPECT_EQ(submitted, blob);
    EXPECT_FALSE(xfer.sending);
    EXPECT_TRUE(xfer.sendBuf.empty());
}

TEST(NpScoreGameDataXfer, UploadHandlesARaggedFinalChunk) {
    const std::vector<u8> blob = MakeBlob(4096 * 2 + 17);
    ScoreGameDataXfer xfer;
    const std::vector<u8> submitted = AccumulateUpload(xfer, blob, 4096);
    ASSERT_EQ(submitted.size(), blob.size());
    EXPECT_EQ(submitted, blob);
}

TEST(NpScoreGameDataXfer, UploadToADifferentTargetStartsOver) {
    ScoreGameDataXfer xfer;
    xfer.BeginSend(/*board=*/1, /*pc=*/0, /*score=*/100, /*total=*/64);
    xfer.sendBuf.resize(32, 0xAA);

    EXPECT_TRUE(xfer.SameSendTarget(1, 0, 100));
    // A different board, character or score is a different blob; the half-finished one must not
    // be prepended to it.
    EXPECT_FALSE(xfer.SameSendTarget(2, 0, 100));
    EXPECT_FALSE(xfer.SameSendTarget(1, 1, 100));
    EXPECT_FALSE(xfer.SameSendTarget(1, 0, 101));

    xfer.BeginSend(2, 0, 100, 64);
    EXPECT_TRUE(xfer.sendBuf.empty());
    EXPECT_EQ(xfer.sendTotal, 64u);
}

TEST(NpScoreGameDataXfer, DownloadHandsOutSuccessiveSlices) {
    const std::vector<u8> blob = MakeBlob(3 * 4096 + 5, 0x40);
    ScoreGameDataXfer xfer;
    xfer.BeginRecv(/*board=*/1, /*pc=*/0, "player-one");
    xfer.recvBuf = blob;
    xfer.recvOffset = 0;

    std::vector<u8> reassembled;
    std::vector<u8> staging(4096);
    while (!xfer.RecvExhausted()) {
        const u64 copied = xfer.ReadSlice(staging.data(), staging.size());
        ASSERT_GT(copied, 0u);
        reassembled.insert(reassembled.end(), staging.begin(), staging.begin() + copied);
    }
    // Byte-for-byte: the bug this covers returned the opening slice every time, which produced a
    // file of the right length made of the wrong bytes.
    EXPECT_EQ(reassembled, blob);
    EXPECT_EQ(xfer.recvOffset, blob.size());
}

TEST(NpScoreGameDataXfer, DownloadSliceLargerThanTheRemainderIsClamped) {
    ScoreGameDataXfer xfer;
    xfer.BeginRecv(1, 0, "player-one");
    xfer.recvBuf = MakeBlob(100);
    xfer.recvOffset = 0;

    std::vector<u8> staging(4096);
    EXPECT_EQ(xfer.ReadSlice(staging.data(), staging.size()), 100u);
    EXPECT_TRUE(xfer.RecvExhausted());
    EXPECT_EQ(xfer.ReadSlice(staging.data(), staging.size()), 0u);
}

TEST(NpScoreGameDataXfer, DownloadTargetIdentityDistinguishesBlobs) {
    ScoreGameDataXfer xfer;
    xfer.BeginRecv(/*board=*/1, /*pc=*/0, "player-one");
    EXPECT_TRUE(xfer.SameRecvTarget(1, 0, "player-one"));
    EXPECT_FALSE(xfer.SameRecvTarget(1, 0, "player-two"));
    EXPECT_FALSE(xfer.SameRecvTarget(2, 0, "player-one"));
    EXPECT_FALSE(xfer.SameRecvTarget(1, 1, "player-one"));

    xfer.EndRecv();
    EXPECT_FALSE(xfer.SameRecvTarget(1, 0, "player-one"));
    EXPECT_FALSE(xfer.receiving);
}

TEST(NpScoreGameDataXfer, ReadSliceToleratesANullDestination) {
    // A title may ask only for the total size by passing a zero-length buffer; the read position
    // must still be left alone rather than advanced past data nobody received.
    ScoreGameDataXfer xfer;
    xfer.BeginRecv(1, 0, "player-one");
    xfer.recvBuf = MakeBlob(64);
    xfer.recvOffset = 0;
    EXPECT_EQ(xfer.ReadSlice(nullptr, 0), 0u);
    EXPECT_EQ(xfer.recvOffset, 0u);
}
