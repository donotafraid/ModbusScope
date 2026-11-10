// P2 regression probes for the public API and write-echo validation.
//
// Modes:
//   error_mapping          create() against a closed local port must report
//                          CONNECTION_FAILED, not CONNECT_TIMEOUT.
//   write_echo <host> <port>
//                          the server echoes the wrong address/count for
//                          0x06/0x10; both writes must fail as SERVICE_FAILED.
//   no_qcoreapp            covered by p1_regression.cpp; kept out of this file.
//
// Build: cmake --build build
// Run:   ./build/bin/modbus_client_p2_regression error_mapping
//        tests/run_p2_regression.sh build
#include "modbusscope/client.h"

#include <QCoreApplication>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int fail(const std::string &message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

std::uint16_t closed_local_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) !=
        0) {
        ::close(fd);
        return 0;
    }
    const std::uint16_t port = ntohs(address.sin_port);
    ::close(fd); // closing releases the port before the client connects
    return port;
}

modbusscope::ClientConfig make_config(const char *host, std::uint16_t port,
                                      std::uint32_t connectTimeoutMs,
                                      std::uint32_t requestTimeoutMs) {
    modbusscope::ClientConfig config;
    config.endpoint.host = host;
    config.endpoint.port = port;
    config.connectTimeoutMs = connectTimeoutMs;
    config.requestTimeoutMs = requestTimeoutMs;
    config.recoveryIntervalMs = 500;
    config.recoveryWindowMs = 2000;
    return config;
}

modbusscope::WriteRequest single_write() {
    modbusscope::WriteRequest request;
    request.function = modbusscope::FunctionCode::WriteSingleRegister;
    request.unitId = 1;
    request.startAddress = 1;
    request.quantity = 1;
    request.values = {555};
    return request;
}

modbusscope::WriteRequest multiple_write() {
    modbusscope::WriteRequest request;
    request.function = modbusscope::FunctionCode::WriteMultipleRegisters;
    request.unitId = 1;
    request.startAddress = 4;
    request.quantity = 2;
    request.values = {700, 701};
    return request;
}

int run_error_mapping() {
    const std::uint16_t port = closed_local_port();
    if (port == 0) {
        return fail("could not reserve a closed local port");
    }

    const auto start = std::chrono::steady_clock::now();
    auto created = modbusscope::Client::create(
        make_config("127.0.0.1", port, 500, 500));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (created.is_success()) {
        return fail("create() connected to a port that was just closed");
    }
    const RichError &error = *created.get_error();
    if (error.code() != RichError::ErrorCode::CONNECTION_FAILED) {
        return fail(std::string("refused connect reported as ") +
                    error.codeName() + ": " + error.context());
    }
    if (elapsed > 500 + 2000) {
        return fail("refused connect blocked for " + std::to_string(elapsed) +
                    " ms");
    }
    std::cout << "error_mapping: ok code=" << error.codeName()
              << " elapsed=" << elapsed << " ms\n";
    return 0;
}

int run_write_echo(const char *host, const char *portText) {
    const auto port = static_cast<std::uint16_t>(std::atoi(portText));
    auto created = modbusscope::Client::create(
        make_config(host, port, 1000, 1500));
    if (created.is_fail()) {
        return fail(std::string("create: ") + created.get_error()->context());
    }
    auto client = std::move(*created.get());

    auto single = client->write(single_write());
    if (single.is_success()) {
        return fail("0x06 with a wrong address echo was accepted");
    }
    if (single.get_error()->code() != RichError::ErrorCode::SERVICE_FAILED) {
        return fail(std::string("0x06 echo mismatch reported as ") +
                    single.get_error()->codeName());
    }

    auto multiple = client->write(multiple_write());
    if (multiple.is_success()) {
        return fail("0x10 with a wrong quantity echo was accepted");
    }
    if (multiple.get_error()->code() != RichError::ErrorCode::SERVICE_FAILED) {
        return fail(std::string("0x10 echo mismatch reported as ") +
                    multiple.get_error()->codeName());
    }

    std::cout << "write_echo: ok 0x06='" << single.get_error()->context()
              << "' 0x10='" << multiple.get_error()->context() << "'\n";
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " error_mapping | write_echo <host> <port>\n";
        return 2;
    }

    const std::string mode = argv[1];
    if (mode == "error_mapping") {
        return run_error_mapping();
    }
    if (mode == "write_echo") {
        if (argc < 4) {
            std::cerr << "usage: " << argv[0] << " write_echo <host> <port>\n";
            return 2;
        }
        return run_write_echo(argv[2], argv[3]);
    }
    std::cerr << "unknown mode: " << mode << '\n';
    return 2;
}
