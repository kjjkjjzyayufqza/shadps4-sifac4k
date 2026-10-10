// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Deterministic floating point for lockstep P2P titles.
//
// The setting is per-title on purpose: replacing every reciprocal estimate costs
// performance and only buys something for titles whose netplay advances an identical
// simulation on both peers. Gundam Versus and Maxi Boost ON enable it automatically so
// Intel and AMD peers do not desync; other titles stay off unless a profile asks for it,
// and switching titles cannot leave it on. An explicit per-title false still wins.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

class DeterministicFpProfileTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
               ("shadps4_deterministic_fp_settings_test_" + std::to_string(suffix));
        custom_configs = root / "custom_configs";
        fs::create_directories(custom_configs);

        Common::FS::SetUserPath(Common::FS::PathType::UserDir, root);
        Common::FS::SetUserPath(Common::FS::PathType::CustomConfigs, custom_configs);

        state = std::make_shared<EmulatorState>();
        EmulatorState::SetInstance(state);
        settings = std::make_shared<EmulatorSettingsImpl>();
        EmulatorSettingsImpl::SetInstance(settings);
    }

    void TearDown() override {
        EmulatorSettingsImpl::SetInstance(nullptr);
        EmulatorState::SetInstance(nullptr);
        settings.reset();
        state.reset();

        std::error_code error;
        fs::remove_all(root, error);
    }

    void WriteGameConfig(const std::string& serial, const json& config) const {
        std::ofstream output(custom_configs / (serial + ".json"));
        ASSERT_TRUE(output.is_open());
        output << config;
    }

    fs::path root;
    fs::path custom_configs;
    std::shared_ptr<EmulatorState> state;
    std::shared_ptr<EmulatorSettingsImpl> settings;
};

TEST(DeterministicFpSettingsTest, OffIsTheDefault) {
    EmulatorSettingsImpl settings;

    EXPECT_FALSE(settings.IsDeterministicFloatingPoint());
}

TEST(DeterministicFpSettingsTest, GameOverrideDoesNotChangeTheGlobalValue) {
    EmulatorSettingsImpl settings;
    settings.SetDeterministicFloatingPoint(true, true);

    EXPECT_TRUE(settings.IsDeterministicFloatingPoint());

    settings.SetConfigMode(ConfigMode::Global);
    EXPECT_FALSE(settings.IsDeterministicFloatingPoint());
}

TEST(DeterministicFpSettingsTest, JsonUsesThePerGameKey) {
    DeterministicFpSettings source;
    source.deterministic_floating_point.value = true;

    const nlohmann::json encoded = source;
    ASSERT_TRUE(encoded.contains("deterministic_floating_point"));

    const auto decoded = encoded.get<DeterministicFpSettings>();
    EXPECT_TRUE(decoded.deterministic_floating_point.value);
}

TEST_F(DeterministicFpProfileTest, TitleProfileEnablesIt) {
    json enabled;
    enabled["DeterministicFp"]["deterministic_floating_point"] = true;
    WriteGameConfig("CUSA15006", enabled);

    ASSERT_TRUE(settings->Load("CUSA15006"));
    EXPECT_TRUE(settings->IsDeterministicFloatingPoint());
}

TEST_F(DeterministicFpProfileTest, LockstepTitleEnablesItWithoutAProfile) {
    EXPECT_FALSE(settings->Load("CUSA08379"));
    EXPECT_TRUE(settings->IsDeterministicFloatingPoint());

    EXPECT_FALSE(settings->Load("CUSA15006"));
    EXPECT_TRUE(settings->IsDeterministicFloatingPoint());
}

TEST_F(DeterministicFpProfileTest, LockstepTitleEnablesItWhenProfileOmitsTheKey) {
    json unrelated;
    unrelated["General"]["neo_mode"] = true;
    WriteGameConfig("CUSA08379", unrelated);

    ASSERT_TRUE(settings->Load("CUSA08379"));
    EXPECT_TRUE(settings->IsDeterministicFloatingPoint());
}

TEST_F(DeterministicFpProfileTest, LockstepTitleRespectsExplicitOff) {
    json disabled;
    disabled["DeterministicFp"]["deterministic_floating_point"] = false;
    WriteGameConfig("CUSA08379", disabled);

    ASSERT_TRUE(settings->Load("CUSA08379"));
    EXPECT_FALSE(settings->IsDeterministicFloatingPoint());
}

TEST_F(DeterministicFpProfileTest, SwitchingTitlesCannotLeaveItOn) {
    json enabled;
    enabled["DeterministicFp"]["deterministic_floating_point"] = true;
    WriteGameConfig("CUSA15006", enabled);
    WriteGameConfig("CUSA00002", json::object());

    ASSERT_TRUE(settings->Load("CUSA15006"));
    ASSERT_TRUE(settings->IsDeterministicFloatingPoint());

    ASSERT_TRUE(settings->Load("CUSA00002"));
    EXPECT_FALSE(settings->IsDeterministicFloatingPoint())
        << "a title with no profile must not inherit the previous title's rewriting";
}

TEST_F(DeterministicFpProfileTest, MissingProfileClearsIt) {
    settings->SetDeterministicFloatingPoint(true, true);

    EXPECT_FALSE(settings->Load("CUSA00001"));
    EXPECT_FALSE(settings->IsDeterministicFloatingPoint());
}
