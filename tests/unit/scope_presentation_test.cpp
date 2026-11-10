#include "modbusscope/scope.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

modbusscope::ScopeSnapshot snapshot(modbusscope::ScopeRunState run,
                                    modbusscope::LifeState transport,
                                    std::string error = {}) {
    modbusscope::ScopeSnapshot value;
    value.run = run;
    value.transport = transport;
    value.lastError = std::move(error);
    value.lastSampleMs = 1; // model has produced at least one sample
    return value;
}

} // namespace

TEST(ScopePresentation, StoppedStateStartsAndEdits) {
    const auto view = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Stopped,
                 modbusscope::LifeState::IDLE));
    EXPECT_STREQ(view.label, "Stopped");
    EXPECT_TRUE(view.canStart);
    EXPECT_TRUE(view.canClear);
    EXPECT_TRUE(view.canEditChannels);
    EXPECT_FALSE(view.canPause);
    EXPECT_FALSE(view.canResume);
    EXPECT_FALSE(view.showError);
}

TEST(ScopePresentation, PausedStateResumesOnly) {
    const auto view = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Paused,
                 modbusscope::LifeState::READY));
    EXPECT_STREQ(view.label, "Paused");
    EXPECT_TRUE(view.canResume);
    EXPECT_FALSE(view.canStart);
    EXPECT_FALSE(view.canPause);
}

TEST(ScopePresentation, FaultedStateOffersRetryAndShowsError) {
    const auto view = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Faulted,
                 modbusscope::LifeState::RECOVERING, "boom"));
    EXPECT_STREQ(view.label, "Fault");
    EXPECT_TRUE(view.canStart);
    EXPECT_TRUE(view.canPause);
    EXPECT_TRUE(view.showError);
}

TEST(ScopePresentation, RunningReadyIsLive) {
    const auto view = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Running,
                 modbusscope::LifeState::READY));
    EXPECT_STREQ(view.label, "Live");
    EXPECT_TRUE(view.canPause);
    EXPECT_FALSE(view.canStart);
    EXPECT_FALSE(view.showError);
}

TEST(ScopePresentation, RunningBeforeFirstSampleShowsStarting) {
    modbusscope::ScopeSnapshot value;
    value.run = modbusscope::ScopeRunState::Running;
    value.transport = modbusscope::LifeState::IDLE;
    const auto view = modbusscope::present(value);
    EXPECT_STREQ(view.label, "Starting");
    EXPECT_FALSE(view.showError);
}

TEST(ScopePresentation, RunningTransportStatesMapToDistinctLabels) {
    const auto connecting = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Running,
                 modbusscope::LifeState::CONNECTING));
    EXPECT_STREQ(connecting.label, "Connecting");
    EXPECT_TRUE(connecting.showError);

    const auto idle = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Running,
                 modbusscope::LifeState::IDLE));
    EXPECT_STREQ(idle.label, "Idle");
    EXPECT_TRUE(idle.showError);

    const auto recovering = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Running,
                 modbusscope::LifeState::RECOVERING));
    EXPECT_STREQ(recovering.label, "Recovering");
    EXPECT_TRUE(recovering.showError);

    const auto givenUp = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Running,
                 modbusscope::LifeState::GIVEN_UP));
    EXPECT_STREQ(givenUp.label, "Given up");
    EXPECT_TRUE(givenUp.showError);

    const auto closed = modbusscope::present(
        snapshot(modbusscope::ScopeRunState::Running,
                 modbusscope::LifeState::CLOSED));
    EXPECT_STREQ(closed.label, "Closed");
    EXPECT_TRUE(closed.canStart);
    EXPECT_TRUE(closed.showError);
}

TEST(ScopePresentation, ToStringCoversEveryRunState) {
    EXPECT_STREQ(modbusscope::to_string(modbusscope::ScopeRunState::Stopped),
                 "STOPPED");
    EXPECT_STREQ(modbusscope::to_string(modbusscope::ScopeRunState::Running),
                 "RUNNING");
    EXPECT_STREQ(modbusscope::to_string(modbusscope::ScopeRunState::Paused),
                 "PAUSED");
    EXPECT_STREQ(modbusscope::to_string(modbusscope::ScopeRunState::Faulted),
                 "FAULTED");
}
