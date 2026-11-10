#include "modbusscope/client.h"
#include "modbusscope/modbusscope.h"

#include <QCoreApplication>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *what) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    } else {
        std::cout << "ok: " << what << '\n';
    }
}

void print_error(const RichError &error) {
    std::cerr << "error " << error.codeName() << ": " << error.context() << '\n';
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <host> <port>\n";
        return 2;
    }

    modbusscope::ClientConfig config;
    config.endpoint.host = argv[1];
    config.endpoint.port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    config.connectTimeoutMs = 3000;
    config.requestTimeoutMs = 1500;
    config.recoveryIntervalMs = 200;
    config.recoveryWindowMs = 3000;

    auto created = modbusscope::Client::create(config);
    if (created.is_fail()) {
        print_error(*created.get_error());
        return 1;
    }
    std::unique_ptr<modbusscope::Client> client = std::move(*created.get());

    check(client->state() == modbusscope::LifeState::READY,
          "create() returns READY");

    modbusscope::ReadRequest read;
    read.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    read.unitId = 1;
    read.startAddress = 0;
    read.quantity = 4;

    auto first = client->read(read);
    if (first.is_fail()) {
        print_error(*first.get_error());
        return 1;
    }
    check(first.get()->registers ==
              std::vector<std::uint16_t>{100, 101, 102, 103},
          "read holding registers 0..3");

    read.function = modbusscope::FunctionCode::ReadInputRegisters;
    read.startAddress = 2;
    read.quantity = 3;
    auto input = client->read(read);
    check(input.is_success() &&
              input.get()->registers == std::vector<std::uint16_t>{202, 203, 204},
          "read input registers 2..4");

    modbusscope::WriteRequest write;
    write.function = modbusscope::FunctionCode::WriteSingleRegister;
    write.unitId = 1;
    write.startAddress = 1;
    write.quantity = 1;
    write.values = {555};
    auto single = client->write(write);
    check(single.is_success(), "write single register 0x06");

    read.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    read.startAddress = 0;
    read.quantity = 2;
    auto afterSingle = client->read(read);
    check(afterSingle.is_success() &&
              afterSingle.get()->registers == std::vector<std::uint16_t>{100, 555},
          "read back single write");

    write.function = modbusscope::FunctionCode::WriteMultipleRegisters;
    write.startAddress = 4;
    write.quantity = 2;
    write.values = {700, 701};
    auto multiple = client->write(write);
    check(multiple.is_success(), "write multiple registers 0x10");

    read.startAddress = 4;
    read.quantity = 2;
    auto afterMultiple = client->read(read);
    check(afterMultiple.is_success() &&
              afterMultiple.get()->registers == std::vector<std::uint16_t>{700, 701},
          "read back multiple write");

    read.startAddress = 0;
    read.quantity = 126;
    auto invalid = client->read(read);
    check(invalid.is_fail() &&
              invalid.get_error()->code() ==
                  RichError::ErrorCode::INVALID_ARGUMENT,
          "quantity 126 is rejected before the transport");

    auto disconnected = client->disconnect();
    check(disconnected.is_success(), "disconnect");
    check(client->state() == modbusscope::LifeState::IDLE,
          "state is IDLE after disconnect");

    auto reconnected = client->connect();
    check(reconnected.is_success(), "reconnect");
    read.startAddress = 0;
    read.quantity = 1;
    auto again = client->read(read);
    check(again.is_success() && again.get()->registers ==
                                   std::vector<std::uint16_t>{100},
          "read after reconnect");

    auto shutdown = client->shutdown();
    check(shutdown.is_success(), "shutdown");
    check(client->state() == modbusscope::LifeState::CLOSED,
          "state is CLOSED after shutdown");

    std::cout << (failures == 0 ? "SMOKE PASS" : "SMOKE FAIL") << '\n';
    return failures == 0 ? 0 : 1;
}
