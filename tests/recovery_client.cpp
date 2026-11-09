#include "modbusscope/client.h"

#include <QCoreApplication>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

bool wait_ready(modbusscope::Client &client, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        auto state = client.state();
        if (state.is_success() &&
            *state.get() == modbusscope::LifeState::READY) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
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
    config.connectTimeoutMs = 2000;
    config.requestTimeoutMs = 500;
    config.recoveryIntervalMs = 100;
    config.recoveryWindowMs = 4000;

    auto created = modbusscope::Client::create(config);
    if (created.is_fail()) {
        std::cerr << "create failed: " << created.get_error()->context() << '\n';
        return 1;
    }
    auto client = std::move(*created.get());

    modbusscope::ReadRequest request;
    request.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    request.unitId = 1;
    request.startAddress = 0;
    request.quantity = 1;

    auto first = client->read(request);
    if (first.is_fail() || first.get()->registers !=
                               std::vector<std::uint16_t>{100}) {
        std::cerr << "first read failed\n";
        return 1;
    }
    std::cout << "first read ok\n";

    // The server closes the connection after the first reply.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto broken = client->read(request);
    std::cout << "read after drop: "
              << (broken.is_success() ? "watchdog already reconnected"
                                      : "transport failed as expected")
              << '\n';

    if (!wait_ready(*client, 5000)) {
        std::cerr << "watchdog did not recover\n";
        return 1;
    }
    std::cout << "watchdog recovered to READY\n";

    auto after = client->read(request);
    if (after.is_fail() || after.get()->registers !=
                               std::vector<std::uint16_t>{100}) {
        std::cerr << "read after recovery failed\n";
        return 1;
    }
    std::cout << "read after recovery ok\n";
    return 0;
}
