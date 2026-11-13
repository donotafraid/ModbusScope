#pragma once
#include <cstdint>
#include <string>

namespace modbusscope {

struct TcpEndpoint {
    std::string host; // must be non-empty; Client::create rejects an empty host
    std::uint16_t port = 502;
};

// Immutable configuration snapshot. Client::create copies it by value; there is
// no runtime hot reload, so a running call can never observe a half-updated
// config. The two operation budgets are clamped to 600000 ms per attempt (the
// same bound as the Qt response timer).
struct ClientConfig {
    TcpEndpoint endpoint;
    std::uint32_t connectTimeoutMs = 3000;
    std::uint32_t requestTimeoutMs = 1000;
    std::uint32_t recoveryIntervalMs = 500; // delay between RECOVERING attempts
    std::uint32_t recoveryWindowMs = 5000;  // SLO window before GIVEN_UP
};

} // namespace modbusscope