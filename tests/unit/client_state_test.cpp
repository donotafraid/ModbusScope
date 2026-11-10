#include "MockSdk.h"

#include "modbusscope/client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

modbusscope::ClientConfig config(std::uint32_t recoveryIntervalMs = 10000,
                                 std::uint32_t recoveryWindowMs = 20000) {
    modbusscope::ClientConfig value;
    value.endpoint.host = "127.0.0.1";
    value.endpoint.port = 1502;
    value.connectTimeoutMs = 50;
    value.requestTimeoutMs = 50;
    value.recoveryIntervalMs = recoveryIntervalMs;
    value.recoveryWindowMs = recoveryWindowMs;
    return value;
}

modbusscope::ReadRequest readRequest() {
    modbusscope::ReadRequest request;
    request.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    request.unitId = 1;
    request.startAddress = 0;
    request.quantity = 1;
    return request;
}

struct Fixture {
    std::unique_ptr<modbusscope::Client> client;
    modbusscope::test::MockSdk *mock = nullptr;
};

Fixture makeFixture(modbusscope::ClientConfig value = config()) {
    auto mock = std::make_unique<modbusscope::test::MockSdk>();
    auto *raw = mock.get();
    auto created = modbusscope::Client::createWithSdk(std::move(mock), value);
    EXPECT_TRUE(created.is_success());
    if (created.is_fail()) {
        return {};
    }
    return Fixture{std::move(*created.get()), raw};
}

} // namespace

TEST(ClientStateMachine, FirstTimeoutStaysReadySecondEntersRecovery) {
    auto fixture = makeFixture();
    ASSERT_TRUE(fixture.client);
    fixture.mock->setReadCode(modbusscope::ErrorCode::Timeout);

    auto first = fixture.client->read(readRequest());
    EXPECT_EQ(first.get_error()->code(), RichError::ErrorCode::TIMEOUT);
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::READY);

    auto second = fixture.client->read(readRequest());
    EXPECT_EQ(second.get_error()->code(), RichError::ErrorCode::TIMEOUT);
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::RECOVERING);
    EXPECT_GE(fixture.mock->disconnectCalls(), 1u);
}

TEST(ClientStateMachine, SuccessfulReadClearsTimeoutStreak) {
    auto fixture = makeFixture();
    ASSERT_TRUE(fixture.client);

    fixture.mock->setReadCode(modbusscope::ErrorCode::Timeout);
    EXPECT_TRUE(fixture.client->read(readRequest()).is_fail());
    fixture.mock->setReadCode(modbusscope::ErrorCode::Ok);
    EXPECT_TRUE(fixture.client->read(readRequest()).is_success());
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::READY);

    fixture.mock->setReadCode(modbusscope::ErrorCode::Timeout);
    EXPECT_EQ(fixture.client->read(readRequest()).get_error()->code(),
              RichError::ErrorCode::TIMEOUT);
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::READY);
}

TEST(ClientStateMachine, ConnectionFailureEntersRecoveryImmediately) {
    auto fixture = makeFixture();
    ASSERT_TRUE(fixture.client);
    fixture.mock->setReadCode(modbusscope::ErrorCode::ConnectionFailed);

    auto result = fixture.client->read(readRequest());
    EXPECT_EQ(result.get_error()->code(),
              RichError::ErrorCode::CONNECTION_FAILED);
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::RECOVERING);
}

TEST(ClientStateMachine, WatchdogReconnectsFromRecovery) {
    auto fixture = makeFixture();
    ASSERT_TRUE(fixture.client);
    fixture.mock->setReadCode(modbusscope::ErrorCode::ConnectionFailed);
    ASSERT_TRUE(fixture.client->read(readRequest()).is_fail());
    ASSERT_EQ(fixture.client->state(), modbusscope::LifeState::RECOVERING);

    fixture.mock->setReadCode(modbusscope::ErrorCode::Ok);
    fixture.mock->setConnectCode(modbusscope::ErrorCode::Ok);
    fixture.client->pumpWatchdogForTest();
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::READY);
}

TEST(ClientStateMachine, RecoveryWindowExpiryMovesToGivenUp) {
    auto fixture = makeFixture(config(5, 20));
    ASSERT_TRUE(fixture.client);
    // Keep reconnects failing so the watchdog measures the same bad window.
    fixture.mock->setConnectCode(modbusscope::ErrorCode::ConnectionFailed);
    fixture.mock->setReadCode(modbusscope::ErrorCode::ConnectionFailed);
    ASSERT_TRUE(fixture.client->read(readRequest()).is_fail());
    ASSERT_EQ(fixture.client->state(), modbusscope::LifeState::RECOVERING);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline &&
           fixture.client->state() != modbusscope::LifeState::GIVEN_UP) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::GIVEN_UP);
    EXPECT_EQ(fixture.client->read(readRequest()).get_error()->code(),
              RichError::ErrorCode::NOT_CONNECTED);
    EXPECT_EQ(fixture.client->connect().get_error()->code(),
              RichError::ErrorCode::ALREADY_TERMINATED);
}

TEST(ClientStateMachine, DisconnectStopsAutomaticRecoveryInIdle) {
    auto fixture = makeFixture();
    ASSERT_TRUE(fixture.client);
    // connect() failure leaves IDLE while recovery is enabled.
    fixture.mock->setConnectCode(modbusscope::ErrorCode::ConnectionFailed);
    ASSERT_TRUE(fixture.client->disconnect().is_success());
    ASSERT_EQ(fixture.client->state(), modbusscope::LifeState::IDLE);
    EXPECT_TRUE(fixture.client->connect().is_fail());
    ASSERT_EQ(fixture.client->state(), modbusscope::LifeState::IDLE);

    EXPECT_TRUE(fixture.client->disconnect().is_success());
    fixture.client->pumpWatchdogForTest();
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::IDLE);
}

TEST(ClientStateMachine, ShutdownWaitsForInFlightCall) {
    auto fixture = makeFixture();
    ASSERT_TRUE(fixture.client);
    fixture.mock->setReadDelayMs(80);

    std::atomic<bool> readFinished{false};
    std::thread caller([&] {
        const auto result = fixture.client->read(readRequest());
        EXPECT_TRUE(result.is_success());
        readFinished.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_TRUE(fixture.client->shutdown().is_success());
    caller.join();
    EXPECT_TRUE(readFinished.load(std::memory_order_acquire));
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::CLOSED);
    EXPECT_EQ(fixture.mock->stopCalls(), 1u);
}

TEST(ClientStateMachine, ManyCallersAreSerializedByTheClient) {
    auto fixture = makeFixture();
    ASSERT_TRUE(fixture.client);

    std::atomic<int> failures{0};
    std::vector<std::thread> callers;
    for (int thread = 0; thread < 4; ++thread) {
        callers.emplace_back([&] {
            for (int iteration = 0; iteration < 10; ++iteration) {
                const auto result = fixture.client->read(readRequest());
                if (result.is_fail()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &caller : callers) {
        caller.join();
    }
    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(fixture.mock->readCalls(), 40u);
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::READY);
}
