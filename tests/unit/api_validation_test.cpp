#include "MockSdk.h"

#include "modbusscope/client.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace {

modbusscope::ClientConfig validConfig() {
    modbusscope::ClientConfig config;
    config.endpoint.host = "127.0.0.1";
    config.endpoint.port = 1502;
    config.connectTimeoutMs = 50;
    config.requestTimeoutMs = 50;
    config.recoveryIntervalMs = 10000;
    config.recoveryWindowMs = 20000;
    return config;
}

struct ClientUnderTest {
    std::unique_ptr<modbusscope::Client> client;
    modbusscope::test::MockSdk *mock = nullptr;
};

ClientUnderTest makeClient(modbusscope::ClientConfig config = validConfig()) {
    auto mock = std::make_unique<modbusscope::test::MockSdk>();
    auto *raw = mock.get();
    auto created = modbusscope::Client::createWithSdk(std::move(mock), config);
    EXPECT_TRUE(created.is_success());
    if (created.is_fail()) {
        return {};
    }
    return ClientUnderTest{std::move(*created.get()), raw};
}

modbusscope::ReadRequest readRequest() {
    modbusscope::ReadRequest request;
    request.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    request.unitId = 1;
    request.startAddress = 0;
    request.quantity = 1;
    return request;
}

modbusscope::WriteRequest writeRequest() {
    modbusscope::WriteRequest request;
    request.function = modbusscope::FunctionCode::WriteMultipleRegisters;
    request.unitId = 1;
    request.startAddress = 0;
    request.quantity = 1;
    request.values = {1};
    return request;
}

} // namespace

TEST(ConfigValidation, RejectsInvalidSnapshots) {
    auto config = validConfig();
    config.endpoint.host.clear();
    EXPECT_EQ(modbusscope::Client::create(config).get_error()->code(),
              RichError::ErrorCode::INVALID_CONFIG);

    config = validConfig();
    config.endpoint.port = 0;
    EXPECT_EQ(modbusscope::Client::create(config).get_error()->code(),
              RichError::ErrorCode::INVALID_CONFIG);

    config = validConfig();
    config.connectTimeoutMs = 9;
    EXPECT_EQ(modbusscope::Client::create(config).get_error()->code(),
              RichError::ErrorCode::INVALID_CONFIG);

    config = validConfig();
    config.requestTimeoutMs = 9;
    EXPECT_EQ(modbusscope::Client::create(config).get_error()->code(),
              RichError::ErrorCode::INVALID_CONFIG);

    config = validConfig();
    config.recoveryIntervalMs = 0;
    EXPECT_EQ(modbusscope::Client::create(config).get_error()->code(),
              RichError::ErrorCode::INVALID_CONFIG);

    config = validConfig();
    config.recoveryWindowMs = config.recoveryIntervalMs - 1;
    EXPECT_EQ(modbusscope::Client::create(config).get_error()->code(),
              RichError::ErrorCode::INVALID_CONFIG);
}

TEST(ConfigValidation, RequiresQCoreApplicationForRealTransport) {
    // This test binary never creates a QCoreApplication. RealSdk::start must
    // fail fast instead of blocking on a QEventLoop that can never run.
    auto created = modbusscope::Client::create(validConfig());
    ASSERT_TRUE(created.is_fail());
    EXPECT_EQ(created.get_error()->code(), RichError::ErrorCode::SDK_ERROR);
    EXPECT_NE(created.get_error()->context().find("QCoreApplication"),
              std::string::npos);
}

