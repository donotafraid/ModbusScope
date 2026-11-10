#include "FakeScopeSource.h"

#include "modbusscope/scope.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool waitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

modbusscope::ScopeChannelConfig channel(std::string name,
                                        std::uint16_t address = 0) {
    modbusscope::ScopeChannelConfig value;
    value.name = std::move(name);
    value.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    value.unitId = 1;
    value.address = address;
    return value;
}

struct ModelFixture {
    std::unique_ptr<modbusscope::ScopeModel> model;
    modbusscope::test::FakeScopeSource *source = nullptr;
};

ModelFixture makeModel(std::vector<double> values,
                       std::vector<modbusscope::ScopeChannelConfig> channels,
                       std::uint32_t intervalMs = 5,
                       std::size_t capacity = 100) {
    auto source = std::make_unique<modbusscope::test::FakeScopeSource>();
    auto *raw = source.get();
    raw->setValues(std::move(values));
    auto created = modbusscope::ScopeModel::createWithSource(
        std::move(source), std::move(channels), intervalMs, capacity);
    EXPECT_TRUE(created.is_success());
    if (created.is_fail()) {
        return {};
    }
    return ModelFixture{std::move(*created.get()), raw};
}

} // namespace

TEST(ScopeModel, RejectsInvalidChannelConfiguration) {
    auto source = std::make_unique<modbusscope::test::FakeScopeSource>();

    auto empty = modbusscope::ScopeModel::createWithSource(
        std::move(source), {}, 5, 10);
    ASSERT_TRUE(empty.is_fail());
    EXPECT_EQ(empty.get_error()->code(), RichError::ErrorCode::INVALID_ARGUMENT);

    auto unitId = modbusscope::ScopeModel::createWithSource(
        std::make_unique<modbusscope::test::FakeScopeSource>(),
        {channel("a")}, 5, 10);
    ASSERT_TRUE(unitId.is_success());

    auto badUnit = channel("bad");
    badUnit.unitId = 0;
    auto rejected = modbusscope::ScopeModel::createWithSource(
        std::make_unique<modbusscope::test::FakeScopeSource>(),
        {badUnit}, 5, 10);
    ASSERT_TRUE(rejected.is_fail());
    EXPECT_EQ(rejected.get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    auto badFunction = channel("bad");
    badFunction.function = modbusscope::FunctionCode::WriteSingleRegister;
    auto functionRejected = modbusscope::ScopeModel::createWithSource(
        std::make_unique<modbusscope::test::FakeScopeSource>(),
        {badFunction}, 5, 10);
    ASSERT_TRUE(functionRejected.is_fail());

    auto badInterval = modbusscope::ScopeModel::createWithSource(
        std::make_unique<modbusscope::test::FakeScopeSource>(),
        {channel("a")}, 0, 10);
    ASSERT_TRUE(badInterval.is_fail());

    auto badCapacity = modbusscope::ScopeModel::createWithSource(
        std::make_unique<modbusscope::test::FakeScopeSource>(),
        {channel("a")}, 5, 0);
    ASSERT_TRUE(badCapacity.is_fail());
}

TEST(ScopeModel, PollsChannelsAndPublishesStatistics) {
    auto fixture = makeModel({10.0, 20.0}, {channel("a"), channel("b", 1)});
    ASSERT_TRUE(fixture.model);
    ASSERT_TRUE(fixture.model->start().is_success());

    ASSERT_TRUE(waitUntil([&] {
        const auto snapshot = fixture.model->snapshot();
        return !snapshot.channels.empty() &&
               snapshot.channels[0].samples.size() >= 3;
    }));
    const auto snapshot = fixture.model->snapshot();
    EXPECT_EQ(snapshot.run, modbusscope::ScopeRunState::Running);
    EXPECT_EQ(snapshot.transport, modbusscope::LifeState::READY);
    EXPECT_EQ(snapshot.channels.size(), 2u);
    EXPECT_DOUBLE_EQ(snapshot.channels[0].latest, 10.0);
    EXPECT_DOUBLE_EQ(snapshot.channels[1].latest, 20.0);
    EXPECT_DOUBLE_EQ(snapshot.channels[0].minimum, 10.0);
    EXPECT_DOUBLE_EQ(snapshot.channels[0].maximum, 10.0);
    EXPECT_TRUE(snapshot.channels[0].hasValue);
    EXPECT_GT(snapshot.sequence, 0u);
    EXPECT_TRUE(fixture.model->stop().is_success());
    EXPECT_TRUE(fixture.source->stopped());
}

TEST(ScopeModel, RingBufferRespectsCapacity) {
    auto fixture = makeModel({1.0}, {channel("a")}, 1, 3);
    ASSERT_TRUE(fixture.model);
    ASSERT_TRUE(fixture.model->start().is_success());
    ASSERT_TRUE(waitUntil([&] { return fixture.source->polls() >= 10; }));
    std::this_thread::sleep_for(10ms);
    const auto snapshot = fixture.model->snapshot();
    ASSERT_EQ(snapshot.channels.size(), 1u);
    EXPECT_LE(snapshot.channels[0].samples.size(), 3u);
    EXPECT_GT(snapshot.channels[0].samples.size(), 0u);
    EXPECT_TRUE(fixture.model->stop().is_success());
}

TEST(ScopeModel, PauseStopsPublishingUntilResume) {
    auto fixture = makeModel({1.0}, {channel("a")});
    ASSERT_TRUE(fixture.model);
    ASSERT_TRUE(fixture.model->start().is_success());
    ASSERT_TRUE(waitUntil([&] {
        return fixture.model->snapshot().channels[0].samples.size() >= 2;
    }));

    ASSERT_TRUE(fixture.model->pause().is_success());
    EXPECT_EQ(fixture.model->run_state(),
              modbusscope::ScopeRunState::Paused);
    std::this_thread::sleep_for(30ms);
    const std::uint64_t sequence = fixture.model->snapshot().sequence;
    std::this_thread::sleep_for(40ms);
    EXPECT_EQ(fixture.model->snapshot().sequence, sequence);

    ASSERT_TRUE(fixture.model->resume().is_success());
    EXPECT_EQ(fixture.model->run_state(),
              modbusscope::ScopeRunState::Running);
    EXPECT_TRUE(waitUntil([&] {
        return fixture.model->snapshot().sequence > sequence;
    }));
    EXPECT_TRUE(fixture.model->stop().is_success());
}

TEST(ScopeModel, FailurePublishesFaultAndSuccessRecovers) {
    auto fixture = makeModel({1.0}, {channel("a")});
    ASSERT_TRUE(fixture.model);
    ASSERT_TRUE(fixture.model->start().is_success());
    ASSERT_TRUE(waitUntil([&] {
        return fixture.model->run_state() ==
               modbusscope::ScopeRunState::Running;
    }));

    fixture.source->setFailure(true, "injected poll failure");
    ASSERT_TRUE(waitUntil([&] {
        return fixture.model->run_state() ==
               modbusscope::ScopeRunState::Faulted;
    }));
    EXPECT_NE(fixture.model->snapshot().lastError.find("injected"),
              std::string::npos);

    fixture.source->setFailure(false);
    ASSERT_TRUE(waitUntil([&] {
        return fixture.model->run_state() ==
               modbusscope::ScopeRunState::Running;
    }));
    EXPECT_TRUE(fixture.model->snapshot().lastError.empty());
    EXPECT_TRUE(fixture.model->stop().is_success());
}

TEST(ScopeModel, ClearDropsSamplesButKeepsRunState) {
    auto fixture = makeModel({5.0}, {channel("a")});
    ASSERT_TRUE(fixture.model);
    ASSERT_TRUE(fixture.model->start().is_success());
    ASSERT_TRUE(waitUntil([&] {
        return fixture.model->snapshot().channels[0].samples.size() >= 2;
    }));
    ASSERT_TRUE(fixture.model->pause().is_success());
    std::this_thread::sleep_for(20ms);

    ASSERT_TRUE(fixture.model->clear().is_success());
    const auto snapshot = fixture.model->snapshot();
    EXPECT_TRUE(snapshot.channels[0].samples.empty());
    EXPECT_FALSE(snapshot.channels[0].hasValue);
    EXPECT_EQ(snapshot.lastSampleMs, 0);
    EXPECT_EQ(snapshot.run, modbusscope::ScopeRunState::Paused);
    EXPECT_TRUE(fixture.model->stop().is_success());
}

TEST(ScopeModel, StopAndRestartReusesTheSource) {
    auto fixture = makeModel({2.0}, {channel("a")});
    ASSERT_TRUE(fixture.model);
    ASSERT_TRUE(fixture.model->start().is_success());
    ASSERT_TRUE(waitUntil([&] { return fixture.source->polls() > 0; }));

    ASSERT_TRUE(fixture.model->stop().is_success());
    EXPECT_EQ(fixture.model->run_state(),
              modbusscope::ScopeRunState::Stopped);
    EXPECT_TRUE(fixture.source->stopped());

    const int pollsBeforeRestart = fixture.source->polls();
    ASSERT_TRUE(fixture.model->start().is_success());
    EXPECT_TRUE(waitUntil([&] {
        return fixture.source->polls() > pollsBeforeRestart;
    }));
    EXPECT_TRUE(fixture.model->stop().is_success());
}

TEST(ScopeModel, PauseResumeAndClearRejectInvalidStates) {
    auto fixture = makeModel({1.0}, {channel("a")});
    ASSERT_TRUE(fixture.model);
    EXPECT_EQ(fixture.model->pause().get_error()->code(),
              RichError::ErrorCode::INVALID_STATE);
    EXPECT_EQ(fixture.model->resume().get_error()->code(),
              RichError::ErrorCode::INVALID_STATE);
    EXPECT_TRUE(fixture.model->clear().is_success());
}
