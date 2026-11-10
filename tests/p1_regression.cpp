// P1 runtime regression probes for the QModbus wrapper.
//
// Modes:
//   timeout_budget <host> <port>  server delays every reply by 1200 ms;
//                                 connectTimeout=500, requestTimeout=5000.
//   half_open      <host> <port>  server accepts 0x03/0x04 but never replies;
//                                 two consecutive timeouts must leave READY.
//   no_qcoreapp                   create() without QCoreApplication must fail
//                                 fast instead of blocking forever.
//
// Build: cmake --build build
// Run:   tests/run_p1_regression.sh build
#include "modbusscope/client.h"

#include <QCoreApplication>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

int fail(const std::string &message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

std::uint16_t parse_port(const char *text) {
    return static_cast<std::uint16_t>(std::atoi(text));
}

modbusscope::ClientConfig make_config(const char *host, const char *port,
                                      std::uint32_t connectTimeoutMs,
                                      std::uint32_t requestTimeoutMs,
                                      std::uint32_t recoveryIntervalMs,
                                      std::uint32_t recoveryWindowMs) {
    modbusscope::ClientConfig config;
    config.endpoint.host = host;
    config.endpoint.port = parse_port(port);
    config.connectTimeoutMs = connectTimeoutMs;
    config.requestTimeoutMs = requestTimeoutMs;
    config.recoveryIntervalMs = recoveryIntervalMs;
    config.recoveryWindowMs = recoveryWindowMs;
    return config;
}

modbusscope::ReadRequest one_register() {
    modbusscope::ReadRequest request;
    request.function = modbusscope::FunctionCode::ReadHoldingRegisters;
    request.unitId = 1;
    request.startAddress = 0;
    request.quantity = 1;
    return request;
}

int run_timeout_budget(const char *host, const char *port) {
    auto created = modbusscope::Client::create(
        make_config(host, port, 500, 5000, 500, 5000));
    if (created.is_fail()) {
        return fail(std::string("create: ") + created.get_error()->context());
    }
    auto client = std::move(*created.get());

    const auto start = std::chrono::steady_clock::now();
    auto result = client->read(one_register());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

    if (result.is_fail()) {
        return fail("read failed although 1200 ms < requestTimeoutMs 5000: " +
                    result.get_error()->context());
    }
    if (elapsed < 1000) {
        return fail("read returned before the 1200 ms server delay: " +
                    std::to_string(elapsed) + " ms");
    }
    if (elapsed > 5000) {
        return fail("read exceeded requestTimeoutMs 5000: " +
                    std::to_string(elapsed) + " ms");
    }
    std::cout << "timeout_budget: ok elapsed=" << elapsed << " ms\n";
    return 0;
}

int run_half_open(const char *host, const char *port) {
    // recoveryIntervalMs is deliberately long so the state snapshot is taken
    // before the watchdog can reconnect to the still-listening blackhole.
    auto created = modbusscope::Client::create(
        make_config(host, port, 500, 200, 2000, 4000));
    if (created.is_fail()) {
        return fail(std::string("create: ") + created.get_error()->context());
    }
    auto client = std::move(*created.get());

    auto first = client->read(one_register());
    if (first.is_success()) {
        return fail("blackhole read #1 unexpectedly succeeded");
    }
    auto second = client->read(one_register());
    if (second.is_success()) {
        return fail("blackhole read #2 unexpectedly succeeded");
    }

    const auto state = client->state();
    if (state == modbusscope::LifeState::READY) {
        return fail("two consecutive timeouts left the client READY "
                    "(half-open connection not escalated)");
    }
    std::cout << "half_open: ok state=" << modbusscope::to_string(state)
              << '\n';
    return 0;
}

int run_no_qcoreapp() {
    modbusscope::ClientConfig config;
    config.endpoint.host = "127.0.0.1";
    config.endpoint.port = 1;
    config.connectTimeoutMs = 500;
    config.requestTimeoutMs = 500;

    auto created = modbusscope::Client::create(config);
    if (created.is_success()) {
        return fail("create() succeeded without QCoreApplication");
    }
    if (created.get_error()->code() != RichError::ErrorCode::SDK_ERROR) {
        return fail("create() failed with the wrong code: " +
                    std::string(created.get_error()->codeName()));
    }
    std::cout << "no_qcoreapp: ok context="
              << created.get_error()->context() << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " timeout_budget|half_open <host> <port>\n"
                  << "       " << argv[0] << " no_qcoreapp\n";
        return 2;
    }

    const std::string mode = argv[1];
    if (mode == "no_qcoreapp") {
        // Intentionally no QCoreApplication: this is the precondition probe.
        return run_no_qcoreapp();
    }

    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::cerr << "usage: " << argv[0] << " " << mode
                  << " <host> <port>\n";
        return 2;
    }
    if (mode == "timeout_budget") {
        return run_timeout_budget(argv[2], argv[3]);
    }
    if (mode == "half_open") {
        return run_half_open(argv[2], argv[3]);
    }
    std::cerr << "unknown mode: " << mode << '\n';
    return 2;
}