TEST(RequestValidation, ReadRejectsOutOfRangeFields) {
    auto fixture = makeClient();
    ASSERT_TRUE(fixture.client);

    auto request = readRequest();
    request.function = modbusscope::FunctionCode::WriteSingleRegister;
    EXPECT_EQ(fixture.client->read(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = readRequest();
    request.unitId = 0;
    EXPECT_EQ(fixture.client->read(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = readRequest();
    request.unitId = 248;
    EXPECT_EQ(fixture.client->read(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = readRequest();
    request.quantity = 0;
    EXPECT_EQ(fixture.client->read(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = readRequest();
    request.quantity = 126;
    EXPECT_EQ(fixture.client->read(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = readRequest();
    request.startAddress = 0xFFFF;
    request.quantity = 2;
    EXPECT_EQ(fixture.client->read(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    EXPECT_EQ(fixture.mock->readCalls(), 0u);
}

TEST(RequestValidation, WriteRejectsOutOfRangeFields) {
    auto fixture = makeClient();
    ASSERT_TRUE(fixture.client);

    auto request = writeRequest();
    request.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    EXPECT_EQ(fixture.client->write(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = writeRequest();
    request.values.clear();
    EXPECT_EQ(fixture.client->write(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = writeRequest();
    request.function = modbusscope::FunctionCode::WriteSingleRegister;
    request.quantity = 2;
    request.values = {1, 2};
    EXPECT_EQ(fixture.client->write(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = writeRequest();
    request.quantity = 0;
    request.values.clear();
    EXPECT_EQ(fixture.client->write(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = writeRequest();
    request.quantity = 124;
    request.values.assign(124, 1);
    EXPECT_EQ(fixture.client->write(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    request = writeRequest();
    request.unitId = 248;
    EXPECT_EQ(fixture.client->write(request).get_error()->code(),
              RichError::ErrorCode::INVALID_ARGUMENT);

    EXPECT_EQ(fixture.mock->writeCalls(), 0u);
}

TEST(RequestValidation, BoundaryValuesReachTheTransport) {
    auto fixture = makeClient();
    ASSERT_TRUE(fixture.client);

    auto request = readRequest();
    request.unitId = 247;
    request.quantity = 125;
    EXPECT_TRUE(fixture.client->read(request).is_success());
    EXPECT_EQ(fixture.mock->readCalls(), 1u);

    auto write = writeRequest();
    write.quantity = 123;
    write.values.assign(123, 7);
    EXPECT_TRUE(fixture.client->write(write).is_success());
    EXPECT_EQ(fixture.mock->writeCalls(), 1u);
}

TEST(ErrorMapping, TransportCodesMapToPublicRichError) {
    auto fixture = makeClient();
    ASSERT_TRUE(fixture.client);

    fixture.mock->setReadCode(modbusscope::ErrorCode::ProtocolError);
    EXPECT_EQ(fixture.client->read(readRequest()).get_error()->code(),
              RichError::ErrorCode::SERVICE_FAILED);

    fixture.mock->setReadCode(modbusscope::ErrorCode::ConnectionFailed);
    EXPECT_EQ(fixture.client->read(readRequest()).get_error()->code(),
              RichError::ErrorCode::CONNECTION_FAILED);
    EXPECT_TRUE(fixture.client->connect().is_success());

    fixture.mock->setReadCode(modbusscope::ErrorCode::Timeout);
    EXPECT_EQ(fixture.client->read(readRequest()).get_error()->code(),
              RichError::ErrorCode::TIMEOUT);

    fixture.mock->setWriteCode(modbusscope::ErrorCode::NotConnected);
    EXPECT_EQ(fixture.client->write(writeRequest()).get_error()->code(),
              RichError::ErrorCode::NOT_CONNECTED);

    auto second = makeClient();
    ASSERT_TRUE(second.client);
    second.mock->setWriteCode(modbusscope::ErrorCode::Closed);
    EXPECT_EQ(second.client->write(writeRequest()).get_error()->code(),
              RichError::ErrorCode::ALREADY_TERMINATED);
}

TEST(ClientState, DisconnectMakesReadFailNotConnected) {
    auto fixture = makeClient();
    ASSERT_TRUE(fixture.client);
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::READY);

    EXPECT_TRUE(fixture.client->disconnect().is_success());
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::IDLE);
    EXPECT_EQ(fixture.client->read(readRequest()).get_error()->code(),
              RichError::ErrorCode::NOT_CONNECTED);

    EXPECT_TRUE(fixture.client->connect().is_success());
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::READY);
}

TEST(ClientState, ShutdownIsIdempotentAndTerminal) {
    auto fixture = makeClient();
    ASSERT_TRUE(fixture.client);

    EXPECT_TRUE(fixture.client->shutdown().is_success());
    EXPECT_EQ(fixture.client->state(), modbusscope::LifeState::CLOSED);
    EXPECT_TRUE(fixture.client->shutdown().is_success());
    EXPECT_EQ(fixture.client->read(readRequest()).get_error()->code(),
              RichError::ErrorCode::ALREADY_TERMINATED);
    EXPECT_EQ(fixture.client->connect().get_error()->code(),
              RichError::ErrorCode::ALREADY_TERMINATED);
}

// The enum is part of the public API: a consumer may log it, put it in a
// ScopeChannelConfig, or use it to build a raw PDU. Its numeric value must be
// the Modbus function code, not an arbitrary enum index.
TEST(FunctionCode, MatchesTheModbusWireValues) {
    using modbusscope::FunctionCode;
    EXPECT_EQ(static_cast<std::uint8_t>(FunctionCode::ReadHoldingRegisters),
              0x03);
    EXPECT_EQ(static_cast<std::uint8_t>(FunctionCode::ReadInputRegisters), 0x04);
    EXPECT_EQ(static_cast<std::uint8_t>(FunctionCode::WriteSingleRegister),
              0x06);
    EXPECT_EQ(static_cast<std::uint8_t>(FunctionCode::WriteMultipleRegisters),
              0x10);
}
