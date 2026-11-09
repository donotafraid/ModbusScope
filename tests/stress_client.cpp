#include "modbusscope/client.h"

#include <QCoreApplication>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " <host> <port> <threads>\n";
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
        std::cerr << "create failed: " << created.get_error()->context() << '\n';
        return 1;
    }
    auto client = std::move(*created.get());

    const int threadCount = std::max(1, std::atoi(argv[3]));
    const int iterations = 50;
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(threadCount));
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < iterations; ++i) {
                modbusscope::ReadRequest request;
                request.function = (i % 2 == 0)
                                       ? modbusscope::FunctionCode::ReadHoldingRegisters
                                       : modbusscope::FunctionCode::ReadInputRegisters;
                request.unitId = 1;
                request.startAddress = static_cast<std::uint16_t>((t + i) % 8);
                request.quantity = 4;
                auto result = client->read(request);
                if (result.is_fail()) {
                    ++failures;
                    std::cerr << "read failed: "
                              << result.get_error()->context() << '\n';
                    continue;
                }
                if (result.get()->registers.size() != 4) {
                    ++failures;
                }
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    auto shutdown = client->shutdown();
    if (shutdown.is_fail()) {
        ++failures;
    }
    std::cout << "threads=" << threadCount << " failures=" << failures.load()
              << '\n';
    return failures.load() == 0 ? 0 : 1;
}
