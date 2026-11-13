#include "MockSdk.h"

#include "modbusscope/client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace {

modbusscope::ClientConfig quickConfig() {
    modbusscope::ClientConfig config;
    config.endpoint.host = "127.0.0.1";
    config.endpoint.port = 1502;
    // Small operation budgets keep the shutdown drain deadline short. The
    // drain grace is max(request, connect) + 2 s + 1 s.
    config.connectTimeoutMs = 50;
    config.requestTimeoutMs = 50;
    config.recoveryIntervalMs = 10000;
    config.recoveryWindowMs = 20000;
    return config;
}

modbusscope::ReadRequest readRequest() {
    modbusscope::ReadRequest request;
    request.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    request.unitId = 1;
    request.startAddress = 0;
    request.quantity = 1;
    return request;
}

} // namespace

// Regression for the shutdown-timeout lifetime hole. Before the shared_ptr
// Impl refactor, ~Client() deleted the Impl after the drain expired while the
// reader was still inside ISdk::read; the deferred stop then touched freed
// state. The test holds a read inside the mock, lets the drain expire, destroys
// the Client, then releases the read.
TEST(ClientLifetime, DestroyClientWhileReadIsStillInsideSdk) {
    modbusscope::test::MockSdk::stopCount().store(0);
    auto mock = std::make_unique<modbusscope::test::MockSdk>();
    auto *raw = mock.get();
    auto created = modbusscope::Client::createWithSdk(std::move(mock),
                                                      quickConfig());
    ASSERT_TRUE(created.is_success());
    auto client = std::move(*created.get());
    ASSERT_TRUE(client);

    raw->blockNextRead();
    std::atomic<int> resultCode{-1};
    std::thread reader([&] {
        const auto result = client->read(readRequest());
        resultCode.store(result.is_fail()
                             ? static_cast<int>(result.get_error()->code())
                             : static_cast<int>(RichError::ErrorCode::OK),
                         std::memory_order_release);
    });

    ASSERT_TRUE(raw->waitReadEntered(std::chrono::milliseconds(2000)));
    const auto shutdown = client->shutdown();
    ASSERT_TRUE(shutdown.is_fail());
    EXPECT_EQ(shutdown.get_error()->code(),
              RichError::ErrorCode::IN_FLIGHT_TIMEOUT);
    EXPECT_EQ(client->state(), modbusscope::LifeState::CLOSED);

    // The Client wrapper goes away; only the reader's shared_ptr snapshot keeps
    // the Impl (and the MockSdk) alive.
    client.reset();
    raw->releaseBlockedRead();
    reader.join();

    EXPECT_EQ(resultCode.load(std::memory_order_acquire),
              static_cast<int>(RichError::ErrorCode::ALREADY_TERMINATED));
    // The last lease performs the deferred SDK stop exactly once.
    EXPECT_EQ(modbusscope::test::MockSdk::stopCount().load(
                  std::memory_order_relaxed),
              1u);
}

// A timeout is a public uint32_t. Adding the drain/grace values in 32 bits
// wraps for a value near UINT32_MAX: UINT32_MAX + 3000 becomes 2999 ms. The
// shutdown would then report IN_FLIGHT_TIMEOUT after ~3 s even though the
// configured request budget is ~49 days. The read is released after 3.5 s, so
// this test only passes when the grace computation clamps before adding.
TEST(ClientLifetime, HugeConfiguredTimeoutDoesNotWrapTheShutdownGrace) {
    auto config = quickConfig();
    config.requestTimeoutMs = std::numeric_limits<std::uint32_t>::max();
    auto mock = std::make_unique<modbusscope::test::MockSdk>();
    auto *raw = mock.get();
    auto created = modbusscope::Client::createWithSdk(std::move(mock), config);
    ASSERT_TRUE(created.is_success());
    auto client = std::move(*created.get());
    ASSERT_TRUE(client);

    raw->blockNextRead();
    std::thread reader([&] { (void)client->read(readRequest()); });
    ASSERT_TRUE(raw->waitReadEntered(std::chrono::milliseconds(2000)));

    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        raw->releaseBlockedRead();
    });
    const auto shutdown = client->shutdown();
    EXPECT_TRUE(shutdown.is_success());
    reader.join();
    releaser.join();
}

// shutdown() is documented as idempotent. After a timed-out drain, the last
// in-flight call performs the deferred stop; a later shutdown() must observe
// that terminal state rather than replay the historical timeout.
TEST(ClientLifetime, SecondShutdownSucceedsAfterDeferredStop) {
    auto mock = std::make_unique<modbusscope::test::MockSdk>();
    auto *raw = mock.get();
    auto created = modbusscope::Client::createWithSdk(std::move(mock),
                                                      quickConfig());
    ASSERT_TRUE(created.is_success());
    auto client = std::move(*created.get());
    ASSERT_TRUE(client);

    raw->blockNextRead();
    std::thread reader([&] { (void)client->read(readRequest()); });
    ASSERT_TRUE(raw->waitReadEntered(std::chrono::milliseconds(2000)));

    EXPECT_EQ(client->shutdown().get_error()->code(),
              RichError::ErrorCode::IN_FLIGHT_TIMEOUT);
    raw->releaseBlockedRead();
    reader.join();

    EXPECT_TRUE(client->shutdown().is_success());
    EXPECT_EQ(client->state(), modbusscope::LifeState::CLOSED);
}
