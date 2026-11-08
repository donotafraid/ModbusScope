#pragma once
#include <cstdint>
#include <string>

namespace modbusscope {

struct TcpEndpoint {
    std::string host;          // 非空；空则 connect() 返回 InvalidArgument
    std::uint16_t port = 502;
};

// 配置快照。配置在 Client 生命周期内只读；不支持运行期热更新。
struct ClientConfig {
    TcpEndpoint endpoint;
    std::uint32_t connectTimeoutMs = 3000;
    std::uint32_t requestTimeoutMs = 1000;
    std::uint32_t recoveryIntervalMs = 500;   // Recovering 重试间隔
    std::uint32_t recoveryWindowMs = 5000;    // SLO 恢复窗口参考
};

} // namespace modbusscope